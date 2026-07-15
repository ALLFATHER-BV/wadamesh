// SPDX-License-Identifier: GPL-3.0-or-later
// C6WifiShim — P4-only drop-in facade for the Arduino `WiFi` object, backed by the c6_at driver.
//
// WHY: on the T-Display P4 the C6 runs ESP-AT (not an esp-hosted slave). Arduino's real WiFi class
// calls esp_hosted_init() at WiFi.mode()/begin() time, which grabs the C6's SDIO bus out from under
// the c6_at driver and panics the P4 (the "connect Wi-Fi -> blue screen" crash). This facade keeps
// the exact WiFi.* call surface the app already uses, but routes it to the c6_at worker: scans and
// joins become REAL (over AT), status/IP reads come from the worker's lock-free cache, and nothing
// can ever touch esp_hosted.
//
// USAGE (per translation unit, AFTER all other includes):   #include <C6WifiShim.h>
// The trailing `#define WiFi WadaC6WiFi` rebinds every later `WiFi.` in the TU to the facade.
// Header-only; the single instance is a C++17 inline variable. Only the methods the app actually
// calls are implemented — adding a new WiFi.* call elsewhere will fail to compile HERE (good: it
// forces a conscious decision instead of silently hitting esp_hosted).
#pragma once
#include <WiFi.h>      // real header first (types: String, IPAddress, wl_status_t, wifi_mode_t)
#include "c6_at.h"

class C6WiFiFacade {
public:
  // ---- station control ----
  wl_status_t begin(const char *ssid, const char *passphrase = nullptr) {
    c6at_worker_start();
    c6at_set_sta_enabled(true);
    c6at_req_join(ssid, passphrase);
    return status();
  }
  bool disconnect(bool wifioff = false, bool eraseap = false) {
    (void)eraseap;
    c6at_req_disconnect();
    if (wifioff) c6at_set_sta_enabled(false);
    return true;
  }
  bool mode(wifi_mode_t m) {
    if (m == WIFI_MODE_NULL) { c6at_req_disconnect(); c6at_set_sta_enabled(false); }
    else                     { c6at_worker_start();   c6at_set_sta_enabled(true);  }
    return true;
  }
  wifi_mode_t getMode() { return c6at_sta_enabled() ? WIFI_MODE_STA : WIFI_MODE_NULL; }

  // ---- status (lock-free cache reads; safe on the UI thread) ----
  wl_status_t status() { return c6at_connected() ? WL_CONNECTED : WL_DISCONNECTED; }
  IPAddress localIP() {
    uint32_t v = c6at_ip4();
    return IPAddress((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  }
  String SSID() {
    char s[33];
    return c6at_current_ssid(s, sizeof(s)) ? String(s) : String();
  }
  int8_t RSSI() { return (int8_t)c6at_current_rssi(); }
  int32_t channel() { return (int32_t)c6at_current_channel(); }   // current AP channel (0 = unknown)

  // ---- scanning (async via the worker; Arduino return-code semantics) ----
  int16_t scanNetworks(bool async = false, bool show_hidden = false, bool passive = false,
                       uint32_t max_ms_per_chan = 300, uint8_t channel = 0,
                       const char *ssid = nullptr, const uint8_t *bssid = nullptr) {
    (void)async; (void)show_hidden; (void)passive; (void)max_ms_per_chan; (void)channel; (void)ssid; (void)bssid;
    c6at_worker_start();
    c6at_set_sta_enabled(true);
    c6at_req_scan();
    return WIFI_SCAN_RUNNING;
  }
  int16_t scanComplete() { return (int16_t)c6at_scan_state(); }
  void    scanDelete()   { c6at_scan_clear(); }
  String SSID(uint8_t i) {
    char s[33];
    return c6at_scan_get(i, s, sizeof(s), nullptr, nullptr) ? String(s) : String();
  }
  int32_t channel(uint8_t i) { return (int32_t)c6at_scan_channel(i); }   // scan-result channel

  // ---- accepted no-ops (ESP-AT handles reconnect + power policy on the C6 itself) ----
  bool setAutoReconnect(bool en) { (void)en; return true; }
  bool setSleep(bool en)         { (void)en; return true; }
};

inline C6WiFiFacade WadaC6WiFi;

// Rebind all later `WiFi.` uses in this TU to the facade. (String literals are unaffected; any
// re-include of <WiFi.h> after this point is inert thanks to its include guard.)
#define WiFi WadaC6WiFi
