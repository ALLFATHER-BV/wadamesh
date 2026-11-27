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
#include "helpers/ClockRestartCarry.h"

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

  // adoptMeshPeerTime (#835): two different verified keys within 15 min for EVERY step,
  // the lower of the two wins, _source stays Floor, nothing reaches the RTC chip.
  {
    Rig r; r.hw().hw = MIN; g_sys = MIN; g_sys_sets = 0;
    r.clk.seedFloor(NOW);
    r.clk.seedSystemClock();
    assert(!r.clk.timeIsCurrent());
    // One key, even a small step and even repeated, never moves the clock
    assert(!r.clk.adoptMeshPeerTime(NOW + 300, 0xAAAA0001u));
    assert(!r.clk.adoptMeshPeerTime(NOW + 600, 0xAAAA0001u));
    assert(!r.clk.adoptMeshPeerTime(NOW + 900, 0xAAAA0001u));
    assert(r.clk.getFloor() == NOW);
    // No verified identity (a channel post) never counts, not even as the second witness
    assert(!r.clk.adoptMeshPeerTime(NOW + 900, 0));
    assert(r.clk.getFloor() == NOW);
    // A second key far from the first is not a quorum; it becomes the new first witness
    assert(!r.clk.adoptMeshPeerTime(NOW + 172800, 0xBAD0BEEFu));
    assert(r.clk.getFloor() == NOW);
    // ...and the old first key agreeing with neither changes nothing
    assert(!r.clk.adoptMeshPeerTime(NOW + 21600, 0x11111111u));
    assert(r.clk.getFloor() == NOW);
    // Two different keys 20 s apart: the lower one is adopted
    assert(r.clk.adoptMeshPeerTime(NOW + 21620, 0x22222222u));
    assert(r.clk.getFloor() == NOW + 21600 && g_sys == (long)(NOW + 21600));
    assert(!r.clk.timeIsCurrent() && r.clk.source() == C::ClockSource::Floor && r.hw().writes == 0);
    // Only ever forward: two keys agreeing on an earlier time do nothing
    assert(!r.clk.adoptMeshPeerTime(NOW + 100, 0x33333333u));
    assert(!r.clk.adoptMeshPeerTime(NOW + 110, 0x44444444u));
    assert(r.clk.getFloor() == NOW + 21600);
    // Garbage times are refused before they can even be a witness
    assert(!r.clk.adoptMeshPeerTime(FUTURE_2043, 0x55555555u));
    assert(!r.clk.adoptMeshPeerTime(FUTURE_2043, 0x66666666u));
    assert(r.clk.getFloor() == NOW + 21600);
    // A real sync later corrects a wrong mesh time (>10 min back pulls the floor down)
    r.clk.setCurrentTime(NOW + 60);
    assert(r.clk.getFloor() == NOW + 60 && r.clk.timeIsCurrent());
    // ...and once the time is current, peers are ignored
    assert(!r.clk.adoptMeshPeerTime(NOW + 7200, 0x77777777u));
    assert(!r.clk.adoptMeshPeerTime(NOW + 7210, 0x88888888u));
    assert(r.clk.getFloor() == NOW + 60);
  }
  // Seed-only boot (no persisted floor) follows the same quorum rule
  {
    Rig r; r.hw().hw = MIN; g_sys = MIN;
    r.clk.seedSystemClock();
    assert(r.clk.source() == C::ClockSource::Seed);
    assert(!r.clk.adoptMeshPeerTime(NOW, 0x1u));
    assert(r.clk.adoptMeshPeerTime(NOW + 5, 0x2u));
    assert(r.clk.getFloor() == NOW && r.clk.source() == C::ClockSource::Floor && r.hw().writes == 0);
  }
  // A good hardware clock is never moved by the mesh
  {
    Rig r; r.hw().hw = NOW; g_sys = MIN;
    r.clk.noteHardwareClock(true);
    r.clk.noteHardwareTime();
    assert(!r.clk.adoptMeshPeerTime(NOW + 86400, 0x1u));
    assert(!r.clk.adoptMeshPeerTime(NOW + 86400, 0x2u));
    assert(r.clk.getCurrentTime() == NOW);
  }

  // raiseFloorFromLocalRecord (#835/#840): our own records may lift the floor, but only
  // within LOCAL_RECORD_WINDOW_SECS above it, only while the time is not current, and
  // only on top of a real persisted floor.
  {
    const uint32_t W = C::LOCAL_RECORD_WINDOW_SECS;
    { Rig r; r.hw().hw = MIN; g_sys = MIN; r.clk.seedFloor(NOW); r.clk.seedSystemClock();
      assert(r.clk.localRecordCeiling() == NOW + W);
      assert(!r.clk.raiseFloorFromLocalRecord(NOW - 10));          // behind: nothing to do
      assert(!r.clk.raiseFloorFromLocalRecord(NOW + W + 1));       // too far: a corrected clock
      assert(r.clk.raiseFloorFromLocalRecord(NOW + 600));
      assert(r.clk.getFloor() == NOW + 600);
      r.clk.seedSystemClock();
      assert(g_sys == (long)(NOW + 600) && r.clk.source() == C::ClockSource::Floor && r.hw().writes == 0); }
    // no persisted floor: no raise at all
    { Rig r; r.hw().hw = MIN; g_sys = MIN; r.clk.seedSystemClock();
      assert(r.clk.localRecordCeiling() == 0 && !r.clk.raiseFloorFromLocalRecord(NOW)); }
    // time already current (good chip): no raise
    { Rig r; r.hw().hw = NOW; g_sys = MIN; r.clk.seedFloor(NOW - 60);
      r.clk.noteHardwareClock(true); r.clk.noteHardwareTime();
      assert(r.clk.localRecordCeiling() == 0 && !r.clk.raiseFloorFromLocalRecord(NOW + 60)); }
  }

  // Discord, beta_108: a floor 14 days ahead (11 Dec on 27 Nov). Something stamped
  // before the sync, then Wi-Fi time landed: the status bar was right, every sent
  // message still said 11 Dec (the core's unique ratchet), and the next boot brought
  // the floor back. The trusted backward set resets the ratchet and counts a step back
  // for the persister and the DM ratchet.
  {
    Rig r;
    const uint32_t AHEAD = NOW + 14u * 86400u;
    r.clk.seedFloor(AHEAD); r.hw().hw = NOW; g_sys = NOW;
    assert(r.clk.getCurrentTimeUnique() == AHEAD);       // the boot advert, on the bad floor
    assert(r.clk.getCurrentTimeUnique() == AHEAD + 1);
    assert(r.clk.stepBacks() == 0);
    r.clk.setCurrentTime(NOW + 5);                       // the NTP sync
    assert(r.clk.getFloor() == NOW + 5 && r.clk.stepBacks() == 1);
    assert(r.clk.getCurrentTimeUnique() == NOW + 5);     // the next message has the real date
    assert(r.clk.getCurrentTimeUnique() == NOW + 6);     // and is still unique
    r.clk.setCurrentTime(NOW + 30);                      // a small forward correction: no step back
    assert(r.clk.stepBacks() == 1);
    r.clk.setCurrentTime(NOW + 30 - 5 * 60);             // inside TRUSTED_BACK_CAP: the floor holds
    assert(r.clk.stepBacks() == 1 && r.clk.getFloor() == NOW + 30);
  }
  // Our own per-sender ratchets give way the same way.
  {
    uint32_t last = NOW + 14u * 86400u;
    assert(core_clock_ratchets::nextSendTs(NOW, last) == NOW && last == NOW);
    assert(core_clock_ratchets::nextSendTs(NOW, last) == NOW + 1);
    assert(core_clock_ratchets::nextSendTs(NOW - 60, last) == NOW + 2);   // a small step back stays monotonic
  }

  // A restart the board vouched for (noteRestartCarry): the restored time stays
  // current under its own source name, instead of dropping to the floor.
  {
    // No chip: the fallback is the system clock, restored a few seconds past the floor
    Rig r; r.hw().hw = NOW; g_sys = NOW; r.clk.seedFloor(NOW - 5);
    r.clk.noteRestartCarry();
    r.clk.seedSystemClock();
    assert(r.clk.source() == C::ClockSource::Restart && r.clk.timeIsCurrent());
    assert(r.clk.getCurrentTime() == NOW && r.hw().writes == 0);
    // A later seedSystemClock() (the history-record raise) does not demote it
    r.clk.seedSystemClock();
    assert(r.clk.source() == C::ClockSource::Restart);
    // The time is current, so neither the mesh nor our own records move it
    assert(!r.clk.adoptMeshPeerTime(NOW + 7200, 0x1u));
    assert(!r.clk.adoptMeshPeerTime(NOW + 7210, 0x2u));
    assert(r.clk.localRecordCeiling() == 0);
    // A real sync still outranks it
    r.clk.setCurrentTime(NOW + 30);
    assert(r.clk.source() == C::ClockSource::ExternalSync);
  }
  // Without the board's word the same boot is the floor, as before
  { Rig r; r.hw().hw = NOW; g_sys = NOW; r.clk.seedFloor(NOW - 5); r.clk.seedSystemClock();
    assert(r.clk.source() == C::ClockSource::Floor && !r.clk.timeIsCurrent()); }
  // A restored seed stays a seed, whatever the board said
  { Rig r; r.hw().hw = MIN; g_sys = MIN; r.clk.noteRestartCarry(); r.clk.seedSystemClock();
    assert(r.clk.source() == C::ClockSource::Seed && !r.clk.timeIsCurrent()); }
  // A healthy chip ahead of the floor is still what it is
  { Rig r; r.hw().hw = NOW; g_sys = MIN; r.clk.seedFloor(NOW - 60);
    r.clk.noteHardwareClock(true); r.clk.noteRestartCarry(); r.clk.seedSystemClock();
    assert(r.clk.source() == C::ClockSource::HardwareRTC); }

  // ClockRestartCarry: the evidence that a restart kept the time. Accepted only for a
  // software restart of a current session, with the clock not behind the last stamp
  // and no more than MAX_BEHIND_SECS lost since the last real set.
  {
    using K = ClockRestartCarry;
    const uint32_t B = K::MAX_BEHIND_SECS;
    const uint32_t RTC = 5000;   // seconds on the RTC counter at the stamp
    // The ordinary restart: stamped current, 6 s pass, the clock came back 2 s on
    { K k{}; k.stamp(true, false, NOW, RTC);
      assert(k.consume(true, NOW + 2, RTC + 6) && k.behind_secs == 5);
      // ...and it is spent: the same evidence never serves a second boot
      assert(!k.consume(true, NOW + 2, RTC + 6)); }
    // Power-on: RTC memory holds whatever it holds, the magic is not there
    { K k{}; assert(!k.consume(true, NOW, RTC)); }
    { K k{0xDEADBEEFu, NOW, RTC, 0}; assert(!k.consume(true, NOW, RTC + 1)); }
    // Any reset but a software restart is refused, and disarms
    { K k{}; k.stamp(true, false, NOW, RTC);
      assert(!k.consume(false, NOW + 2, RTC + 6) && k.magic == 0); }
    // The session's time was not current: nothing to carry
    { K k{}; k.stamp(true, false, NOW, RTC); k.stamp(false, false, NOW + 1, RTC + 1);
      assert(!k.consume(true, NOW + 2, RTC + 6)); }
    // No RTC counter to measure with, at the stamp or at boot
    { K k{}; k.stamp(true, false, NOW, 0); assert(!k.consume(true, NOW + 2, RTC)); }
    { K k{}; k.stamp(true, false, NOW, RTC); assert(!k.consume(true, NOW + 2, 0)); }
    // The counter went back: it restarted with the power
    { K k{}; k.stamp(true, false, NOW, RTC); assert(!k.consume(true, NOW + 2, RTC - 1)); }
    // The clock came back behind the stamp: an older floor, not the ticked time
    { K k{}; k.stamp(true, false, NOW, RTC); assert(!k.consume(true, NOW - 1, RTC + 6)); }
    // A restart that sat for minutes before resetting cost the clock too much
    { K k{}; k.stamp(true, false, NOW, RTC); assert(!k.consume(true, NOW + 2, RTC + 300)); }
    // The limit itself: B seconds lost is kept, one more is not
    { K k{}; k.stamp(true, false, NOW, RTC); assert(k.consume(true, NOW, RTC + B - 1) && k.behind_secs == B); }
    { K k{}; k.stamp(true, false, NOW, RTC); assert(!k.consume(true, NOW, RTC + B)); }
    // A clock that moved further than the counter says has lost nothing
    { K k{}; k.stamp(true, false, NOW, RTC); assert(k.consume(true, NOW + 30, RTC + 6) && k.behind_secs == 0); }
    // The loss adds up over consecutive restarts with no real set in between...
    { K k{}; k.stamp(true, false, NOW, RTC);
      assert(k.consume(true, NOW, RTC + 24) && k.behind_secs == 25);
      k.stamp(true, true, NOW + 100, RTC + 124);                      // current only because carried
      assert(k.consume(true, NOW + 100, RTC + 148) && k.behind_secs == 50);
      k.stamp(true, true, NOW + 200, RTC + 248);
      assert(!k.consume(true, NOW + 200, RTC + 272));                 // 75 s behind: back to the floor
      // ...and a real set clears it
      k.stamp(true, false, NOW + 300, RTC + 372);
      assert(k.behind_secs == 0 && k.consume(true, NOW + 300, RTC + 396) && k.behind_secs == 25); }
  }

  printf("test_clock_floor: ok\n");
  return 0;
}
