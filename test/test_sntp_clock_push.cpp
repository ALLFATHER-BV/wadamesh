// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for src/helpers/esp32/SntpClockPush.h (Discord, ThinkNode M9: "sync clock from
// system with Wi-Fi on does not always set it"). Simulates the Wi-Fi loop of src/main.cpp:
// SNTP results arrive once at association and then every hour; a clock with an RTC chip
// (the M9's PCF8563) pulls the system clock back to the chip once an hour (#583); the chip is
// written only when the mesh clock is set. Checks that every SNTP result now reaches the mesh
// clock (the old loop pushed the first one only), so the hourly chip resync can no longer
// undo a correction, and that the "Sync clock from system" request restarts SNTP and reports.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_sntp_clock_push.cpp -o /tmp/t && /tmp/t
#include <assert.h>
#include <stdio.h>
#include "helpers/esp32/SntpClockPush.h"

static const uint32_t MIN_VALID = 1772323200u, MAX_OK = 1893456000u;

// The M9 clock stack, as far as this question goes. All three clocks run at the same rate;
// what differs is who writes which.
struct M9Clock {
  long truth = 0;  // real UTC
  long sys = 0;    // ESP32 system clock (the UI shows this; SNTP sets it)
  long chip = 0;   // PCF8563 (written by rtc_clock.setCurrentTime only)
  void advance(long dt) { truth += dt; sys += dt; chip += dt; }
  void meshSet(long t) { sys = t; chip = t; }      // HardwareRtcClock::setCurrentTime
  void sntpSet() { sys = truth; }                  // lwIP SNTP: settimeofday only
  void hourlyChipResync() { sys = chip; }          // HardwareRtcClock::resyncFromChip (|delta| >= 2 s)
};

// One association, `hours` long: an SNTP result at the top of every hour, the chip resync at
// half past. In hour `stale_at_h`, ten minutes in, a stale set (the pre-fix GPS wake) moves
// the mesh clock 30 min back. Returns how far behind the clock ends, in seconds, right after
// the final chip resync. `old_loop` = the pre-fix rule (push the first result only).
static long simulate(bool old_loop, int hours, int stale_at_h) {
  M9Clock c;
  c.truth = 1793017200;               // 2026-10-26 12:20Z
  c.meshSet(c.truth - 3000);          // came up 50 min behind (stale floor after a power cut)
  SntpClockPush p;
  bool old_pushed = false;
  const bool kicked = p.wantKick();
  assert(kicked);
  for (int h = 0; h <= hours; ++h) {
    c.sntpSet();                      // SNTP result
    if (old_loop) { if (!old_pushed) { c.meshSet(c.sys); old_pushed = true; } }
    else if (p.shouldPush(true, (uint32_t)c.sys, MIN_VALID, MAX_OK, nullptr)) c.meshSet(c.sys);
    c.advance(600);
    if (h == stale_at_h) c.meshSet(c.truth - 1800);
    c.advance(1200);
    c.hourlyChipResync();
    if (h < hours) c.advance(1800);
  }
  return c.truth - c.sys;
}

int main() {
  // ---- the state machine ----
  {
    SntpClockPush p;
    bool report = true;
    assert(!p.shouldPush(true, MIN_VALID + 10, MIN_VALID, MAX_OK, &report) && !report);   // not started
    assert(p.wantKick() && !p.wantKick());                                  // one start per association
    assert(!p.shouldPush(false, MIN_VALID + 10, MIN_VALID, MAX_OK, &report));   // no result yet
    assert(!p.shouldPush(true, MIN_VALID, MIN_VALID, MAX_OK, &report));         // the unset seed: never
    assert(!p.shouldPush(true, MAX_OK + 1, MIN_VALID, MAX_OK, &report));        // garbage future: never
    assert(p.shouldPush(true, MIN_VALID + 10, MIN_VALID, MAX_OK, &report) && !report);   // first result
    assert(p.shouldPush(true, MIN_VALID + 3610, MIN_VALID, MAX_OK, &report) && !report); // hourly: again
    p.linkDown();                                                           // after a result: SNTP keeps going
    assert(!p.wantKick());
    // The user's "Sync clock from system": restart now, and the next result is reported.
    p.rekick = true;
    assert(p.wantKick() && !p.wantKick());
    assert(!p.shouldPush(false, MIN_VALID + 3700, MIN_VALID, MAX_OK, &report) && !report);
    assert(p.shouldPush(true, MIN_VALID + 3720, MIN_VALID, MAX_OK, &report) && report);
    assert(p.shouldPush(true, MIN_VALID + 7300, MIN_VALID, MAX_OK, &report) && !report);   // once
  }
  {
    SntpClockPush p;
    assert(p.wantKick());
    p.linkDown();                       // dropped before any result: start again next time
    assert(p.wantKick());
  }

  // ---- the M9 timeline ----
  // Pre-fix loop: the first result is pushed, a later stale set puts the chip 30 min behind,
  // and every hour the chip resync pulls the SNTP-corrected system clock back to it.
  const long old_lag = simulate(true, 6, 2);
  // Now: the next hourly result writes through to the chip, so the clock ends on time.
  const long new_lag = simulate(false, 6, 2);
  printf("  M9, stale set in hour 2, 6 h on Wi-Fi: pre-fix loop ends %ld s behind, now %ld s\n", old_lag, new_lag);
  assert(old_lag == 1800);
  assert(new_lag == 0);
  assert(simulate(false, 1, -1) == 0 && simulate(true, 1, -1) == 0);   // no stale set: both fine

  printf("test_sntp_clock_push: all passed\n");
  return 0;
}
