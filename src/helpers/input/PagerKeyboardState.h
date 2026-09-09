// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

#include "LatchedModifier.h"

class PagerKeyboardState {
 public:
  static constexpr uint8_t ROWS = 4;
  static constexpr uint8_t COLS = 10;
#if defined(HAS_TDECK_PRO)
  static constexpr uint8_t ALT_POS = 2 * COLS + 9;
  static constexpr uint8_t SYMBOL_POS = 3 * COLS + 1;
  static constexpr uint8_t SHIFT_POS = 3 * COLS;
  static constexpr uint8_t SHIFT_POS_2 = 3 * COLS + 4;
  static constexpr uint8_t BACKSPACE_POS = 1 * COLS;
  static constexpr uint8_t SPACE_POS = 3 * COLS + 2;
#else
  static constexpr uint8_t ALT_POS = 2 * COLS;
  static constexpr uint8_t SHIFT_POS = 2 * COLS + 8;
  static constexpr uint8_t BACKSPACE_POS = 2 * COLS + 9;
  static constexpr uint8_t SPACE_POS = 3 * COLS;
#endif

  uint8_t event(uint8_t code, bool pressed, uint32_t now_ms) {
#if defined(HAS_TDECK_PRO)
    static const char base[ROWS][COLS] = {
      {'p', 'o', 'i', 'u', 'y', 't', 'r', 'e', 'w', 'q'},
      {'\0', 'l', 'k', 'j', 'h', 'g', 'f', 'd', 's', 'a'},
      {'\r', '$', 'm', 'n', 'b', 'v', 'c', 'x', 'z', '\0'},
      {'\0', '\0', ' ', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
    };
    static const char symbols[ROWS][COLS] = {
      {'@', '+', '-', '_', ')', '(', '3', '2', '1', '#'},
      {'\0', '"', '\'', ';', ':', '/', '6', '5', '4', '*'},
      {'\r', '$', '.', ',', '!', '?', '9', '8', '7', '\0'},
      {'\0', '\0', ' ', '0', '\0', '\0', '\0', '\0', '\0', '\0'},
    };
#else
    static const char base[ROWS][COLS] = {
      {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
      {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\r'},
      {'\0', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '\0', '\0'},
      {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
    };
    static const char symbols[ROWS][COLS] = {
      {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
      {'*', '/', '+', '-', '=', ':', '\'', '"', '@', '\0'},
      {'\0', '_', '$', ';', '?', '!', ',', '.', '\0', '\0'},
      {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
    };
#endif

    if (code == ALT_POS
#if defined(HAS_TDECK_PRO)
        || code == SYMBOL_POS
#endif
       ) {
      if (pressed) alt_.press();
      else         alt_.release(now_ms);
      return 0;
    }
    if (code == SHIFT_POS
#if defined(HAS_TDECK_PRO)
        || code == SHIFT_POS_2
#endif
       ) {
      if (pressed) {
        if (alt_.held()) {
          alt_.markHeldUsed();
          alt_shift_chord_pending_ = true;
        } else {
          shift_held_ = true;
        }
      } else {
        shift_held_ = false;
      }
      return 0;
    }
    if (code == BACKSPACE_POS) {
      if (pressed) {
        if (alt_.held()) {
          alt_.markHeldUsed();
          alt_backspace_chord_pending_ = true;
        } else {
          alt_.consumeForKey();
          backspace_held_ = true;
          return '\b';
        }
      } else {
        backspace_held_ = false;
      }
      return 0;
    }
    if (code == SPACE_POS) {
      space_held_ = pressed;
      if (pressed) {
        alt_.consumeForKey();
        return ' ';
      }
      return 0;
    }
    if (!pressed) return 0;

    const uint8_t row = code / COLS;
    const uint8_t col = code % COLS;
    if (row >= ROWS) return 0;
    const bool symbol_layer = alt_.consumeForKey();
    char key = symbol_layer ? symbols[row][col] : base[row][col];
    if (key == '\0') return 0;
    if ((caps_ || shift_held_) && !symbol_layer && key >= 'a' && key <= 'z') key -= 32;
    return (uint8_t)key;
  }

  bool altHeld() const { return alt_.held(); }
  void markAltUsed() { alt_.markHeldUsed(); }
  void discardAlt() {
    alt_.discard();
    alt_shift_chord_pending_ = false;
    alt_backspace_chord_pending_ = false;
  }
  bool backspaceHeld() const { return backspace_held_; }
  bool spaceHeld() const { return space_held_; }
  bool consumeAltShiftChord() {
    const bool pending = alt_shift_chord_pending_;
    alt_shift_chord_pending_ = false;
    return pending;
  }
  void toggleCaps() { caps_ = !caps_; }
  bool consumeAltBackspaceChord() {
    const bool pending = alt_backspace_chord_pending_;
    alt_backspace_chord_pending_ = false;
    return pending;
  }

 private:
  LatchedModifier alt_;
  bool caps_ = false;
  bool shift_held_ = false;
  bool alt_shift_chord_pending_ = false;
  bool alt_backspace_chord_pending_ = false;
  bool backspace_held_ = false;
  bool space_held_ = false;
};