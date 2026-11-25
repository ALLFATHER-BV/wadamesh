// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// WifiRoam: which saved network the main loop's rejoin tries next (Discord: only the network
// joined last ever reconnected; a second saved network, home vs work, had to be picked by hand
// every time). Pure logic, no Arduino or IDF headers, so it is host-tested
// (test/test_wifi_roam.cpp) exactly as the boards run it.
//
// The loop's rejoin used to try the active network (the one joined last) forever. Now, while
// the active network is not on the air (the last attempt ended in "AP not found"), each rejoin
// tries the next saved network marked Auto-Join, then the active one again, round and round.
// A try that joins becomes the active network (the caller saves that). A network that is on
// the air but refuses us (wrong password, a full AP) is never left for another one: only "not
// found" moves on. The order is slot order; every turn of the round still starts and ends at
// the active network, so it is never more than one round away.
#include <stdint.h>
#include <string.h>

namespace WifiRoam {

static constexpr uint8_t REASON_NO_AP_FOUND = 201;   // WIFI_REASON_NO_AP_FOUND

struct Net {
  bool used;
  bool auto_join;
  const char* ssid;
};

// `cur` is the slot of the network the last attempt went to, -1 for the active network.
// Returns the slot to try now, or -1 for the active network.
inline int next(const Net* nets, int count, const char* active_ssid, int cur, uint8_t last_reason) {
  // Only "not found" moves on from the active network; a candidate that did not join (for
  // any reason) hands over to the next one.
  if (cur < 0 && last_reason != REASON_NO_AP_FOUND) return -1;
  for (int i = cur + 1; i < count; ++i) {
    const Net& n = nets[i];
    if (!n.used || !n.auto_join || !n.ssid || !n.ssid[0]) continue;
    if (active_ssid && strcmp(n.ssid, active_ssid) == 0) continue;   // that one is the active
    return i;
  }
  return -1;   // round done: the active network again
}

}  // namespace WifiRoam
