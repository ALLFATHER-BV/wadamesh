// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the T-Deck keyboard protocol detection (3864292c): the production
// src/helpers/input/TDeckKeyboard.cpp (looksLikeAscii, frameBits, the probe and the raw
// path) against a scripted keyboard controller on a fake I2C bus.
//
//   c++ -std=c++17 -Wall -Wextra -I src -I test/tdeck_keyboard_fakes \
//       test/test_tdeck_keyboard_detect.cpp -o /tmp/test_tdeck_keyboard_detect \
//       && /tmp/test_tdeck_keyboard_detect
#include <cassert>
#include <cstdio>
#include <string>
#include "Arduino.h"
#include "Wire.h"
#define HAS_TDECK_KEYBOARD 1
#define ESP32 1
uint32_t testMillis = 1000;
TestSerial Serial;
TwoWire Wire;
#include "../src/helpers/input/TDeckKeyboard.cpp"

using Frame = std::array<uint8_t, 5>;

static void poll(int n = 1) { for (int i = 0; i < n; ++i) { testMillis += 32; tdeckKeyboardPoll(); } }
static std::string keys() {
  std::string s;
  for (int k; (k = tdeckKeyboardReadKey()) != 0;) s.push_back((char)k);
  return s;
}
static void fresh() {
  Wire = TwoWire{};
  Serial.output.clear();
  (void)tdeckKeyboardTakeLegacyEvidence();
  tdeckKeyboardForceLegacy(false);
  tdeckKeyboardBegin();
}
// A controller that answered idle frames until the probe settled on raw, and one more
// idle frame to take the baseline.
static void toRaw() {
  fresh();
  for (int i = 0; i < 200 && tdeckKeyboardDetectedMode() != TDECK_KB_RAW; ++i) poll();
  assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW);
  poll();   // baseline frame
  assert(keys().empty());
}
static bool sentLegacyCommand() {
  for (uint8_t c : Wire.commands) if (c == 0x04) return true;
  return false;
}

int main() {
  // ===== the probe (first frames after boot) =====
  // 71 71 71 71 71 on the very first read: the older controller repeating 'q'.
  // Legacy, with the evidence the UI saves. The byte itself is the one the boot guard
  // drops (it may be a stale mailbox byte from before an ESP reset); the next press
  // comes through the one-byte protocol.
  {
    fresh();
    Wire.frames.push_back(Frame{0x71, 0x71, 0x71, 0x71, 0x71});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    assert(sentLegacyCommand());
    assert(Serial.output.find("repeated a character (0x71)") != std::string::npos);
    assert(tdeckKeyboardTakeLegacyEvidence());
    assert(!tdeckKeyboardTakeLegacyEvidence());   // taken once
    assert(keys().empty());
    Wire.legacy.push_back('w');
    poll();
    assert(keys() == "w");
  }
  // padded 71 00 00 00 00 in the probe: legacy, NOT saved (no evidence)
  {
    fresh();
    Wire.frames.push_back(Frame{0x71, 0, 0, 0, 0});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    assert(!tdeckKeyboardTakeLegacyEvidence());
  }
  // a burst in the probe that is not a repeat (8 bits, not printable): legacy, not saved
  {
    fresh();
    Wire.frames.push_back(Frame{0x0F, 0x0F, 0, 0, 0});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    assert(Serial.output.find("probe frame held too many keys") != std::string::npos);
    assert(!tdeckKeyboardTakeLegacyEvidence());
  }
  // a real key in column 1 during the probe: raw
  {
    fresh();
    Wire.frames.push_back(Frame{0, 0x01, 0, 0, 0});   // 'e'
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW);
  }

  // ===== the raw path (probe settled on raw, then a key) =====
  // 71 71 71 71 71: legacy + 'q' + evidence
  {
    toRaw();
    Wire.frames.push_back(Frame{0x71, 0x71, 0x71, 0x71, 0x71});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    assert(keys() == "q");
    assert(Serial.output.find(", repeated") != std::string::npos);
    assert(tdeckKeyboardTakeLegacyEvidence());
  }
  // padded 71 00 00 00 00: legacy + 'q', no evidence (not saved)
  {
    toRaw();
    Wire.frames.push_back(Frame{0x71, 0, 0, 0, 0});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    assert(keys() == "q");
    assert(!tdeckKeyboardTakeLegacyEvidence());
  }
  // two keys in the same row across columns (q + e, 01 01 00 00 00): stays raw, types both
  {
    toRaw();
    Wire.frames.push_back(Frame{0x01, 0x01, 0, 0, 0});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW);
    assert(keys() == "qe");
  }
  // q+space in column 0 and e+z in column 1 (21 21 00 00 00, two equal 2-bit columns): raw
  {
    toRaw();
    Wire.frames.push_back(Frame{0x21, 0x21, 0, 0, 0});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW);
    assert(keys().size() == 4);
  }
  // a printable 3-bit column 0 (q + w + space = '#') with another key in column 1: a
  // real chord, since a reply never puts a different byte after the character: raw
  {
    toRaw();
    Wire.frames.push_back(Frame{0x23, 0x01, 0, 0, 0});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW);
    assert(keys().size() == 4);
  }
  // ' ' repeated (20 20 20 20 20): legacy + ' ', evidence
  {
    toRaw();
    Wire.frames.push_back(Frame{0x20, 0x20, 0x20, 0x20, 0x20});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    assert(keys() == " ");
    assert(tdeckKeyboardTakeLegacyEvidence());
  }
  // space + z (20 20 00 00 00): a partial repeat of a 1-bit byte is two keys: raw
  {
    toRaw();
    Wire.frames.push_back(Frame{0x20, 0x20, 0, 0, 0});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW);
    assert(keys() == " z");
  }
  // the 8-key burst rule: 8 bits that are not a character reply -> legacy, nothing typed,
  // not saved; 7 bits -> still raw
  {
    toRaw();
    Wire.frames.push_back(Frame{0x01, 0x03, 0x07, 0x03, 0x00});   // 1+2+3+2 = 8 keys
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    assert(keys().empty());
    assert(Serial.output.find("raw frame held too many keys") != std::string::npos);
    assert(!tdeckKeyboardTakeLegacyEvidence());
  }
  {
    toRaw();
    Wire.frames.push_back(Frame{0x01, 0x03, 0x07, 0x01, 0x00});   // 7 keys
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW);
  }
  // a frame with a high bit set is not a frame: ignored, still raw
  {
    toRaw();
    Wire.frames.push_back(Frame{0xF1, 0xF1, 0xF1, 0xF1, 0xF1});
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_RAW && keys().empty());
  }
  // the setting forces legacy without a probe
  {
    fresh();
    tdeckKeyboardForceLegacy(true);
    poll();
    assert(tdeckKeyboardDetectedMode() == TDECK_KB_LEGACY);
    tdeckKeyboardForceLegacy(false);
  }

  // ===== every real chord of 1, 2 or 3 keys, exhaustively =====
  // All 35 matrix cells (modifiers included). No 1- or 2-key chord may read as a
  // character reply or a burst. 3-key chords that do are fumbles: three keys all in
  // column 0 (q, w, Sym, a, Alt, space, the row-6 key), one of them space or the row-6
  // key, since only bits 5 and 6 lift column 0's byte into the printable range.
  {
    struct K { int col, row; };
    K all[40]; int n = 0;
    const int rows[5] = {7, 7, 7, 7, 7};
    for (int c = 0; c < 5; ++c) for (int r = 0; r < rows[c]; ++r) {
      // the cells TDeckKeyboardState maps to a key, modifiers included (any bit can be set)
      all[n++] = {c, r};
    }
    int flagged2 = 0, flagged3 = 0, flagged3_not_col0 = 0, flagged3_no_space_row6 = 0;
    for (int a = 0; a < n; ++a) {
      uint8_t f[5] = {};
      f[all[a].col] |= (uint8_t)(1u << all[a].row);
      assert(!looksLikeAscii(f) && frameBits(f) < kBurstBits);
      for (int b = a + 1; b < n; ++b) {
        uint8_t g[5] = {}; memcpy(g, f, 5);
        g[all[b].col] |= (uint8_t)(1u << all[b].row);
        if (looksLikeAscii(g)) ++flagged2;
        for (int c = b + 1; c < n; ++c) {
          uint8_t h[5] = {}; memcpy(h, g, 5);
          h[all[c].col] |= (uint8_t)(1u << all[c].row);
          if (looksLikeAscii(h)) {
            ++flagged3;
            if (all[a].col || all[b].col || all[c].col) ++flagged3_not_col0;
            if (all[a].row < 5 && all[b].row < 5 && all[c].row < 5) ++flagged3_no_space_row6;
          }
        }
      }
    }
    assert(flagged2 == 0);
    assert(flagged3_not_col0 == 0 && flagged3_no_space_row6 == 0);
    printf("  3-key chords that read as a character reply: %d (all column 0, with space or the row-6 key)\n", flagged3);
  }
  // every printable character, repeated in all five bytes, is the older reply
  for (int ch = 0x20; ch <= 0x7E; ++ch) {
    uint8_t f[5] = {(uint8_t)ch, (uint8_t)ch, (uint8_t)ch, (uint8_t)ch, (uint8_t)ch};
    bool rep = false;
    assert(looksLikeAscii(f, &rep) && rep);
  }

  printf("test_tdeck_keyboard_detect: ok\n");
  return 0;
}
