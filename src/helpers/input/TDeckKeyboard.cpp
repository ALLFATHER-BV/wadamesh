// LilyGo T-Deck physical keyboard driver — see TDeckKeyboard.h.
#if defined(HAS_TDECK_KEYBOARD) && defined(ESP32)

#include "TDeckKeyboard.h"
#include <Arduino.h>
#include <Wire.h>

#ifndef PIN_KB_ADDR
  #define PIN_KB_ADDR 0x55
#endif

// Single-producer (core-0 poll) / single-consumer (UI thread) ring. Byte indices
// on a 32-bit MCU are atomic enough for SPSC without a lock.
static volatile uint8_t s_ring[16];
static volatile uint8_t s_head = 0;   // written by producer
static volatile uint8_t s_tail = 0;   // written by consumer
static bool             s_inited = false;

// Backlight: the UI thread requests a level; the actual I2C write happens in the
// poll (core 0). The keyboard's C3 firmware sets the backlight on an I2C write.
static volatile uint8_t s_bl_desired = 0;
static volatile bool    s_bl_dirty   = false;

void tdeckKeyboardBegin() {
  s_inited = true;   // Wire was configured (18/8, 400k, 20ms timeout) by the touch driver
}

void tdeckKeyboardSetBacklight(uint8_t level) {
  // Force the FIRST write even when the requested level matches our cached default (0). A reflash
  // resets the ESP32 but NOT the keyboard's C3 — it keeps its previously-lit backlight — so without
  // this the boot "off" request (0 == cached 0) was never sent and the backlight stayed on despite
  // the setting reading "Off" (issue #33). After the first write, change-detection resumes.
  static bool forced = false;
  if (level != s_bl_desired || !forced) { s_bl_desired = level; s_bl_dirty = true; forced = true; }
}

void tdeckKeyboardFlushBacklight() {
  if (!s_inited || !s_bl_dirty) return;
  {
    s_bl_dirty = false;
    // LilyGo T-Keyboard backlight: 2-byte command [0x01, brightness] (0 = off).
    Wire.beginTransmission(PIN_KB_ADDR);
    Wire.write(0x01);            // LILYGO_KB_BRIGHTNESS_CMD
    Wire.write(s_bl_desired);    // 0 = off, 1-255 = brightness
    Wire.endTransmission();
  }
}

void tdeckKeyboardPoll() {
  if (!s_inited) return;
  tdeckKeyboardFlushBacklight();
  if (Wire.requestFrom((int)PIN_KB_ADDR, 1) != 1) return;
  uint8_t key = Wire.read();
  if (key == 0) return;                       // no key this scan
  const uint8_t nh = (uint8_t)((s_head + 1) & 15);
  if (nh != s_tail) {                          // drop if the ring is full
    s_ring[s_head] = key;
    s_head = nh;
  }
}

int tdeckKeyboardReadKey() {
  if (s_tail == s_head) return 0;              // empty
  const uint8_t key = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 15);
  return key;
}

#endif
