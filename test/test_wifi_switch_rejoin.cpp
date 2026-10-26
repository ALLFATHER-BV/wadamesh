// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the Wi-Fi switches' rejoin (79b605fe): src/ui-touch/WifiSwitchRejoin.h,
// the decision wifiSwitchRejoinService runs every UI loop pass after wifiRadioSwitch(true).
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_wifi_switch_rejoin.cpp \
//       -o /tmp/test_wifi_switch_rejoin && /tmp/test_wifi_switch_rejoin
#include <assert.h>
#include <stdio.h>
#include <functional>
#include <vector>

#include "ui-touch/WifiSwitchRejoin.h"

using WifiSwitchRejoin::State;

// Run the service every 20 ms (a UI loop pass) from `t0` (the switch: since = t0) until
// `t_end`, with the world given by `at(now)`. Returns the times the rejoin fired.
static std::vector<uint32_t> run(uint32_t t0, uint32_t t_end, const std::function<State(uint32_t)>& at,
                                 uint32_t* since_out = nullptr) {
  std::vector<uint32_t> fired;
  uint32_t since = t0;   // wifiRadioSwitch(true): s_wifi_switch_rejoin_ms = millis()
  for (uint32_t now = t0; now <= t_end; now += 20)
    if (WifiSwitchRejoin::step(since, now, at(now))) fired.push_back(now);
  if (since_out) *since_out = since;
  return fired;
}
static State up(bool connected = false) { return State{true, true, connected, true, false}; }

int main() {
  const uint32_t T = 50000;

  // The T-Deck case: the driver comes up 300 ms after the switch, the first join does
  // not take. One rejoin, 4 s after the driver is up; never a second one.
  {
    auto f = run(T, T + 60000, [&](uint32_t now) {
      State s = up();
      s.driver_up = now >= T + 300;
      return s;
    });
    assert(f.size() == 1);
    assert(f[0] >= T + 300 - 20 + 4000 && f[0] <= T + 300 + 4000);   // counted from the last pass that saw it down
  }

  // A join that works first time (linked at 2.5 s): left alone, the wait ends.
  {
    uint32_t since = 1;
    auto f = run(T, T + 60000, [&](uint32_t now) { return up(now >= T + 2500); }, &since);
    assert(f.empty() && since == 0);
  }

  // Linked just before the 4 s: no rejoin.
  {
    auto f = run(T, T + 60000, [&](uint32_t now) { return up(now >= T + 3980); });
    assert(f.empty());
  }

  // A scan owns the radio from 1 s to 9 s (Settings > Wi-Fi opened): nothing while it
  // runs, and the 4 s count starts again when it ends (the scan's own apply may join).
  {
    auto f = run(T, T + 60000, [&](uint32_t now) {
      State s = up();
      s.scanning = now >= T + 1000 && now < T + 9000;
      return s;
    });
    assert(f.size() == 1);
    assert(f[0] >= T + 9000 + 4000 - 20 && f[0] < T + 9000 + 4000 + 40);
  }
  // ...and when the scan's apply joins, nothing fires at all
  {
    auto f = run(T, T + 60000, [&](uint32_t now) {
      State s = up(now >= T + 9500);
      s.scanning = now >= T + 1000 && now < T + 9000;
      return s;
    });
    assert(f.empty());
  }

  // Switched off again within the 4 s: cancelled, nothing fires even if switched on
  // later by something else (a new switch starts its own wait).
  {
    uint32_t since = 1;
    auto f = run(T, T + 60000, [&](uint32_t now) { State s = up(); s.wants = now < T + 2000; return s; }, &since);
    assert(f.empty() && since == 0);
  }

  // No saved network to join: nothing to do.
  {
    auto f = run(T, T + 60000, [&](uint32_t) { State s = up(); s.has_runtime = false; return s; });
    assert(f.empty());
  }

  // The driver never comes up (main.cpp failed to start it): the wait keeps restarting,
  // no rejoin is asked of a driver that is not there.
  {
    uint32_t since = 0;
    auto f = run(T, T + 60000, [&](uint32_t) { State s = up(); s.driver_up = false; return s; }, &since);
    assert(f.empty() && since != 0);
  }

  // Nothing pending (switched off, or a switch that never turned it on): nothing happens.
  {
    uint32_t since = 0;
    for (uint32_t now = T; now < T + 20000; now += 20) assert(!WifiSwitchRejoin::step(since, now, up()));
    assert(since == 0);
  }

  // millis() wrapping during the wait: still 4 s.
  {
    const uint32_t W = 0xFFFFFFFFu - 1000;
    uint32_t since = W;
    uint32_t fired_at = 0; bool fired = false;
    for (uint32_t i = 0; i < 400 && !fired; ++i) {
      const uint32_t now = W + i * 20;
      if (WifiSwitchRejoin::step(since, now, up())) { fired = true; fired_at = now - W; }
    }
    assert(fired && fired_at == 4000);
  }

  printf("test_wifi_switch_rejoin: ok\n");
  return 0;
}
