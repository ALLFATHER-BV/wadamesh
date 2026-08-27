// LilyGo T-Deck physical keyboard driver — see TDeckKeyboard.h.
#if defined(HAS_TDECK_KEYBOARD) && defined(ESP32)

#include "TDeckKeyboard.h"
#include "TDeckKeyboardState.h"
#include <Arduino.h>
#include <Wire.h>

#ifndef PIN_KB_ADDR
  #define PIN_KB_ADDR 0x55
#endif

// Core-0 produces and the UI task consumes. One short cross-core critical
// section publishes each byte together with its index and also protects the
// desired modifier-input mode below.
static portMUX_TYPE s_keyboard_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_ring[16];
static uint8_t s_head = 0;
static uint8_t s_tail = 0;
static bool             s_inited = false;
enum class KeyboardMode : uint8_t { Probe, Raw, Legacy };
static KeyboardMode       s_mode = KeyboardMode::Probe;
static TDeckKeyboardState s_raw_state;
static bool               s_modifier_input_allowed = true; // guarded by s_keyboard_mux
static uint32_t           s_modifier_mode_generation = 0;   // guarded by s_keyboard_mux
static uint32_t           s_modifier_generation_applied = 0; // core-0 owner only

// Backlight: the UI thread requests a level; the actual I2C write happens in the
// poll (core 0). The keyboard's C3 firmware sets the backlight on an I2C write.
static volatile uint8_t s_bl_desired = 0;
static volatile bool    s_bl_dirty   = false;

static void ringPushLocked(uint8_t key) {
  if (!key) return;
  const uint8_t next = (uint8_t)((s_head + 1) & 15);
  if (next != s_tail) {
    s_ring[s_head] = key;
    s_head = next;
  }
}

static void keyboardCommand(uint8_t command) {
  Wire.beginTransmission(PIN_KB_ADDR);
  Wire.write(command);
  Wire.endTransmission();
}

static int keyboardRead(uint8_t* out, size_t count) {
  Wire.requestFrom((int)PIN_KB_ADDR, (int)count);
  int read = 0;
  while (Wire.available() && read < (int)count) out[read++] = (uint8_t)Wire.read();
  while (Wire.available()) Wire.read();
  return read;
}

static void processRawFrame(const uint8_t frame[TDeckKeyboardState::COLS], uint32_t now_ms) {
  uint8_t keys[16];
  portENTER_CRITICAL(&s_keyboard_mux);
  const uint32_t generation = s_modifier_mode_generation;
  if (generation != s_modifier_generation_applied) {
    // Transition frame: establish current physical state, publish nothing.
    // The mode setter already cleared older queued bytes under this same lock.
    s_raw_state.baseline(frame);
    s_modifier_generation_applied = generation;
  } else {
    const size_t key_count = s_raw_state.update(frame, now_ms, keys, sizeof keys,
                                                s_modifier_input_allowed);
    for (size_t i = 0; i < key_count; ++i) ringPushLocked(keys[i]);
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

static void processLegacyKey(uint8_t key) {
  portENTER_CRITICAL(&s_keyboard_mux);
  const uint32_t generation = s_modifier_mode_generation;
  if (generation != s_modifier_generation_applied) {
    // No raw modifier state exists, and the C3 exposes one latest-byte mailbox
    // (comdata/comdata_flag), not one response per host mode transition. Any
    // number of transitions before this read therefore coalesce deliberately:
    // drop the sole pending byte once so it cannot cross into the final mode.
    s_modifier_generation_applied = generation;
  } else {
    ringPushLocked(key);
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

void tdeckKeyboardBegin() {
  s_mode = KeyboardMode::Probe;
  s_raw_state = TDeckKeyboardState{};
  portENTER_CRITICAL(&s_keyboard_mux);
  s_head = s_tail = 0;
  // Keep any desired mode the UI published immediately after starting this
  // task. Setting applied one generation behind forces the first response to
  // baseline (or drain the legacy controller's one-byte mailbox).
  s_modifier_generation_applied = s_modifier_mode_generation - 1;
  portEXIT_CRITICAL(&s_keyboard_mux);
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
  if (s_mode == KeyboardMode::Probe) {
    // LilyGO keyboard firmware from June 2025 onward accepts 0x03 and returns
    // five column-state bytes. Older controllers ignore it and keep returning
    // one resolved ASCII byte; restore 0x04 key mode immediately in that case.
    keyboardCommand(0x03);
    uint8_t frame[TDeckKeyboardState::COLS] = {};
    const int count = keyboardRead(frame, sizeof frame);
    bool valid_raw = count == (int)sizeof frame;
    for (size_t col = 0; col < sizeof frame && valid_raw; ++col)
      valid_raw = (frame[col] & 0x80) == 0;
    if (valid_raw) {
      s_mode = KeyboardMode::Raw;
      Serial.println("[keyboard] T-Deck raw mode: modifier latching enabled");
      processRawFrame(frame, millis());
      return;
    }
    keyboardCommand(0x04);
    s_mode = KeyboardMode::Legacy;
    Serial.println("[keyboard] T-Deck legacy mode: update keyboard C3 firmware for modifier latching");
    if (count == 1) processLegacyKey(frame[0]);
    return;
  }
  if (s_mode == KeyboardMode::Raw) {
    uint8_t frame[TDeckKeyboardState::COLS] = {};
    if (keyboardRead(frame, sizeof frame) != (int)sizeof frame) return;
    for (size_t col = 0; col < sizeof frame; ++col) if (frame[col] & 0x80) return;
    processRawFrame(frame, millis());
    return;
  }
  uint8_t key = 0;
  if (keyboardRead(&key, 1) == 1) processLegacyKey(key);
}

int tdeckKeyboardReadKey() {
  portENTER_CRITICAL(&s_keyboard_mux);
  if (s_tail == s_head) {
    portEXIT_CRITICAL(&s_keyboard_mux);
    return 0;
  }
  const uint8_t key = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 15);
  portEXIT_CRITICAL(&s_keyboard_mux);
  return key;
}

void tdeckKeyboardDiscardModifiers() {
  portENTER_CRITICAL(&s_keyboard_mux);
  if (s_modifier_input_allowed) {
    s_modifier_input_allowed = false;
    ++s_modifier_mode_generation;
    s_tail = s_head;
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

void tdeckKeyboardAllowModifiers() {
  portENTER_CRITICAL(&s_keyboard_mux);
  if (!s_modifier_input_allowed) {
    s_modifier_input_allowed = true;
    ++s_modifier_mode_generation;
    s_tail = s_head;
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

#endif
