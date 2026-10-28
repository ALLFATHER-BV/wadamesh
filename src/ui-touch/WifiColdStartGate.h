// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// =============================================================================
// Whether a Wi-Fi switch may start the radio from cold (wifiEnableGate in
// UITask.cpp). Pure, so the host test (test/test_wifi_cold_start_gate.cpp) runs
// the same decision the board does.
//
// Bringing esp_wifi up the first time after boot allocates its buffers and tasks
// from internal RAM. With Bluetooth already resident on a tight board that
// allocation can fail deep in the Wi-Fi state machine, where nothing reports it,
// so the switch checks first and says why instead. The reserve is the same one
// the boot co-init and a cold Bluetooth start use (main.cpp, enableBle()).
//
//  * The driver is already up (a later off/on toggle): no new allocation, Ready.
//  * The T-LoRa Pager orders the start itself in the main loop and reports a
//    failed allocation there (ordered_by_main_loop): Ready.
//  * Otherwise: Ready only with the reserve free.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

namespace WifiColdStartGate {

static constexpr size_t kMinFree  = 50u * 1024u;   // free internal heap (8-bit capable)
static constexpr size_t kMinBlock = 20u * 1024u;   // largest free internal block

enum class Decision : uint8_t { Ready, LowMemory };

inline Decision decide(bool driver_up, bool ordered_by_main_loop,
                       size_t free_internal, size_t largest_block) {
  if (driver_up) return Decision::Ready;
  if (ordered_by_main_loop) return Decision::Ready;
  if (free_internal >= kMinFree && largest_block >= kMinBlock) return Decision::Ready;
  return Decision::LowMemory;
}

}  // namespace WifiColdStartGate
