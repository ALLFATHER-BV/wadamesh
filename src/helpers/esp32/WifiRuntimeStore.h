#pragma once

/* NVS Wi-Fi credentials: namespace meshcomod; used by companion_radio and repeater_tcp. */
#if defined(ESP32)

#include <stddef.h>
#include <stdint.h>

#define WIFI_CONFIG_SSID_MAX 32
#define WIFI_CONFIG_PWD_MAX 64

void wifiConfigBegin();
bool wifiConfigHasRuntime();
void wifiConfigGetSsid(char *buf, size_t len);
void wifiConfigGetPwd(char *buf, size_t len);
bool wifiConfigSetSsid(const char *ssid);
bool wifiConfigSetPwd(const char *pwd);
void wifiConfigClear();
void wifiConfigApply();

bool wifiConfigGetRadioEnabled();
void wifiConfigSetRadioEnabled(bool enabled);

/* BLE radio on/off, persisted independently of the Wi-Fi radio (default ON) so
 * the two can be toggled separately and coexist. The companion BLE toggle writes
 * this; boot honors it. Fixes "BT turned off reverts to on after reboot". */
bool wifiConfigGetBleEnabled();
void wifiConfigSetBleEnabled(bool enabled);

#if defined(TLORA_PAGER)
/* Pager coexistence ownership. Wi-Fi intent alone cannot answer whether a
 * cold NimBLE start is safe: touch builds keep the STA scannable even with no
 * SSID. Cold STA allocation and active WPA association own the short ordering
 * boundary; an initialized idle STA can coexist with NimBLE. */
enum class PagerWifiBlePhase : uint8_t {
  Idle,
  Associating,
  Connected,
  BleFallback,
};
void wifiConfigSetPagerWifiBlePhase(PagerWifiBlePhase phase);
PagerWifiBlePhase wifiConfigGetPagerWifiBlePhase();
bool wifiConfigPagerWifiBlocksBle();
bool wifiConfigPagerBleFallbackActive();
#endif

/* "Wi-Fi chosen" sticky flag: set whenever the radio is explicitly enabled
 * (setRadioEnabled(true)). Historically gated whether the touch build brought
 * Wi-Fi up with no creds; now vestigial — touch defaults to Wi-Fi whenever the
 * radio is on (see wifiConfigWantsWifi). Kept for compatibility. */
bool wifiConfigGetWifiChosen();
void wifiConfigSetWifiChosen(bool chosen);

/* Wi-Fi intent used at boot and in the main loop. True = bring Wi-Fi up (STA).
 * Non-touch rule = radio enabled AND creds present. On touch builds, radio-on
 * is enough to keep the STA scannable without credentials. Pager BLE ordering
 * must use PagerWifiBlePhase rather than treating this intent as association. */
bool wifiConfigWantsWifi();

/* Request an immediate (re)apply of current Wi-Fi settings from the main loop.
 * Use after changing SSID/PWD or after Set* writes from another task/context —
 * avoids calling WiFi.disconnect/begin from an LVGL event handler. */
void wifiConfigRequestApply();
bool wifiConfigConsumeApplyRequest();

/* Set while a Wi-Fi scan runs on the worker task. The main loop's reconnect-retry
 * must NOT call WiFi.disconnect()/begin() during a scan — that aborts the in-flight
 * sweep (esp_wifi_scan_start gets cancelled), which is why a scan-while-connected
 * returned 0 APs. The worker also disables setAutoReconnect for the same reason. */
void wifiScanSetActive(bool active);
bool wifiScanIsActive();

#endif
