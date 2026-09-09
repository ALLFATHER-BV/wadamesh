// SPDX-License-Identifier: GPL-3.0-or-later
#include "PagerKeyboard.h"

#if defined(HAS_PAGER_KEYBOARD) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_TCA8418.h>
#include "PagerKeyboardState.h"

#ifndef KB_INT
  #define KB_INT 6
#endif
#ifndef KB_BACKLIGHT
  #define KB_BACKLIGHT 46
#endif
// Modifier/special-key positions: 0-based (row*KB_COLS + col), matching the
// TCA8418 raw event's (code & 0x7F) - 1. Alt selects symbols while held, a
// solo tap selects them for one key, and a double tap locks the layer. Shift
// is a hold too (momentary uppercase on the base layer, real
// Shift-key semantics) — held Alt THEN a Shift press instead chords into
// Alt+Shift, reported via s_alt_shift_chord_pending. What that chord DOES is
// a UI-level decision (UITask.cpp): Caps Lock toggle while editing a text
// field, no-op otherwise — this driver has no idea which field (if any) is
// focused, so it only reports the chord, it doesn't act on it (see
// pagerKeyboardConsumeAltShiftChord()/pagerKeyboardToggleCaps()). Held Alt
// THEN a Backspace press similarly chords into Alt+Backspace
// (s_alt_backspace_chord_pending) — unlike Alt+Shift this one has a single,
// context-independent effect (jump Home, everywhere, including mid-edit),
// so it's just as driver-agnostic to report but never conditional at the UI
// layer. Note: Alt (row2,col0) and Shift (row2,col8) share row2, and
// row0/row1 col8 are 'o'/'l' — holding Alt+Shift+O or Alt+Shift+L all three
// at once will phantom-ghost a 'q'/'a' at the row2/col0 intersection
// (classic diode-less-matrix 3-key rectangle, no software fix possible);
// harmless in practice since the intended gesture is
// hold-Alt-tap-Shift-release-both, not holding all three simultaneously.
// Backspace (row2,col9) sits one column over from Shift, so Alt+Backspace
// doesn't share this exact ghosting risk with any base-layer letter. The maps
// and positions live in PagerKeyboardState so host tests exercise the exact
// production translation logic.

static Adafruit_TCA8418 s_kb;
static bool s_inited = false;
static PagerKeyboardState s_state;

// Single-producer (poll) / single-consumer ring. Polling currently happens on
// the UI task, but the short critical section preserves the header's contract
// if a future board moves I2C polling to another core.
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
#if defined(HAS_TDECK_PRO)
static constexpr uint8_t kRingSize = 64;
#else
static constexpr uint8_t kRingSize = 16;
#endif
static_assert((kRingSize & (kRingSize - 1)) == 0, "keyboard ring must be a power of two");
static uint8_t s_ring[kRingSize];
static uint8_t s_head = 0;
static uint8_t s_tail = 0;

static bool s_bl_ready = false;
// This framework's Arduino-ESP32 core only has the channel-based LEDC API
// (ledcSetup/ledcAttachPin/ledcWrite by channel — confirmed against
// esp32-hal-ledc.h, not the newer pin-based ledcAttach()). Channel 0: nothing
// else on this board claims an LEDC channel (the AW9364 display backlight is
// pulse-driven, not PWM).
#if defined(HAS_TDECK_PRO)
static constexpr uint8_t kKbBacklightPwmChannel = 1;   // channel 0 drives the e-paper frontlight
#else
static constexpr uint8_t kKbBacklightPwmChannel = 0;
#endif

static void ringPush(uint8_t c) {
  portENTER_CRITICAL(&s_ring_mux);
  const uint8_t nh = (uint8_t)((s_head + 1) & (kRingSize - 1));
  if (nh != s_tail) {   // drop if the ring is full
    s_ring[s_head] = c;
    s_head = nh;
  }
  portEXIT_CRITICAL(&s_ring_mux);
}

void pagerKeyboardBegin() {
  if (s_inited) return;
  s_inited = s_kb.begin(TCA8418_DEFAULT_ADDR, &Wire) &&
             s_kb.matrix(PagerKeyboardState::ROWS, PagerKeyboardState::COLS);
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

    const uint8_t key = s_state.event(code, pressed, millis());
    if (key) ringPush(key);
  }
}

int pagerKeyboardReadKey() {
  portENTER_CRITICAL(&s_ring_mux);
  if (s_tail == s_head) {
    portEXIT_CRITICAL(&s_ring_mux);
    return 0;
  }
  const uint8_t c = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & (kRingSize - 1));
  portEXIT_CRITICAL(&s_ring_mux);
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

bool pagerKeyboardAltHeld() { return s_state.altHeld(); }

void pagerKeyboardMarkAltUsed() { s_state.markAltUsed(); }

void pagerKeyboardDiscardAlt() {
  s_state.discardAlt();
}

bool pagerKeyboardBackspaceHeld() { return s_state.backspaceHeld(); }

bool pagerKeyboardSpaceHeld() { return s_state.spaceHeld(); }

bool pagerKeyboardConsumeAltShiftChord() {
  return s_state.consumeAltShiftChord();
}

void pagerKeyboardToggleCaps() { s_state.toggleCaps(); }

bool pagerKeyboardConsumeAltBackspaceChord() {
  return s_state.consumeAltBackspaceChord();
}

#endif
