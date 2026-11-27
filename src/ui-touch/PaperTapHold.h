// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// =============================================================================
// When the e-paper boards (T-Deck Pro and Max) should wait before committing a
// frame to the panel. Every panel update blocks the UI for most of a second, and a
// tap used to cost two of them: the pressed look of whatever was touched, then
// the screen the tap opened once that first update let go of the loop.
//
// So a frame is held back while it is about to be replaced:
//   - while LVGL has a pointer pressed and has not sent its long press yet
//     (and never past kPressMaxMs, so a drag still shows). LVGL's state, not
//     the touch controller's: a tap made during a panel update reaches LVGL
//     only afterwards, with the finger long gone.
//   - while LVGL has changes it has not drawn yet (up to kUndrawnMaxMs, so
//     something that redraws constantly cannot starve the panel). This covers
//     the release itself: the screen a tap opens is invalid until it is drawn.
// A tap then shows in one update, the screen it opened. A press that changes
// nothing leaves the frame as the panel already has it, and costs no update.
//
// Pure, so the host test (test/test_paper_tap_hold.cpp) runs the same rule the
// board does.
// =============================================================================

#include <stdint.h>

namespace PaperTapHold {

constexpr uint32_t kPressMaxMs   = 500;
constexpr uint32_t kUndrawnMaxMs = 300;

struct State {
  uint32_t undrawn_ms = 0;     // when undrawn changes were first seen (valid while undrawn)
  bool     undrawn = false;
};

// Call once per UI pass, after LVGL has run. `pressed_ms` is how long LVGL has
// had a pointer pressed without its long press sent (0 when none is), `undrawn`
// whether LVGL still has invalid areas. Returns true when this pass should not commit a frame.
inline bool hold(State& s, uint32_t now, uint32_t pressed_ms, bool undrawn) {
  if (pressed_ms > 0 && pressed_ms < kPressMaxMs) return true;
  if (!undrawn) {
    s.undrawn = false;
    return false;
  }
  if (!s.undrawn) {
    s.undrawn = true;
    s.undrawn_ms = now;
  }
  // Once the limit has passed this stays false until LVGL has caught up.
  return now - s.undrawn_ms < kUndrawnMaxMs;
}

}  // namespace PaperTapHold
