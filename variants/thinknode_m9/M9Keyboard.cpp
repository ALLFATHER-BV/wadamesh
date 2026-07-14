// ThinkNode M9 keyboard driver — see M9Keyboard.h.
#if defined(HAS_M9_KEYBOARD) && defined(ESP32)

#include "M9Keyboard.h"
#include <Arduino.h>
#include <Wire.h>

#ifndef PIN_KB_ADDR
  #define PIN_KB_ADDR 0x6c
#endif

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
static TwoWire*         s_bus = nullptr;      // set once the controller answers
static uint32_t         s_last_probe_ms = 0;

static bool kbReadReg(TwoWire& w, uint8_t reg, uint8_t* out) {
  w.beginTransmission((uint8_t)PIN_KB_ADDR);
  w.write(reg);
  if (w.endTransmission() != 0) return false;         // NACK / bus error
  if (w.requestFrom((int)PIN_KB_ADDR, 1) != 1) return false;
  *out = (uint8_t)w.read();
  return true;
}

static bool kbProbe(TwoWire& w, const char* name) {
  uint8_t hw = 0, fw = 0;
  if (!kbReadReg(w, M9_KB_REG_HW_VER, &hw)) return false;
  kbReadReg(w, M9_KB_REG_FW_VER, &fw);   // best-effort, for the log only
  Serial.printf("M9 keyboard: controller on %s (hw=0x%02X fw=0x%02X)\n", name, hw, fw);
  return true;
}

// The schematic reading puts the keyboard on its own bus (host 20/21); the
// shared sensor bus (Wire, 7/6) is probed as a fallback so a wrong pin
// reading shows up in the boot log instead of as a dead keyboard.
static void kbTryFindBus() {
  if (kbProbe(Wire1, "Wire1 (20/21)"))     s_bus = &Wire1;
  else if (kbProbe(Wire, "Wire (7/6)"))    s_bus = &Wire;
}

void m9KeyboardBegin() {
  Wire1.begin(PIN_KB_SDA, PIN_KB_SCL);
  // The controller is its own MCU on the switched peripheral rail and may
  // still be booting here — poll re-probes once a second until it answers.
  kbTryFindBus();
  if (!s_bus) Serial.println("M9 keyboard: no controller yet (will keep probing)");
  s_inited = true;
}

void m9KeyboardPoll() {
  if (!s_inited) return;
  if (!s_bus) {
    uint32_t now = millis();
    if (now - s_last_probe_ms < 1000) return;
    s_last_probe_ms = now;
    kbTryFindBus();
    if (!s_bus) return;
  }
  uint8_t key = 0;
  if (!kbReadReg(*s_bus, M9_KB_REG_KEY, &key)) return;
  if (key == 0x00 || key == 0xFF) return;   // no key / undefined-register reply
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

void m9KeyboardSetBacklight(uint8_t duty) {
  if (!s_inited || !s_bus) return;
  s_bus->beginTransmission((uint8_t)PIN_KB_ADDR);
  s_bus->write(M9_KB_REG_BACKLIGHT);
  s_bus->write(duty);
  s_bus->endTransmission();
}

#endif
