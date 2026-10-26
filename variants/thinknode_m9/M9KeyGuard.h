// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which key bytes from the M9's keyboard controller are new presses (#625, #671).
// Pure, so the host test can drive it with a simulated controller
// (test/test_m9_keyboard.cpp); M9Keyboard.cpp feeds it every byte it reads.
//
// The controller (Elecrow's ThinkNode-M9-KB firmware, an ESP32-S2) scans the
// matrix every 10 ms with no debounce on the matrix keys, and latches a key byte
// on every released-to-pressed edge it sees, letters on the press itself. A
// contact that chatters as the key comes up (or drops out for one scan while it
// is held) is a second edge, so the same byte arrives again roughly as long after
// the first as the key was held: 100-250 ms on a slow, deliberate press. #628's
// 70 ms same-key guard caught the stale latch and a bounce on the way down, but
// not that one. Nothing on the wire says when a key went up (the controller has
// no key-state register), so the guard can only go by time:
//
//  - The same printable character again within the guard window is the same
//    press. The window follows the typing pace: three fifths of the usual gap
//    between two different characters, 70-250 ms (250 until there is a pace,
//    which is once after boot). A chatter repeat comes after about one hold
//    time, and a letter typed twice on purpose ("ll") after about one whole
//    keystroke, which is longer, at any pace.
//  - Arrows, Delete, Enter and the function keys keep the flat 70 ms: a list is
//    stepped with quick taps, and a held Delete repeats every 100 ms.

#include <stdint.h>

struct M9KeyGuard {
  uint8_t  last_key = 0;
  uint32_t last_ms  = 0;
  uint32_t pace_ms  = 0;   // smoothed gap between two different characters; 0 = not seen yet
};

static constexpr uint32_t kM9GuardMinMs     = 70;    // any key: the stale latch, a bounce going down
static constexpr uint32_t kM9GuardMaxMs     = 250;   // a character: never longer than this
static constexpr uint32_t kM9GuardDefaultMs = 250;   // a character, before the pace is known
static constexpr uint32_t kM9PaceIdleMs     = 1500;  // a longer gap is a pause, not the pace

inline bool m9KeyIsChar(uint8_t key) { return key >= 0x20 && key <= 0x7E; }

// The same-key window for a character at the current pace.
inline uint32_t m9KeyGuardWindow(const M9KeyGuard& g) {
  if (!g.pace_ms) return kM9GuardDefaultMs;
  uint32_t w = g.pace_ms * 3 / 5;
  if (w < kM9GuardMinMs) w = kM9GuardMinMs;
  if (w > kM9GuardMaxMs) w = kM9GuardMaxMs;
  return w;
}

// True when `key`, read at `now_ms`, is a new press. A repeat that is dropped does
// not move the timing: the window runs from the last ACCEPTED key, so a run of
// bounces cannot stretch it.
inline bool m9KeyGuardAccept(M9KeyGuard& g, uint8_t key, uint32_t now_ms) {
  const uint32_t gap = now_ms - g.last_ms;
  if (g.last_key && key == g.last_key) {
    const uint32_t window = m9KeyIsChar(key) ? m9KeyGuardWindow(g) : kM9GuardMinMs;
    if (gap < window) return false;
  } else if (g.last_key && m9KeyIsChar(key) && m9KeyIsChar(g.last_key) && gap < kM9PaceIdleMs) {
    // Two different characters in a row: that gap is one keystroke at this pace.
    g.pace_ms = g.pace_ms ? (g.pace_ms * 3 + gap) / 4 : gap;
  }
  g.last_key = key;
  g.last_ms  = now_ms;
  return true;
}
