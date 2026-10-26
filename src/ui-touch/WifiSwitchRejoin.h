// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// The Wi-Fi switches' one rejoin (79b605fe; wifiRadioSwitch / wifiSwitchRejoinService in
// UITask.cpp), pure so it is host-tested (test/test_wifi_switch_rejoin.cpp).
//
// Turning the radio on from cold starts the driver and joins at once in main.cpp, and
// that first join does not always take on the S3 boards. So after a switch turns Wi-Fi
// on, once STA has been up for WAIT_MS without a link and no scan owns the radio, ask
// for one apply on the running driver (disconnect without erase, then begin), the same
// one a scan ends with. A join that works first time is left alone.
namespace WifiSwitchRejoin {

static constexpr uint32_t WAIT_MS = 4000;

struct State {
  bool wants;         // wifiConfigWantsWifi(): still switched on
  bool has_runtime;   // wifiConfigHasRuntime(): a network to join
  bool connected;     // WiFi.status() == WL_CONNECTED
  bool driver_up;     // WiFi.getMode() != WIFI_MODE_NULL
  bool scanning;      // wifiScanIsActive(): a scan owns the radio
};

// One service pass. `since_ms` is when the wait started (0: nothing pending); the
// switch sets it to millis() when it turns Wi-Fi on. Returns true exactly once, when
// the rejoin is due; the wait is then over.
inline bool step(uint32_t& since_ms, uint32_t now_ms, const State& s) {
  if (!since_ms) return false;
  if (!s.wants || !s.has_runtime || s.connected) {
    since_ms = 0;   // switched off again, nothing to join, or it joined
  } else if (!s.driver_up || s.scanning) {
    since_ms = now_ms;   // the driver is not up yet, or a scan owns it
  } else if ((uint32_t)(now_ms - since_ms) >= WAIT_MS) {
    since_ms = 0;
    return true;
  }
  return false;
}

}  // namespace WifiSwitchRejoin
