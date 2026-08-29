// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "LatchedModifier.h"

class TDeckKeyboardState {
 public:
  static constexpr size_t COLS = 5;

  size_t update(const uint8_t current[COLS], uint32_t now_ms,
                uint8_t* out, size_t out_capacity, bool modifiers_enabled = true) {
    static const char base[COLS][7] = {
      {'q', 'w', '\0', 'a', '\0', ' ', '\0'},
      {'e', 's', 'd', 'p', 'x', 'z', '\0'},
      {'r', 'g', 't', '\0', 'v', 'c', 'f'},
      {'u', 'h', 'y', '\0', 'b', 'n', 'j'},
      {'o', 'l', 'i', '\0', '$', 'm', 'k'},
    };
    static const char symbols[COLS][7] = {
      {'#', '1', '\0', '*', '\0', '\0', '0'},
      {'2', '4', '5', '@', '8', '7', '\0'},
      {'3', '/', '(', '\0', '?', '9', '6'},
      {'_', ':', ')', '\0', '!', ',', ';'},
      {'+', '"', '-', '\0', '\0', '.', '\''},
    };
    auto down = [&](size_t col, uint8_t row) {
      return (current[col] & (uint8_t)(1U << row)) != 0;
    };
    auto pressed = [&](size_t col, uint8_t row) {
      return down(col, row) && (previous_[col] & (uint8_t)(1U << row)) == 0;
    };
    auto released = [&](size_t col, uint8_t row) {
      return !down(col, row) && (previous_[col] & (uint8_t)(1U << row)) != 0;
    };

    if (!modifiers_enabled) {
      symbol_.baseline(down(0, 2));
      alt_.baseline(down(0, 4));
      shift_.baseline(down(1, 6) || down(2, 3));
    }
    if (modifiers_enabled && pressed(0, 2)) symbol_.press();
    if (modifiers_enabled && pressed(0, 4)) alt_.press();
    if (modifiers_enabled && released(0, 2)) symbol_.release(now_ms);
    if (modifiers_enabled && released(0, 4)) alt_.release(now_ms);
    if (modifiers_enabled && (pressed(1, 6) || pressed(2, 3)))  shift_.press();
    if (modifiers_enabled && (released(1, 6) || released(2, 3))) shift_.release(now_ms);

    // Held OR latched: a tap arms it for the next key, a double tap locks it.
    const bool shift = modifiers_enabled && (down(1, 6) || down(2, 3) || shift_.latched());
    size_t written = 0;
    auto emit = [&](uint8_t key) {
      if (written < out_capacity) out[written++] = key;
    };
    for (uint8_t row = 0; row < 7; ++row) {
      for (size_t col = 0; col < COLS; ++col) {
        if (!pressed(col, row)) continue;
        if ((col == 0 && (row == 2 || row == 4)) ||
            (col == 1 && row == 6) || (col == 2 && row == 3)) continue;
        const bool physical_alt = modifiers_enabled && alt_.held();
        if (physical_alt) alt_.markHeldUsed();
        // Preserve LilyGO controller shortcuts while raw mode moves ordinary
        // matrix translation into the host. The C3 still handles Alt+B's
        // backlight toggle; suppress its character exactly as key mode does.
        if (physical_alt && col == 3 && row == 4) {
          symbol_.consumeForKey();
          continue;
        }
        if (physical_alt && col == 2 && row == 5) {
          symbol_.consumeForKey();
          emit(0x0C);
          continue;
        }
        if (col == 3 && row == 3) {
          symbol_.consumeForKey(); alt_.consumeForKey(); emit('\r'); continue;
        }
        if (col == 4 && row == 3) {
          symbol_.consumeForKey(); alt_.consumeForKey(); emit('\b'); continue;
        }

        const bool use_symbols = modifiers_enabled && (symbol_.active() || alt_.latched());
        const char key = use_symbols ? symbols[col][row] : base[col][row];
        if (modifiers_enabled) {
          symbol_.consumeForKey();
          if (!physical_alt) alt_.consumeForKey();
          shift_.consumeForKey();   // one-shot shift expires on the key it capitalised
        }
        if (key == '\0') continue;
        char resolved = key;
        if (!use_symbols && shift && resolved >= 'a' && resolved <= 'z') resolved -= 32;
        emit((uint8_t)resolved);
      }
    }

    for (size_t col = 0; col < COLS; ++col) previous_[col] = current[col] & 0x7F;
    return written;
  }

  void baseline(const uint8_t current[COLS]) {
    symbol_.baseline((current[0] & (uint8_t)(1U << 2)) != 0);
    alt_.baseline((current[0] & (uint8_t)(1U << 4)) != 0);
    for (size_t col = 0; col < COLS; ++col) previous_[col] = current[col] & 0x7F;
  }

  void discardModifiers() {
    symbol_.discard();
    alt_.discard();
  }

 private:
  uint8_t previous_[COLS] = {};
  LatchedModifier symbol_;
  LatchedModifier alt_;
  // Shift latches like the other two (#344). It used to be a bare "is it held"
  // test, so a capital letter meant holding shift down while reaching for the
  // key. The board has two shift keys and they share one latch: whichever you
  // press arms it, and either releases it.
  LatchedModifier shift_;
};