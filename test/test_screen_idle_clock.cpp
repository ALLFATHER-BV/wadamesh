// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the screen timeout's idle clock (#762): src/ui-touch/ScreenIdleClock.h,
// the rule UITask::loop applies before it dims (or, on the T-Deck Pro, locks) the screen.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_screen_idle_clock.cpp \
//       -o /tmp/test_screen_idle_clock && /tmp/test_screen_idle_clock
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ui-touch/ScreenIdleClock.h"

using namespace ScreenIdleClock;

// One boot, as #762's log reads it (ms since the UI's begin() stamped the last
// input): the history load and the rest of the build take ~30 s, "ui ready", the
// first loop pass, then the splash's one still frame until "splash done" 8.3 s after
// it was built. Loop passes every 20 ms. Returns the first pass the timeout fires on
// (0 = never before t_end). `hold` = the fix.
struct Boot {
  uint32_t stamp      = 0;       // begin(): _last_input_ms = millis()
  uint32_t first_pass = 31000;   // first UITask::loop pass, splash just built
  uint32_t splash_end = 39300;   // "[BOOT] splash done"
};
static uint32_t firstTimeout(const Boot& b, uint32_t timeout_ms, bool hold, uint32_t t_end,
                             uint32_t input_at = 0) {
  unsigned long last_input = b.stamp;   // the member is unsigned long (64-bit on the host)
  for (uint32_t now = b.first_pass; now <= t_end; now += 20) {
    if (input_at && now == input_at) last_input = now;   // a key press
    if (hold) holdWhileSplash(last_input, now, now < b.splash_end);
    if (due(now, (uint32_t)last_input, timeout_ms)) return now;
  }
  return 0;
}

int main() {
  const uint32_t kTimeout = 30000;   // DEFAULT_SCREEN_TIMEOUT_S
  Boot slow;

  // ---- the bug: the boot counted as idle ----
  // Before the fix the very first pass timed out, with the splash still on screen:
  // on the Pro that is lockScreen() committing the splash frame to the e-paper.
  assert(firstTimeout(slow, kTimeout, false, 120000) == slow.first_pass);

  // ---- the fix: the clock starts when the splash goes ----
  const uint32_t fired = firstTimeout(slow, kTimeout, true, 120000);
  // (The clock starts from the last pass that still had the splash, one pass early.)
  assert(fired + 20 >= slow.splash_end + kTimeout);
  assert(fired < slow.splash_end + kTimeout + 40);   // and on time, the next pass or so
  // Never while the splash is up, however slow the boot.
  {
    Boot glacial;
    glacial.first_pass = 95000;    // a minute and a half to the first pass
    glacial.splash_end = 110000;   // the splash's own 15 s wall-clock cap
    const uint32_t f = firstTimeout(glacial, kTimeout, true, 200000);
    assert(f + 20 >= glacial.splash_end + kTimeout);
  }

  // ---- a fast boot: the full wait still runs from when the UI shows ----
  {
    Boot fast;
    fast.first_pass = 3000;
    fast.splash_end = 6000;
    const uint32_t f = firstTimeout(fast, kTimeout, true, 120000);
    assert(f + 20 >= fast.splash_end + kTimeout && f < fast.splash_end + kTimeout + 40);
  }

  // ---- input after the splash restarts the wait ----
  {
    const uint32_t key = slow.splash_end + 20000;   // a key 20 s after the UI shows
    const uint32_t f = firstTimeout(slow, kTimeout, true, 200000, key);
    assert(f >= key + kTimeout && f < key + kTimeout + 40);
  }

  // ---- timeout "never" (0) never fires ----
  assert(firstTimeout(slow, 0, true, 300000) == 0);
  assert(firstTimeout(slow, 0, false, 300000) == 0);

  // ---- the signed compare: an input stamped just after the pass's `now` ----
  assert(!due(1000, 1005, kTimeout));            // 5 ms "in the future" = just now
  assert(!due(0xFFFFFFF0u, 0x00000010u, kTimeout));   // across the millis() wrap
  assert(due(0x00000010u + kTimeout, 0x00000010u, kTimeout));

  printf("test_screen_idle_clock: all passed\n");
  return 0;
}
