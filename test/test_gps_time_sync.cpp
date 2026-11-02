// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the GPS clock set across a GPS saver rest (Discord, ThinkNode M9 on beta_100:
// "the clock is often behind by a half hour"). The real src/helpers/WadaNmeaLocationProvider.h
// runs against a simulated receiver that sends one RMC a second with the TRUE UTC time while it
// is awake (test/gps_time_fakes), and a clock that records every set.
//
// The bug: begin() kept the parser's pre-rest sentence, so the first loop pass after a wake
// saw a "valid" fix with the 30-minute re-sync due and set the clock to the moment the receiver
// went to rest, before the receiver had said anything new (the pre-fix provider, run through
// this test, set the clock 1801 s behind after a 30-minute rest). The rule checked here: every clock
// set the provider makes is the true time (within the 1 s an RMC is old at most), whatever the
// rest length, both through begin() alone (the provider's own fix) and with the UI's
// syncTime() on wake (UITask::setGpsPower, the fix that also covers the core provider).
//
//   c++ -std=c++17 -Wall -Wextra -DESP32 -I src -I test/gps_time_fakes -I test/gps_probe_fakes \
//       test/test_gps_time_sync.cpp -o /tmp/test_gps_time_sync && /tmp/test_gps_time_sync
#include <assert.h>
#include <stdio.h>

#include "helpers/WadaNmeaLocationProvider.h"

uint32_t g_test_ms = 0;
uint32_t g_utc_at_ms0 = 0;

// The provider's prefs calls (only the UART-probe path uses them; this test has no probe).
uint32_t touchPrefsGetGpsBaud(uint32_t fallback) { return fallback; }
bool touchPrefsGpsBaudChosen() { return false; }
void touchPrefsSetGpsBaudFound(uint32_t) {}
bool touchPrefsSetGpsBaud(uint32_t) { return true; }

struct RecordingClock : mesh::RTCClock {
  int sets = 0, stale_sets = 0;
  long worst_lag = 0;   // seconds the most-behind set was behind the true time
  void setCurrentTime(uint32_t t) override {
    ++sets;
    const long lag = (long)trueUtc() - (long)t;
    if (lag > worst_lag) worst_lag = lag;
    if (lag > 1 || lag < -1) ++stale_sets;
  }
};

// The sensor manager calls loop() only while the GPS is on (EnvironmentSensorManager::loop,
// gps_active); the UI loop passes every 10 ms here.
static void run(WadaNmeaLocationProvider& gps, uint32_t ms) {
  const uint32_t end = g_test_ms + ms;
  while ((int32_t)(g_test_ms - end) < 0) { gps.loop(); g_test_ms += 10; }
}

static void scenario(bool ui_sync_on_wake) {
  g_test_ms = 100000;
  g_utc_at_ms0 = 1793017200u - 100;   // 2026-10-26 12:20Z at the start of the run
  HardwareSerial uart;
  RecordingClock clk;
  WadaNmeaLocationProvider gps(uart, &clk, -1, -1);

  // Boot: the receiver needs a moment, then reports every second.
  uart.wake(1500);
  gps.begin();
  run(gps, 60000);
  assert(clk.sets == 1 && clk.stale_sets == 0);   // the boot sync, at the true time

  // The GPS saver: settled fix -> rest for the chosen interval -> wake, again and again.
  // Every interval the UI offers (2, 5, 15, 30 min), and a long one.
  const uint32_t rests_min[] = { 30, 2, 5, 15, 30, 31, 45, 30 };
  for (uint32_t rest : rests_min) {
    uart.sleep();
    gps.stop();                       // stop_gps(): no more loop() calls while it rests
    g_test_ms += rest * 60000u;
    uart.wake(4000);                  // a waking receiver is silent for a few seconds
    gps.begin();                      // start_gps()
    gps.reset();
    if (ui_sync_on_wake) gps.syncTime();   // UITask::setGpsPower(true)
    // The saver keeps it awake until the fix has held 30 s (s_gps_fix_since + 30000).
    run(gps, 40000);
    assert(clk.stale_sets == 0);
  }
  // The re-syncs did happen (the 30-minute one is always due after a rest that long), just
  // from fresh sentences.
  assert(clk.sets >= 4);
  printf("  %s: %d clock sets, all within 1 s of true UTC (worst lag %ld s)\n",
         ui_sync_on_wake ? "begin()+syncTime()" : "begin() only     ", clk.sets, clk.worst_lag);
}

int main() {
  scenario(false);   // the provider's own begin() fix
  scenario(true);     // plus the UI's syncTime() on wake (what the core provider relies on)
  printf("test_gps_time_sync: all passed\n");
  return 0;
}
