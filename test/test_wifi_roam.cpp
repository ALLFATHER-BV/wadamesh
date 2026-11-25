// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for WifiRoam (src/helpers/WifiRoam.h): which saved network the main loop's
// rejoin tries next while the active network is not on the air.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_wifi_roam.cpp -o /tmp/test_wifi_roam && /tmp/test_wifi_roam
#include <assert.h>
#include <stdio.h>

#include "helpers/WifiRoam.h"

using WifiRoam::Net;
using WifiRoam::next;
static constexpr uint8_t NOT_FOUND = WifiRoam::REASON_NO_AP_FOUND;
static constexpr uint8_t AUTH_FAIL = 15;   // 4-way handshake timeout: on the air, refused us

int main() {
  // slot 0 home (active), 1 work, 2 empty, 3 a café without Auto-Join, 4 hotspot
  const Net nets[] = {
    { true,  true,  "home" },
    { true,  true,  "work" },
    { false, false, "" },
    { true,  false, "cafe" },
    { true,  true,  "hotspot" },
  };
  const int n = (int)(sizeof nets / sizeof nets[0]);

  // Home not found: work, then the hotspot, then home again, round and round.
  int cur = next(nets, n, "home", -1, NOT_FOUND);
  assert(cur == 1);
  cur = next(nets, n, "home", cur, NOT_FOUND);
  assert(cur == 4);
  cur = next(nets, n, "home", cur, NOT_FOUND);
  assert(cur == -1);
  cur = next(nets, n, "home", cur, NOT_FOUND);
  assert(cur == 1);

  // Home on the air but refusing us: stay with it.
  assert(next(nets, n, "home", -1, AUTH_FAIL) == -1);
  assert(next(nets, n, "home", -1, 0) == -1);

  // A candidate that did not join, for whatever reason, hands over to the next one.
  assert(next(nets, n, "home", 1, AUTH_FAIL) == 4);

  // The active network is skipped among the candidates (work is active now).
  assert(next(nets, n, "work", -1, NOT_FOUND) == 0);
  assert(next(nets, n, "work", 0, NOT_FOUND) == 4);

  // One saved network only: always the active one.
  const Net one[] = { { true, true, "home" } };
  assert(next(one, 1, "home", -1, NOT_FOUND) == -1);

  // Nothing saved at all.
  assert(next(nets, 0, "home", -1, NOT_FOUND) == -1);

  printf("test_wifi_roam: ok\n");
  return 0;
}
