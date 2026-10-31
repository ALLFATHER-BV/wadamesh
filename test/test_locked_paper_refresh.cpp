// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the retained lock frame's periodic repaint on e-paper:
// src/ui-touch/LockedPaperRefresh.h, the rule UITask's lock-screen service applies
// while the screen is off on the T-Deck Pro and Max.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_locked_paper_refresh.cpp \
//       -o /tmp/test_locked_paper_refresh && /tmp/test_locked_paper_refresh
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ui-touch/LockedPaperRefresh.h"

using namespace LockedPaperRefresh;

// Run the rule over [from, to) at `step` ms with the screen constantly off and
// return every time it asked for a repaint.
static int repaints(const State& start, uint32_t from, uint32_t to, uint32_t step) {
  State s = start;
  int n = 0;
  for (uint32_t now = from; now < to; now += step) {
    if (due(s, now, true)) ++n;
  }
  return n;
}

int main() {
  // ---- a lit screen never repaints, and clears the off state ----
  {
    State s;
    assert(!due(s, 1000, false));
    assert(!due(s, 1000 + kIntervalMs, false));
    assert(!s.was_off);
  }

  // ---- going dark arms the clock; the first repaint is a full interval later ----
  {
    State s;
    const uint32_t off_at = 5000;
    assert(!due(s, off_at, true));             // armed, not due: lockScreen() just painted
    assert(s.was_off);
    assert(!due(s, off_at + kIntervalMs - 1, true));
    assert(due(s, off_at + kIntervalMs, true));
    // And then the next one is another interval away.
    assert(!due(s, off_at + kIntervalMs + 1, true));
    assert(due(s, off_at + 2 * kIntervalMs, true));
  }

  // ---- a long pass past the deadline is still one repaint, not a burst ----
  {
    State s;
    assert(!due(s, 0, true));
    assert(due(s, kIntervalMs + 90000, true));   // 1.5 minutes late
    assert(!due(s, kIntervalMs + 90001, true));
  }

  // ---- the pass after the due one, at the same timestamp, does not fire again ----
  {
    State s;
    assert(!due(s, 0, true));
    assert(due(s, kIntervalMs, true));
    assert(!due(s, kIntervalMs, true));          // re-armed from this pass
  }

  // ---- a reveal longer than the interval cannot burst on re-dark: the next
  //      repaint waits a full interval from going dark again ----
  {
    State s;
    assert(!due(s, 0, true));
    assert(due(s, kIntervalMs, true));
    assert(!due(s, kIntervalMs + 10, false));            // lit...
    assert(!due(s, 3 * kIntervalMs, false));             // ...well past one interval
    assert(!due(s, 3 * kIntervalMs + 1, true));          // dark again: armed afresh
    assert(!due(s, 4 * kIntervalMs, true));              // a ms short of the fresh deadline
    assert(due(s, 4 * kIntervalMs + 1, true));
  }

  // ---- a reveal and re-lock restart the interval ----
  {
    State s;
    assert(!due(s, 0, true));
    assert(due(s, kIntervalMs, true));
    assert(!due(s, kIntervalMs + 1000, false));  // looking at it: lit
    assert(!due(s, kIntervalMs + 2000, true));   // dark again: armed afresh
    assert(!due(s, kIntervalMs + 2000 + kIntervalMs - 1, true));
    assert(due(s, kIntervalMs + 2000 + kIntervalMs, true));
  }

  // ---- the millis() wrap: armed before the wrap, due after it ----
  {
    State s;
    const uint32_t off_at = 0xFFFFFF00u;   // 256 ms before the wrap
    assert(!due(s, off_at, true));
    assert(!due(s, 0x00000000u, true));    // only 256 ms later
    const uint32_t due_at = off_at + kIntervalMs;   // wrapped
    assert(!due(s, due_at - 1, true));
    assert(due(s, due_at, true));
  }

  // ---- the wrap the other way: deadline before it, the first late pass after it ----
  {
    State s;
    const uint32_t deadline = 0xFFFFFF00u;            // armed a whole interval earlier
    assert(!due(s, deadline - kIntervalMs, true));    // armed here
    assert(!due(s, deadline - 1, true));              // 1 ms short
    assert(due(s, 0x00000010u, true));                // now has wrapped: 272 ms late, one repaint
    assert(!due(s, 0x00000011u, true));               // re-armed from that pass
  }

  // ---- over a simulated day it fires every five minutes: 288 times ----
  {
    State s;
    assert(!due(s, 0, true));
    const int n = repaints(s, 0, 24u * 60u * 60u * 1000u + 1u, 50);
    assert(n == 24 * 12);
  }

  // ---- reset() forgets everything (the overlay was rebuilt) ----
  {
    State s;
    assert(!due(s, 0, true));
    assert(due(s, kIntervalMs, true));
    reset(s);
    assert(!s.was_off && s.next_ms == 0);
    assert(!due(s, kIntervalMs * 2, true));      // armed again, not due
  }

  printf("test_locked_paper_refresh: all passed\n");
  return 0;
}
