// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// =============================================================================
// The screen timeout's idle clock (UITask::loop). Pure, so the host test
// (test/test_screen_idle_clock.cpp) runs the same rule the board does.
//
// begin() stamps the last input when it starts building the UI, and a boot can
// take longer than that: loading a full chat history from internal flash, an SD
// card that is not there, an e-paper panel's refreshes. Counting that time as
// idle made a boot that outlasted the timeout (30 s by default) time out on the
// first loop pass, before anyone had seen the UI. On the T-Deck Pro the timeout
// is a lock, which commits what is on top to the e-paper and powers it down:
// the boot splash, so the panel stayed on the logo while the device ran (#762).
//
// So the clock stands still while the boot splash is up and starts when the UI
// is shown. Same signed compare as before: an input stamped a few ms after the
// pass's `now` counts as "just now", never as a wrapped four-billion ms.
// =============================================================================

#include <stdint.h>

namespace ScreenIdleClock {

// Once per loop pass, before the timeout check.
template <typename Stamp>
inline void holdWhileSplash(Stamp& last_input_ms, uint32_t now, bool splash_up) {
  if (splash_up) last_input_ms = (Stamp)now;
}

inline bool due(uint32_t now, uint32_t last_input_ms, uint32_t timeout_ms) {
  return timeout_ms > 0 && (int32_t)(now - last_input_ms) >= (int32_t)timeout_ms;
}

}  // namespace ScreenIdleClock
