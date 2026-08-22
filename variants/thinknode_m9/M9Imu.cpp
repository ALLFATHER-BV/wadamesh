// ThinkNode M9 QMI8658 accelerometer driver — see M9Imu.h.
#if defined(HAS_M9_IMU) && defined(ESP32)

#include "M9Imu.h"
#include <Arduino.h>
#include <Wire.h>

// Bring-up aid, same role as M9_COMPASS_DEBUG: log the raw vector once a
// second so the sensor's axis orientation on this board can be derived from
// readings in known attitudes. Set to 0 once the mapping is baked in.
#ifndef M9_IMU_DEBUG
  #define M9_IMU_DEBUG 0
#endif

namespace {

constexpr uint8_t kAddr        = 0x6B;   // SA0 grounded on this board
constexpr uint8_t kRegWhoAmI   = 0x00;
constexpr uint8_t kRegRevision = 0x01;
constexpr uint8_t kRegCtrl1    = 0x02;
constexpr uint8_t kRegCtrl2    = 0x03;
constexpr uint8_t kRegCtrl3    = 0x04;
constexpr uint8_t kRegCtrl5    = 0x06;
constexpr uint8_t kRegCtrl7    = 0x08;
constexpr uint8_t kRegStatus0  = 0x2E;
constexpr uint8_t kRegAxL      = 0x35;
constexpr uint8_t kRegResetOk  = 0x4D;
constexpr uint8_t kRegReset    = 0x60;

constexpr uint8_t kWhoAmI      = 0x05;
constexpr uint8_t kResetCmd    = 0xB0;
constexpr uint8_t kResetOkVal  = 0x80;
constexpr uint8_t kStatusADrdy = 0x01;

// CTRL1: ADDR_AI=1 (bit6) so a burst read walks the data registers — without
// it every read returns the same byte. BE=0 for little-endian, SensorDisable=0.
constexpr uint8_t kCtrl1       = 0x40;
// CTRL2: ±2 g (aFS 000) at 62.5 Hz (aODR 0111). ±2 g because this measures
// gravity, not motion, and the finer scale is worth having; 62.5 Hz is the
// slowest NORMAL-mode rate above the compass's own update rate (the low-power
// rates below it duty-cycle the part and add noise for no benefit here).
constexpr uint8_t kCtrl2       = 0x07;
constexpr uint8_t kCtrl3       = 0x00;   // gyro off — nothing here needs it
// CTRL5: accel low-pass on, 2.66% of ODR (~1.7 Hz). Tilt is a slow quantity
// and hand tremor is not; filtering it here costs nothing.
constexpr uint8_t kCtrl5       = 0x01;
constexpr uint8_t kCtrl7On     = 0x01;   // aEN
constexpr uint8_t kCtrl7Off    = 0x00;

constexpr float    kGPerLsb    = 1.0f / 16384.0f;   // ±2 g
constexpr uint32_t kSampleMaxAge  = 1000;
constexpr uint32_t kIdleSuspendMs = 2000;
constexpr uint32_t kReprobeEvery  = 2000;
constexpr int      kMaxBusErrors  = 8;
constexpr uint32_t kWakeSettleMs  = 55;   // 3 ms + 3/ODR at 62.5 Hz

TwoWire* s_bus = nullptr;
bool     s_present = false;
bool     s_awake = false;
uint32_t s_next_probe_ms = 0;
uint32_t s_last_read_ms = 0;
uint32_t s_wake_ms = 0;
int      s_errors = 0;

float    s_x = 0, s_y = 0, s_z = 0;
uint32_t s_sample_ms = 0;
bool     s_have_sample = false;

bool writeReg(uint8_t reg, uint8_t val) {
  s_bus->beginTransmission(kAddr);
  s_bus->write(reg);
  s_bus->write(val);
  return s_bus->endTransmission() == 0;
}

bool readRegs(uint8_t reg, uint8_t* out, uint8_t n) {
  s_bus->beginTransmission(kAddr);
  s_bus->write(reg);
  if (s_bus->endTransmission(false) != 0) return false;
  if (s_bus->requestFrom((int)kAddr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) out[i] = (uint8_t)s_bus->read();
  return true;
}

bool configure() {
  if (!writeReg(kRegCtrl1, kCtrl1)) return false;
  if (!writeReg(kRegCtrl2, kCtrl2)) return false;
  if (!writeReg(kRegCtrl3, kCtrl3)) return false;
  if (!writeReg(kRegCtrl5, kCtrl5)) return false;
  if (!writeReg(kRegCtrl7, kCtrl7On)) return false;
  s_awake = true;
  s_wake_ms = millis();
  // Read back the two that decide whether data is even parseable: without
  // ADDR_AI the burst read silently returns six copies of one byte, which
  // looks like a working sensor reporting nonsense.
  uint8_t c1 = 0, c2 = 0;
  if (!readRegs(kRegCtrl1, &c1, 1) || !readRegs(kRegCtrl2, &c2, 1)) return false;
  return c1 == kCtrl1 && c2 == kCtrl2;
}

bool probe(bool log) {
  uint8_t id = 0;
  if (!readRegs(kRegWhoAmI, &id, 1)) {
    if (log) Serial.println("M9 IMU: no answer at 0x6B (QMI8658 absent or rail not up)");
    return false;
  }
  if (id != kWhoAmI) {
    if (log) Serial.printf("M9 IMU: unexpected WHO_AM_I 0x%02X at 0x6B (want 0x05)\n", id);
    return false;
  }
  // Soft reset, then wait long enough for either die: the QMI8658A finishes in
  // ~15 ms, the C wants ~150 ms, and the M9's variant is not documented.
  if (!writeReg(kRegReset, kResetCmd)) return false;
  delay(160);
  uint8_t ok = 0;
  if (!readRegs(kRegResetOk, &ok, 1) || ok != kResetOkVal) {
    if (log) Serial.printf("M9 IMU: reset flag 0x%02X (want 0x80)\n", ok);
    return false;
  }
  if (!configure()) {
    if (log) Serial.println("M9 IMU: QMI8658 found but configuration did not stick");
    return false;
  }
  uint8_t rev = 0;
  readRegs(kRegRevision, &rev, 1);
  if (log) Serial.printf("M9 IMU: QMI8658 ok (id=0x05 rev=0x%02X, accel 62.5 Hz +/-2 g)\n", rev);
  s_errors = 0;
  s_have_sample = false;
  // Nothing is asking for tilt yet: park it rather than run the accelerometer
  // from boot. m9ImuRead() wakes it.
  if (writeReg(kRegCtrl7, kCtrl7Off) && writeReg(kRegCtrl1, kCtrl1 | 0x01)) s_awake = false;
  return true;
}

}  // namespace

void m9ImuBegin(TwoWire& w) {
  s_bus = &w;
  s_present = probe(true);
  s_next_probe_ms = millis() + kReprobeEvery;
}

bool m9ImuPresent() { return s_present; }

bool m9ImuRead(float* x, float* y, float* z) {
  if (!s_bus) return false;
  const uint32_t now = millis();

  if (!s_present) {
    if ((int32_t)(now - s_next_probe_ms) < 0) return false;
    s_next_probe_ms = now + kReprobeEvery;
    s_present = probe(false);
    if (!s_present) return false;
  }

  s_last_read_ms = now;
  if (!s_awake) {
    // Leave power-down and re-enable the accelerometer. The first conversion
    // is one turn-on time away, so this call reports nothing fresh and the
    // caller's next poll gets data.
    if (!writeReg(kRegCtrl1, kCtrl1)) return false;
    if (!writeReg(kRegCtrl7, kCtrl7On)) return false;
    s_awake = true;
    s_wake_ms = now;
    return false;
  }
  if ((now - s_wake_ms) < kWakeSettleMs) return false;   // still settling

  uint8_t st = 0;
  bool ok = readRegs(kRegStatus0, &st, 1);
  if (ok && (st & kStatusADrdy)) {
    uint8_t b[6];
    ok = readRegs(kRegAxL, b, 6);
    if (ok) {
      const int16_t rx = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
      const int16_t ry = (int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8));
      const int16_t rz = (int16_t)((uint16_t)b[4] | ((uint16_t)b[5] << 8));
      s_x = rx * kGPerLsb;
      s_y = ry * kGPerLsb;
      s_z = rz * kGPerLsb;
      s_sample_ms = now;
      s_have_sample = true;
#if M9_IMU_DEBUG
      {
        static uint32_t last = 0;
        if (now - last >= 1000) {
          last = now;
          Serial.printf("[IMU] x=%+.3f y=%+.3f z=%+.3f g |a|=%.3f\n", (double)s_x, (double)s_y,
                        (double)s_z, (double)sqrtf(s_x * s_x + s_y * s_y + s_z * s_z));
        }
      }
#endif
    }
  }

  if (!ok) {
    if (++s_errors >= kMaxBusErrors) {
      s_present = false;
      s_have_sample = false;
      s_next_probe_ms = now + kReprobeEvery;
      Serial.println("M9 IMU: lost the QMI8658 (bus errors), will re-probe");
    }
    return false;
  }
  s_errors = 0;

  if (!s_have_sample || (now - s_sample_ms) > kSampleMaxAge) return false;
  if (x) *x = s_x;
  if (y) *y = s_y;
  if (z) *z = s_z;
  return true;
}

void m9ImuIdleTick() {
  if (!s_bus || !s_present || !s_awake) return;
  const uint32_t now = millis();
  if ((now - s_last_read_ms) < kIdleSuspendMs) return;
  // Accelerometer off, then the oscillator: CTRL1 bit0 is SensorDisable, which
  // is the part's real power-down (SensorLib's powerDown() writes the wrong
  // bit, so do not copy it).
  if (writeReg(kRegCtrl7, kCtrl7Off) && writeReg(kRegCtrl1, kCtrl1 | 0x01)) {
    s_awake = false;
    s_have_sample = false;
  }
}

#endif  // HAS_M9_IMU && ESP32
