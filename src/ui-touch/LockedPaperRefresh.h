// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// =============================================================================
// Periodic repaint of the retained lock frame on the e-paper boards (T-Deck Pro
// and Max). The panel keeps its last frame with the power off, so a locked device
// would otherwise show the clock and the unread cards from the moment it locked
// for as long as it stays parked. This is the rule UITask's lock-screen service
// applies: repaint once every five minutes while the screen is off, and never
// while it is lit.
//
// Pure, so the host test (test/test_locked_paper_refresh.cpp) runs the same rule
// the board does, including the millis() wrap.
// =============================================================================

#include <stdint.h>

namespace LockedPaperRefresh {

// How stale a retained lock frame may get. Five minutes keeps the clock close
// enough to glance at, while the panel refresh (and the CPU wake around it) is
// rare next to the idle loop's 50 ms passes.
constexpr uint32_t kIntervalMs = 5u * 60u * 1000u;

struct State {
  uint32_t next_ms = 0;   // when the next repaint is due (valid once was_off)
  bool     was_off = false;
};

// Forget the timing: the overlay was rebuilt, or torn down.
inline void reset(State& s) { s = State{}; }

// Call once per UI pass with the screen's on/off state. Returns true when the
// retained frame should be repainted now. The clock is armed on the pass that
// first sees the screen off, so a fresh lock is not repainted immediately (the
// lock view was just committed). A lit reveal never repaints; going dark again
// re-arms the interval from that moment, so the frame is never older than one
// interval past the last time the screen was actually dark.
inline bool due(State& s, uint32_t now, bool screen_off) {
  if (!screen_off) {
    s.was_off = false;
    return false;
  }
  if (!s.was_off) {
    s.was_off = true;
    s.next_ms = now + kIntervalMs;
    return false;
  }
  // Signed compare: works across the millis() wrap, and a pass that overran the
  // deadline by any amount is still exactly one repaint.
  if ((int32_t)(now - s.next_ms) < 0) return false;
  s.next_ms = now + kIntervalMs;
  return true;
}

}  // namespace LockedPaperRefresh
