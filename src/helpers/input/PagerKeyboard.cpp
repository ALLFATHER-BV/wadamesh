// SPDX-License-Identifier: GPL-3.0-or-later
#include "PagerKeyboard.h"

#if defined(HAS_PAGER_KEYBOARD) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_TCA8418.h>
#include <cctype>

#ifndef KB_INT
  #define KB_INT 6
#endif
#ifndef KB_BACKLIGHT
  #define KB_BACKLIGHT 46
#endif
#define KB_ROWS 4
#define KB_COLS 10

// Matrix legend, s_keymap[row][col] — same physical keyboard PCB as
// trail-mate's working LR1121 pager build, cross-checked there. '\0' = no
// character at that position (dead cell, or intercepted as a modifier below).
static constexpr char s_keymap[KB_ROWS][KB_COLS] = {
  {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
  {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\r'},
  {'\0', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '\0', '\0'},
  {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};
// Alt layer (symbols/numbers) — this hardware has no separate physical Symbol
// key, so Alt alone drives it (matches trail-mate's has_symbol_key=false path).
static constexpr char s_symbolMap[KB_ROWS][KB_COLS] = {
  {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
  {'*', '/', '+', '-', '=', ':', '\'', '"', '@', '\0'},
  {'\0', '_', '$', ';', '?', '!', ',', '.', '\0', '\0'},
  {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};

// Modifier/special-key positions: 0-based (row*KB_COLS + col), matching the
// TCA8418 raw event's (code & 0x7F) - 1. Alt is a hold (symbol layer while
// held); Shift is a hold too (momentary uppercase on the base layer, real
// Shift-key semantics) — held Alt THEN a Shift press instead chords into
// Alt+Shift, reported via s_alt_shift_chord_pending. What that chord DOES is
// a UI-level decision (UITask.cpp): Caps Lock toggle while editing a text
// field, or jump Home otherwise — this driver has no idea which field (if
// any) is focused, so it only reports the chord, it doesn't act on it (see
// pagerKeyboardConsumeAltShiftChord()/pagerKeyboardToggleCaps()). Note: Alt
// (row2,col0) and Shift (row2,col8) share row2, and row0/row1 col8 are
// 'o'/'l' — holding Alt+Shift+O or Alt+Shift+L all three at once will
// phantom-ghost a 'q'/'a' at the row2/col0 intersection (classic
// diode-less-matrix 3-key rectangle, no software fix possible); harmless in
// practice since the intended gesture is hold-Alt-tap-Shift-release-both,
// not holding all three simultaneously.
static constexpr uint8_t kAltPos       = 2 * KB_COLS + 0; // row2,col0 ('\0' in both layers)
static constexpr uint8_t kShiftPos     = 2 * KB_COLS + 8; // row2,col8 ('\0' in both layers)
static constexpr uint8_t kBackspacePos = 2 * KB_COLS + 9; // row2,col9 ('\0' in both layers)
static constexpr uint8_t kSpacePos     = 3 * KB_COLS + 0; // row3,col0 (' ' in both layers)

static Adafruit_TCA8418 s_kb;
static bool s_inited = false;
static bool s_alt = false;
static bool s_alt_used = false;         // Alt consumed as a modifier since it was last pressed
static bool s_alt_tap_pending = false;  // Alt pressed+released with nothing else happening meanwhile
static bool s_caps = false;
static bool s_shift_held = false;   // momentary Shift, mirrors s_backspace_held/s_space_held
static bool s_alt_shift_chord_pending = false;   // one-shot, see pagerKeyboardConsumeAltShiftChord()
static bool s_backspace_held = false;
static bool s_space_held = false;

// Single-producer (poll) / single-consumer (UI thread) ring — same pattern as
// TDeckKeyboard.cpp; byte indices are atomic enough for SPSC without a lock.
static volatile uint8_t s_ring[16];
static volatile uint8_t s_head = 0;
static volatile uint8_t s_tail = 0;

static bool s_bl_ready = false;
// This framework's Arduino-ESP32 core only has the channel-based LEDC API
// (ledcSetup/ledcAttachPin/ledcWrite by channel — confirmed against
// esp32-hal-ledc.h, not the newer pin-based ledcAttach()). Channel 0: nothing
// else on this board claims an LEDC channel (the AW9364 display backlight is
// pulse-driven, not PWM).
static constexpr uint8_t kKbBacklightPwmChannel = 0;

static void ringPush(uint8_t c) {
  const uint8_t nh = (uint8_t)((s_head + 1) & 15);
  if (nh != s_tail) {   // drop if the ring is full
    s_ring[s_head] = c;
    s_head = nh;
  }
}

void pagerKeyboardBegin() {
  if (s_inited) return;
  s_inited = s_kb.begin(TCA8418_DEFAULT_ADDR, &Wire) && s_kb.matrix(KB_ROWS, KB_COLS);
  if (!s_inited) return;
  s_kb.flush();
  pinMode(KB_INT, INPUT_PULLUP);   // TCA8418 INT is open-drain active-low; not ISR-driven here (see .h)
  s_kb.enableInterrupts();
}

void pagerKeyboardPoll() {
  if (!s_inited) return;
  while (s_kb.available()) {
    const uint8_t raw = s_kb.getEvent();
    if (raw == 0) break;
    // TCA8418 KEY_EVENT_A bit 7: 1 = press, 0 = release (TI datasheet SCPS215E
    // register description, verified directly — the Adafruit library's own
    // header comment states this backwards; don't trust it).
    const bool pressed = (raw & 0x80) != 0;
    const uint8_t code = (uint8_t)((raw & 0x7F) - 1);

    if (code == kAltPos) {
      // Solo tap (press+release, nothing else in between) vs. a modifier hold
      // (symbol-layer typing, or the rotary encoder's Alt+turn via
      // pagerKeyboardMarkAltUsed()) — only the former queues a pending tap.
      if (pressed) { s_alt = true; s_alt_used = false; }
      else { if (!s_alt_used) s_alt_tap_pending = true; s_alt = false; }
      continue;
    }
    // Any other key event while Alt is held means Alt is being used as a
    // modifier, not tapped solo — cancels the pending-tap interpretation.
    if (s_alt && pressed) s_alt_used = true;
    if (code == kShiftPos) {
      if (pressed) {
        if (s_alt) s_alt_shift_chord_pending = true;   // Alt (Fn) held + Shift press = chord (UI decides the effect)
        else       s_shift_held = true;
      } else {
        s_shift_held = false;
      }
      continue;
    }
    if (code == kBackspacePos) { s_backspace_held = pressed; if (pressed) ringPush('\b'); continue; }
    if (code == kSpacePos) { s_space_held = pressed; if (pressed) ringPush(' '); continue; }
    if (!pressed) continue;   // base/symbol keys only emit on press

    const uint8_t row = code / KB_COLS;
    const uint8_t col = code % KB_COLS;
    if (row >= KB_ROWS) continue;   // a GPIO event outside the matrix, not a key

    char c = s_alt ? s_symbolMap[row][col] : s_keymap[row][col];
    if (c == '\0') continue;
    // Caps Lock and a held Shift both mean "uppercase," base layer only.
    if ((s_caps || s_shift_held) && !s_alt) c = (char)toupper((unsigned char)c);
    ringPush((uint8_t)c);
  }
}

int pagerKeyboardReadKey() {
  if (s_tail == s_head) return 0;
  const uint8_t c = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 15);
  return c;
}

void pagerKeyboardSetBacklight(uint8_t level) {
  if (!s_bl_ready) {
    ledcSetup(kKbBacklightPwmChannel, 1000 /* Hz */, 8 /* bits */);
    ledcAttachPin(KB_BACKLIGHT, kKbBacklightPwmChannel);
    s_bl_ready = true;
  }
  ledcWrite(kKbBacklightPwmChannel, level);
}

bool pagerKeyboardAltHeld() { return s_alt; }

void pagerKeyboardMarkAltUsed() { s_alt_used = true; }

bool pagerKeyboardConsumeAltTap() {
  if (!s_alt_tap_pending) return false;
  s_alt_tap_pending = false;
  return true;
}

bool pagerKeyboardBackspaceHeld() { return s_backspace_held; }

bool pagerKeyboardSpaceHeld() { return s_space_held; }

bool pagerKeyboardConsumeAltShiftChord() {
  if (!s_alt_shift_chord_pending) return false;
  s_alt_shift_chord_pending = false;
  return true;
}

void pagerKeyboardToggleCaps() { s_caps = !s_caps; }

#endif
