// ThinkNode M9 keyboard driver — see M9Keyboard.h.
#if defined(HAS_M9_KEYBOARD) && defined(ESP32)

#include "M9Keyboard.h"
#include "M9KeyGuard.h"
#include <Arduino.h>
#include <Wire.h>

#ifndef PIN_KB_ADDR
  #define PIN_KB_ADDR 0x6c
#endif

static constexpr uint8_t M9_KB_V11_ADDR = 0x6d;

// Controller register map — from Elecrow's keyboard-controller firmware
// (ThinkNode-M9-KB-platformio, src/main.cpp). The keyboard is a separate
// ESP32-S2 running a matrix scanner as an I2C SLAVE with addressed registers:
// reads MUST be preceded by a 1-byte register-address write, otherwise the
// slave answers with whatever register was addressed last (0x00 = HW version
// after its reset — a constant 0x03, which is exactly why the bare-read
// protocol produced "no keys do anything").
#define M9_KB_REG_HW_VER     0x00   // constant 0x03 on this controller FW
#define M9_KB_REG_KEY        0x01   // pending key byte, 0x00 = none (single slot)
#define M9_KB_REG_BACKLIGHT  0x02   // PWM duty 0..255 (controller lights on keypress)
#define M9_KB_REG_FW_VER     0xFE   // constant 0x10 on this controller FW

static volatile uint8_t s_ring[16];
static volatile uint8_t s_head = 0;
static volatile uint8_t s_tail = 0;
static bool             s_inited = false;
static void           (*s_found_cb)(uint8_t) = nullptr;
static bool             s_found_cb_done = false;
static TwoWire*         s_bus = nullptr;      // set once the controller answers
static uint8_t          s_address = PIN_KB_ADDR;
static uint32_t         s_last_probe_ms = 0;
static M9KeyGuard       s_guard;

static bool kbReadReg(TwoWire& w, uint8_t address, uint8_t reg, uint8_t* out) {
  w.beginTransmission(address);
  w.write(reg);
  if (w.endTransmission() != 0) return false;         // NACK / bus error
  if (w.requestFrom((int)address, 1) != 1) return false;
  *out = (uint8_t)w.read();
  return true;
}

static bool kbProbe(TwoWire& w, const char* name, uint8_t address) {
  uint8_t hw = 0, fw = 0;
  if (!kbReadReg(w, address, M9_KB_REG_HW_VER, &hw)) return false;
  kbReadReg(w, address, M9_KB_REG_FW_VER, &fw);   // best-effort, for the log only
  s_bus = &w;
  s_address = address;
  Serial.printf("M9 keyboard: controller on %s addr=0x%02X (hw=0x%02X fw=0x%02X)\n", name, address, hw, fw);
  return true;
}

// The schematic reading puts the keyboard on its own bus (host 20/21); the
// shared sensor bus (Wire, 7/6) is probed as a fallback so a wrong pin
// reading shows up in the boot log instead of as a dead keyboard.
static void kbTryFindBus() {
  // V1.0 uses 0x6c; V1.1 uses 0x6d (Meshtastic's M9 variant.cpp).
  // Keep the existing address/bus preference, then try the newer controller.
  const uint8_t addresses[] = {PIN_KB_ADDR, M9_KB_V11_ADDR};
  for (uint8_t address : addresses) {
    if (kbProbe(Wire1, "Wire1 (20/21)", address) ||
        kbProbe(Wire, "Wire (7/6)", address)) break;
  }
  if (s_bus && s_found_cb && !s_found_cb_done) { s_found_cb_done = true; s_found_cb(s_address); }
}

uint8_t m9KeyboardAddr() { return s_bus ? s_address : 0; }

void m9KeyboardSetFoundCallback(void (*cb)(uint8_t addr)) { s_found_cb = cb; s_found_cb_done = false; }

void m9KeyboardBegin() {
  Wire1.begin(PIN_KB_SDA, PIN_KB_SCL);
  // The controller is its own MCU (ESP32-S2) on the ALWAYS-ON 3V3 rail via R3
  // (schematic-verified, see the keyboard-bus note in platformio.ini — an
  // earlier revision of this comment said "switched peripheral rail", which
  // matters for the deep-sleep drain budget: the S2 keeps drawing through a
  // deep sleep, and only the power slider actually cuts it). It may still be
  // booting here — poll re-probes once a second until it answers.
  kbTryFindBus();
  if (!s_bus) Serial.println("M9 keyboard: no controller yet (will keep probing)");
  s_inited = true;
}

void m9KeyboardPoll() {
  if (!s_inited) return;
  // Rate-limit: the UI loop free-runs, and unthrottled this was a blocking
  // ~0.5 ms write+read on the 100 kHz bus EVERY iteration (hundreds/s) — pure
  // waste on the UI thread. The controller latches one key, so the poll only
  // has to outpace keystrokes: 15 ms (~66/s) is ample for burst typing and
  // removes ~97% of the bus traffic. Wraparound-safe; first call passes.
  {
    static uint32_t s_next_poll_ms = 0;
    const uint32_t now_ms = millis();
    if ((int32_t)(now_ms - s_next_poll_ms) < 0) return;
    s_next_poll_ms = now_ms + 15;
  }
  if (!s_bus) {
    uint32_t now = millis();
    if (now - s_last_probe_ms < 1000) return;
    s_last_probe_ms = now;
    kbTryFindBus();
    if (!s_bus) return;
  }
  // One read per poll. The controller latches a single key, so reading again
  // straight away can only return nothing or that same key, if the controller
  // has not cleared it yet: the old three-read drain entered keys twice (#625).
  // The Launcher and Meshtastic M9 ports also take one read per key.
  uint8_t key = 0;
  if (!kbReadReg(*s_bus, s_address, M9_KB_REG_KEY, &key)) return;
  if (key == 0x00 || key == 0xFF) return;   // no key / undefined-register reply
  // The same key again soon after is the same press: a stale latch read, or the
  // matrix chattering as a slow key comes up (#625, #671). See M9KeyGuard.h.
  if (!m9KeyGuardAccept(s_guard, key, millis())) return;
  const uint8_t nh = (uint8_t)((s_head + 1) & 15);
  if (nh != s_tail) {     // drop if the ring is full
    s_ring[s_head] = key;
    s_head = nh;
  }
}

int m9KeyboardReadKey() {
  if (s_tail == s_head) return 0;   // empty
  const uint8_t key = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 15);
  return key;
}

bool m9KeyboardSetBacklight(uint8_t duty) {
  if (!s_inited || !s_bus) return false;   // controller not found yet (see the probe note above)
  s_bus->beginTransmission(s_address);
  s_bus->write(M9_KB_REG_BACKLIGHT);
  s_bus->write(duty);
  return s_bus->endTransmission() == 0;    // NACK/bus error -> the duty was NOT taken
}

#endif
