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
  {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\n'},
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
// held); Caps is a press-to-toggle letter-case lock — same UX trail-mate
// already validated on this exact keyboard.
static constexpr uint8_t kAltPos       = 2 * KB_COLS + 0; // row2,col0 ('\0' in both layers)
static constexpr uint8_t kCapsPos      = 2 * KB_COLS + 8; // row2,col8 ('\0' in both layers)
static constexpr uint8_t kBackspacePos = 2 * KB_COLS + 9; // row2,col9 ('\0' in both layers)

static Adafruit_TCA8418 s_kb;
static bool s_inited = false;
static bool s_alt = false;
static bool s_caps = false;

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

    if (code == kAltPos)  { s_alt = pressed; continue; }
    if (code == kCapsPos) { if (pressed) s_caps = !s_caps; continue; }
    if (code == kBackspacePos) { if (pressed) ringPush('\b'); continue; }
    if (!pressed) continue;   // base/symbol keys only emit on press

    const uint8_t row = code / KB_COLS;
    const uint8_t col = code % KB_COLS;
    if (row >= KB_ROWS) continue;   // a GPIO event outside the matrix, not a key

    char c = s_alt ? s_symbolMap[row][col] : s_keymap[row][col];
    if (c == '\0') continue;
    if (s_caps && !s_alt) c = (char)toupper((unsigned char)c);   // caps affects the base layer only, matching trail-mate
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

#endif
