// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the Wi-Fi switches' cold-start check (#763):
// src/ui-touch/WifiColdStartGate.h, the decision wifiEnableGate() makes before a
// Wi-Fi switch starts the radio for the first time after boot.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_wifi_cold_start_gate.cpp \
//       -o /tmp/test_wifi_cold_start_gate && /tmp/test_wifi_cold_start_gate
#include <assert.h>
#include <stdio.h>

#include "ui-touch/WifiColdStartGate.h"

using namespace WifiColdStartGate;

static bool ready(bool up, bool pager, size_t f, size_t b) {
  return decide(up, pager, f, b) == Decision::Ready;
}

int main() {
  // The reserve is the one the boot co-init and a cold Bluetooth start use.
  static_assert(kMinFree == 51200 && kMinBlock == 20480, "same reserve as enableBle()");

  // ---- #763: oumike's T-Deck Pro V1.0, Bluetooth on, Wi-Fi off, after a reboot ----
  // "[wifi] cold start refused: free=50676 maxblk=40948": 524 bytes short of the free
  // total, with a largest block twice what it needs.
  const size_t kProFree = 50676, kProBlock = 40948;
  assert(!ready(false, false, kProFree, kProBlock));
  assert(kMinFree - kProFree == 524);

  // The fix moves the e-paper boards' LVGL draw band (240 x 24 px, RGB565) out of
  // internal RAM into PSRAM; it was allocated once at boot and held for good.
  const size_t kBand = 240u * 24u * 2u;
  assert(kBand == 11520);
  assert(ready(false, false, kProFree + kBand, kProBlock));
  // With room to spare, not by a hair: over 10 KB above the reserve, so the small
  // swings in heap use the issue describes no longer decide it.
  assert(kProFree + kBand - kMinFree > 10u * 1024u);
  // And the band coming back to the free total does not depend on it landing in the
  // largest block: the block was never the short side.
  assert(kProBlock >= kMinBlock);

  // ---- the rules themselves ----
  // Driver already up (a later off/on toggle): no new allocation, whatever the heap.
  assert(ready(true, false, 0, 0));
  assert(ready(true, false, kProFree, kProBlock));
  // The Pager's main loop orders the start and reports a failure itself.
  assert(ready(false, true, 0, 0));
  // Exact thresholds pass, one byte under either refuses.
  assert(ready(false, false, kMinFree, kMinBlock));
  assert(!ready(false, false, kMinFree - 1, kMinBlock));
  assert(!ready(false, false, kMinFree, kMinBlock - 1));
  // A fragmented heap: plenty free in total, no block big enough.
  assert(!ready(false, false, 120u * 1024u, 16u * 1024u));
  // #762's T-Deck Pro boot line, with Bluetooth on: "boot int: free=56308 largest=47092".
  assert(ready(false, false, 56308, 47092));

  printf("test_wifi_cold_start_gate: all passed\n");
  return 0;
}
