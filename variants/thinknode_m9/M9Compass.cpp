// ThinkNode M9 QMC6309 magnetometer driver — see M9Compass.h.
#if defined(HAS_M9_COMPASS) && defined(ESP32)

#include "M9Compass.h"
#include <Arduino.h>
#include <Wire.h>

// Bring-up aid: log the raw field vector once a second so the sensor's axis
// orientation on this board can be derived from readings at known headings
// (it is not documented anywhere, and Meshtastic's M9 driver never verified
// its own guess). Set to 0 once the mapping is baked into the default.
#ifndef M9_COMPASS_DEBUG
  #define M9_COMPASS_DEBUG 0
#endif

namespace {

constexpr uint8_t kAddr      = 0x7C;   // the only address the part offers
constexpr uint8_t kRegChipId = 0x00;
constexpr uint8_t kRegData   = 0x01;   // X LSB .. Z MSB, 6 bytes, auto-increment
constexpr uint8_t kRegStatus = 0x09;
constexpr uint8_t kRegCtrl1  = 0x0A;
constexpr uint8_t kRegCtrl2  = 0x0B;
constexpr uint8_t kChipId    = 0x90;

constexpr uint8_t kStatDrdy  = 0x01;
constexpr uint8_t kStatOvfl  = 0x02;

// CTRL2 = ODR 100 Hz (0b011<<4) | range ±32 G (0b00<<2) | set/reset on (0b00).
// 100 Hz so a consumer polling at 10 Hz always finds DRDY set and the
// low-pass below adds little lag (4 samples = 40 ms). ±32 G rather
// than ±8 G: Earth's field is only 0.25..0.65 G, but Meshtastic's M9 driver
// hard-codes calibration extrema around -6..-7.6 G per axis — if that
// on-board hard-iron bias is real it sits right at the ±8 G rail, and a
// saturated axis is worse than a coarser one. At 1000 LSB/G the resolution is
// still 1 mG (≈0.13° of heading in a 0.45 G horizontal field); the sensor's
// own noise floor (~2.5 mG at OSR 8) dominates either way.
constexpr uint8_t kCtrl2 = 0x30;
// CTRL1 = OSR2 (low-pass depth) 4 (0b010<<5) | OSR1 8 (0b00<<3) | normal mode
// (0b01). OSR1 8 is the low-noise oversampling; the low-pass depth is the
// heading's group delay, and 8 deep at 50 Hz (the datasheet's 0x61 example)
// read as sluggish on the dial — 4 deep at 100 Hz keeps the noise figure
// close (madflight measured 1.4 LSB σ at depth 16 vs 6.7 at depth 1) with a
// quarter of the lag. Normal mode honours the ODR (≈1 mA at 100 Hz/OSR 8);
// continuous mode free-runs at the maximum rate and is not needed here.
constexpr uint8_t kCtrl1 = 0x41;
// CTRL1 with MODE = 00: suspend. Registers keep their values, so waking is a
// single write of kCtrl1 — no reconfiguration.
constexpr uint8_t kModeSuspend = 0x40;

constexpr float    kGaussPerLsb   = 1.0f / 1000.0f;   // ±32 G range
constexpr uint32_t kOvflLogEvery  = 10000;            // ms between overflow log lines
constexpr uint32_t kSampleMaxAge  = 1000;             // ms a cached sample stays valid
constexpr uint32_t kIdleSuspendMs = 2000;            // no reads for this long -> suspend the chip
constexpr uint32_t kReprobeEvery  = 2000;             // ms between probes while absent
constexpr int      kMaxBusErrors  = 8;                // consecutive, before re-probing

TwoWire* s_bus       = nullptr;
bool     s_present   = false;
uint32_t s_next_probe_ms = 0;
int      s_errors    = 0;

float    s_x = 0, s_y = 0, s_z = 0;
uint32_t s_sample_ms = 0;
bool     s_have_sample = false;
bool     s_ovfl = false;
uint32_t s_ovfl_log_ms = 0;
// The part measures continuously in normal mode (~1 mA at 100 Hz / OSR 8) —
// worth having while an app is reading the compass, pure waste the rest of the
// time, which is nearly always. So it is parked in suspend and woken on demand.
bool     s_awake = false;
uint32_t s_last_read_ms = 0;

bool writeReg(uint8_t reg, uint8_t val) {
  s_bus->beginTransmission(kAddr);
  s_bus->write(reg);
  s_bus->write(val);
  return s_bus->endTransmission() == 0;
}

// Register read with a repeated start between the address write and the read
// (the keyboard driver uses a full STOP because its controller wants one; the
// QMC6309 is a plain register-addressed part and takes either).
bool readRegs(uint8_t reg, uint8_t* out, uint8_t n) {
  s_bus->beginTransmission(kAddr);
  s_bus->write(reg);
  if (s_bus->endTransmission(false) != 0) return false;
  if (s_bus->requestFrom((int)kAddr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) out[i] = (uint8_t)s_bus->read();
  return true;
}

bool configure() {
  // Soft reset: the bit is NOT self-clearing, the datasheet requires the
  // explicit 0x00 write afterwards. Reset restores every register to its POR
  // value (suspend mode).
  if (!writeReg(kRegCtrl2, 0x80)) return false;
  if (!writeReg(kRegCtrl2, 0x00)) return false;
  delay(10);
  if (!writeReg(kRegCtrl2, kCtrl2)) return false;
  if (!writeReg(kRegCtrl1, kCtrl1)) return false;
  s_awake = true;
  // Read back: one third-party driver (madflight) saw configuration writes not
  // stick right after power-up and retries — do the same once rather than
  // trusting the ACK.
  uint8_t c1 = 0, c2 = 0;
  if (!readRegs(kRegCtrl1, &c1, 1) || !readRegs(kRegCtrl2, &c2, 1)) return false;
  if (c1 != kCtrl1 || c2 != kCtrl2) {
    delay(5);
    if (!writeReg(kRegCtrl2, kCtrl2) || !writeReg(kRegCtrl1, kCtrl1)) return false;
    if (!readRegs(kRegCtrl1, &c1, 1) || !readRegs(kRegCtrl2, &c2, 1)) return false;
    if (c1 != kCtrl1 || c2 != kCtrl2) return false;
  }
  return true;
}

// One probe attempt. Distinguishes "nothing answered" (rail not up yet, or no
// chip) from "answered with a foreign id" in the boot log, since both read as
// a dead compass from the app's side.
bool probe(bool log) {
  uint8_t id = 0;
  if (!readRegs(kRegChipId, &id, 1)) {
    if (log) Serial.println("M9 compass: no answer at 0x7C (QMC6309 absent or rail not up)");
    return false;
  }
  if (id != kChipId) {
    if (log) Serial.printf("M9 compass: unexpected chip id 0x%02X at 0x7C (want 0x90)\n", id);
    return false;
  }
  if (!configure()) {
    if (log) Serial.println("M9 compass: QMC6309 found but configuration did not stick");
    return false;
  }
  if (log) Serial.println("M9 compass: QMC6309 ok (id=0x90, 100 Hz, +/-32 G, OSR 8, LPF 4)");
  s_errors = 0;
  s_have_sample = false;
  // Nothing is reading it yet: park it rather than burn ~1 mA from boot to the
  // first app that asks. m9CompassRead() wakes it.
  if (writeReg(kRegCtrl1, kModeSuspend)) s_awake = false;
  return true;
}

}  // namespace

void m9CompassBegin(TwoWire& w) {
  s_bus = &w;
  s_present = probe(true);
  s_next_probe_ms = millis() + kReprobeEvery;
}

bool m9CompassPresent() { return s_present; }

bool m9CompassRead(float* x, float* y, float* z, bool* overflow) {
  if (overflow) *overflow = false;
  if (!s_bus) return false;
  const uint32_t now = millis();

  if (!s_present) {
    // Rail-powered parts can still be coming out of POR when radio_init()
    // runs, so keep trying — quietly, one NACKed transaction every 2 s at most,
    // and only while something actually asks for the compass.
    if ((int32_t)(now - s_next_probe_ms) < 0) return false;
    s_next_probe_ms = now + kReprobeEvery;
    s_present = probe(false);
    if (!s_present) return false;
  }

  s_last_read_ms = now;
  if (!s_awake) {
    // Waking costs one register write; the first conversion lands a sample
    // period later, so this call reports "nothing fresh" and the caller's next
    // poll gets real data. Callers already handle a miss (the chip may be
    // absent), so this needs no special case at the other end.
    if (!writeReg(kRegCtrl1, kCtrl1)) return false;
    s_awake = true;
    return false;
  }

  uint8_t st = 0;
  bool ok = readRegs(kRegStatus, &st, 1);
  if (ok && (st & kStatDrdy)) {
    uint8_t b[6];
    ok = readRegs(kRegData, b, 6);
    if (ok) {
      const int16_t rx = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
      const int16_t ry = (int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8));
      const int16_t rz = (int16_t)((uint16_t)b[4] | ((uint16_t)b[5] << 8));
      s_x = rx * kGaussPerLsb;
      s_y = ry * kGaussPerLsb;
      s_z = rz * kGaussPerLsb;
      s_sample_ms = now;
      s_have_sample = true;
      // Overflow is kept, flagged, and logged (rate-limited) rather than
      // dropped: silently discarding it would make a board with a huge
      // hard-iron bias or a magnet nearby look exactly like a missing chip.
      s_ovfl = (st & kStatOvfl) != 0;
#if M9_COMPASS_DEBUG
      {
        static uint32_t last = 0;
        if (now - last >= 1000) {
          last = now;
          Serial.printf("[MAG] x=%.3f y=%.3f z=%.3f G%s\n", (double)s_x, (double)s_y, (double)s_z,
                        s_ovfl ? " OVFL" : "");
        }
      }
#endif
      if (s_ovfl && (s_ovfl_log_ms == 0 || (now - s_ovfl_log_ms) > kOvflLogEvery)) {
        s_ovfl_log_ms = now;
        Serial.printf("M9 compass: OVFL raw=%d,%d,%d (axis beyond +/-32000 counts at +/-32 G)\n",
                      (int)rx, (int)ry, (int)rz);
      }
    }
  }

  if (!ok) {
    // A burst of bus errors means the chip dropped off (rail cycled, bus
    // wedged): forget it and let the probe path bring it back configured.
    if (++s_errors >= kMaxBusErrors) {
      s_present = false;
      s_have_sample = false;
      s_next_probe_ms = now + kReprobeEvery;
      Serial.println("M9 compass: lost the QMC6309 (bus errors), will re-probe");
    }
    return false;
  }
  s_errors = 0;

  if (!s_have_sample || (now - s_sample_ms) > kSampleMaxAge) return false;
  if (x) *x = s_x;
  if (y) *y = s_y;
  if (z) *z = s_z;
  if (overflow) *overflow = s_ovfl;
  return true;
}

void m9CompassIdleTick() {
  if (!s_bus || !s_present || !s_awake) return;
  const uint32_t now = millis();
  if ((now - s_last_read_ms) < kIdleSuspendMs) return;
  if (writeReg(kRegCtrl1, kModeSuspend)) {
    s_awake = false;
    s_have_sample = false;   // whatever is cached is stale by the time we wake
  }
}

#endif  // HAS_M9_COMPASS && ESP32
