// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the clock floor (src/helpers/ClockFloorRTC.h), with #756's change:
// a hardware clock that reads behind the floor or in the far future gives way to the
// system clock when that is ahead and plausible (T-Display P4 backups named "nodate"
// after a good Wi-Fi sync, #746). The core's AutoDiscoverRTCClock is a settable fake
// (test/clock_hw_fakes) and settimeofday / gettimeofday are faked, so the test
// never touches the machine's clock.
//
//   c++ -std=c++17 -Wall -Wextra -I src -I test/clock_hw_fakes test/test_clock_floor.cpp \
//       -o /tmp/test_clock_floor && /tmp/test_clock_floor
#include <assert.h>
#include <stdio.h>
#include <sys/time.h>

static long g_sys = 0;          // the system clock, seconds
static int g_sys_sets = 0;
static int fakeSetTimeOfDay(const struct timeval* tv, const void*) { g_sys = tv->tv_sec; ++g_sys_sets; return 0; }
static int fakeGetTimeOfDay(struct timeval* tv, void*) { tv->tv_sec = g_sys; tv->tv_usec = 0; return 0; }
#define settimeofday fakeSetTimeOfDay
#define gettimeofday fakeGetTimeOfDay
#include "helpers/ClockFloorRTC.h"
#undef settimeofday
#undef gettimeofday

struct NullClock : mesh::RTCClock {
  uint32_t getCurrentTime() override { return 0; }
  void setCurrentTime(uint32_t) override {}
};

using C = ClockFloorRTC;
static const uint32_t MIN = C::MIN_VALID_EPOCH, MAX = C::MAX_PLAUSIBLE_EPOCH;
static const uint32_t NOW = 1790000000u;   // 21 Sep 2026: valid, below MAX
static const uint32_t FUTURE_2043 = 2324000000u;

struct Rig {
  NullClock fb;
  C clk{fb};
  AutoDiscoverRTCClock& hw() { return static_cast<AutoDiscoverRTCClock&>(clk); }
};

int main() {
  assert(MIN < NOW && NOW < MAX && MAX < FUTURE_2043);

  // A healthy chip ahead of the floor: its time, the floor follows.
  { Rig r; g_sys = 0; r.hw().hw = NOW; assert(r.clk.getCurrentTime() == NOW && r.clk.getFloor() == NOW); }

  // #756: the chip reads BEHIND the floor (stopped oscillator, a write that did not
  // take) while the system clock runs ahead: the system clock wins and lifts the floor.
  { Rig r; r.clk.seedFloor(NOW); r.hw().hw = NOW - 3600; g_sys = NOW + 120;
    assert(r.clk.getCurrentTime() == NOW + 120 && r.clk.getFloor() == NOW + 120); }
  // ...behind the floor, and the system clock is not ahead: hold the floor (as before)
  { Rig r; r.clk.seedFloor(NOW); r.hw().hw = NOW - 3600; g_sys = NOW - 10;
    assert(r.clk.getCurrentTime() == NOW); }
  // ...behind the floor, system clock on an implausible value: hold the floor
  { Rig r; r.clk.seedFloor(NOW); r.hw().hw = NOW - 3600; g_sys = FUTURE_2043;
    assert(r.clk.getCurrentTime() == NOW); }
  { Rig r; r.clk.seedFloor(NOW); r.hw().hw = NOW - 3600; g_sys = -5;   // gettimeofday < 0
    assert(r.clk.getCurrentTime() == NOW); }

  // A chip asserting 2043: the system clock when it is ahead and plausible...
  { Rig r; r.clk.seedFloor(NOW); r.hw().hw = FUTURE_2043; g_sys = NOW + 60;
    assert(r.clk.getCurrentTime() == NOW + 60); }
  // ...else the floor, or the core's unset seed when there is no floor yet (as before)
  { Rig r; r.clk.seedFloor(NOW); r.hw().hw = FUTURE_2043; g_sys = MIN - 100;
    assert(r.clk.getCurrentTime() == NOW); }
  { Rig r; r.hw().hw = FUTURE_2043; g_sys = MIN;   // the system clock on the seed is not "ahead"
    assert(r.clk.getCurrentTime() == MIN); }
  { Rig r; r.hw().hw = FUTURE_2043; g_sys = 0;
    assert(r.clk.getCurrentTime() == MIN && r.clk.getFloor() == 0); }

  // The #746 story: Wi-Fi sync lands, the chip write does not take, the chip stays on
  // its stale reading. Every later read follows the running system clock (the backup
  // name gets its date), and never goes backward.
  {
    Rig r;
    r.hw().hw = MIN;                 // the stale seed the chip came back with
    r.hw().hw_write_takes = false;
    g_sys = MIN; g_sys_sets = 0;
    r.clk.setCurrentTime(NOW);       // the sync: system clock set, chip write lost
    assert(g_sys == (long)NOW && g_sys_sets == 1 && r.hw().hw == MIN);
    assert(r.clk.timeIsCurrent());
    uint32_t last = 0;
    for (int i = 0; i < 100; ++i) {
      g_sys += 37;                   // time passes on the system clock only
      const uint32_t t = r.clk.getCurrentTime();
      assert(t == (uint32_t)g_sys && t >= last);
      last = t;
    }
  }

  // A healthy chip whose write takes: after a set it reads the new time and is used.
  {
    Rig r; r.hw().hw = MIN; g_sys = MIN;
    r.clk.setCurrentTime(NOW);
    assert(r.hw().hw == NOW && r.clk.getCurrentTime() == NOW);
    r.hw().hw = NOW + 5; g_sys = NOW + 4;   // the chip a second ahead of the system clock
    assert(r.clk.getCurrentTime() == NOW + 5);
  }

  // Monotonic whatever the two clocks do: a random walk of both never makes it step back.
  {
    Rig r; r.clk.seedFloor(NOW);
    uint32_t x = 12345, last = 0;
    for (int i = 0; i < 20000; ++i) {
      x = x * 1664525u + 1013904223u;
      r.hw().hw = (x & 1) ? NOW + (x >> 20) : ((x & 2) ? FUTURE_2043 : NOW - (x >> 20));
      g_sys = (x & 4) ? (long)(NOW + ((x >> 8) & 0xFFFF)) : (long)(MIN - 1);
      const uint32_t t = r.clk.getCurrentTime();
      assert(t >= last && t <= MAX);
      last = t;
    }
  }

  // setCurrentTime: garbage sets are refused (no chip write, no system clock set)
  {
    Rig r; g_sys_sets = 0;
    r.clk.setCurrentTime(MIN);            // the seed itself: the "Sync from system" poisoning
    r.clk.setCurrentTime(1000);           // 1970-class
    r.clk.setCurrentTime(FUTURE_2043);
    assert(r.hw().writes == 0 && g_sys_sets == 0 && !r.clk.timeIsCurrent());
  }
  // a >10 min backward correction pulls the floor down; a smaller one keeps it
  {
    Rig r; r.clk.seedFloor(NOW + 3600);
    r.clk.setCurrentTime(NOW);
    assert(r.clk.getFloor() == NOW);
    Rig q; q.clk.seedFloor(NOW + 300);
    q.clk.setCurrentTime(NOW);
    assert(q.clk.getFloor() == NOW + 300);
  }
  // a poisoned persisted floor is refused
  { Rig r; r.clk.seedFloor(FUTURE_2043); assert(r.clk.getFloor() == 0); }

  printf("test_clock_floor: ok\n");
  return 0;
}
