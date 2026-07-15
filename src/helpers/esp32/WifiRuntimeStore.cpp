#include "WifiRuntimeStore.h"

#if defined(ESP32)

#include "SdNvsPrefs.h"   // NVS, or SD /meshcomod fallback when NVS is unusable (Launcher)
#include <WiFi.h>
#include <cstring>
#if defined(HAS_TDISPLAY_P4)
  // T-Display P4: rebind WiFi.* to the c6_at AT-over-SDIO facade — Arduino's real WiFi.mode()/begin()
  // re-init esp_hosted (the C6 runs ESP-AT, not an esp-hosted slave) and panic the P4. P4-only header.
  #include <C6WifiShim.h>
#endif

static const char *WIFI_CONFIG_NAMESPACE = "meshcomod";
static const char *WIFI_CONFIG_SSID_KEY = "wifi_ssid";
static const char *WIFI_CONFIG_PWD_KEY = "wifi_pwd";
static const char *WIFI_CONFIG_RADIO_EN_KEY = "wifi_radio_en";
static const char *WIFI_CONFIG_WIFI_CHOSEN_KEY = "wifi_chosen";
static const char *WIFI_CONFIG_BLE_EN_KEY = "ble_en";   // BLE radio on/off (default on)

static SdNvsPrefs s_prefs;
static bool s_begun = false;
static volatile bool s_wifi_apply_requested = false;

void wifiConfigBegin() {
  if (s_begun) return;
  s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);
  if (!s_begun) {
    if (s_prefs.begin(WIFI_CONFIG_NAMESPACE, false)) {
      s_prefs.end();
      s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);
    }
  }
}

bool wifiConfigHasRuntime() {
  if (!s_begun) wifiConfigBegin();
  if (!s_prefs.isKey(WIFI_CONFIG_SSID_KEY)) return false;
  char ssid[WIFI_CONFIG_SSID_MAX];
  s_prefs.getString(WIFI_CONFIG_SSID_KEY, ssid, sizeof(ssid));
  return ssid[0] != '\0';
}

void wifiConfigGetSsid(char *buf, size_t len) {
  if (!buf || len == 0) return;
  buf[0] = '\0';
  if (!s_begun) wifiConfigBegin();
  if (s_prefs.isKey(WIFI_CONFIG_SSID_KEY))
    s_prefs.getString(WIFI_CONFIG_SSID_KEY, buf, len);
  buf[len - 1] = '\0';
}

void wifiConfigGetPwd(char *buf, size_t len) {
  if (!buf || len == 0) return;
  buf[0] = '\0';
  if (!s_begun) wifiConfigBegin();
  if (s_prefs.isKey(WIFI_CONFIG_PWD_KEY))
    s_prefs.getString(WIFI_CONFIG_PWD_KEY, buf, len);
  buf[len - 1] = '\0';
}

bool wifiConfigSetSsid(const char *ssid) {
  if (!ssid) return false;
  size_t n = strlen(ssid);
  if (n >= WIFI_CONFIG_SSID_MAX) return false;
  if (!s_begun) {
    wifiConfigBegin();
  }
  s_prefs.end();
  if (!s_prefs.begin(WIFI_CONFIG_NAMESPACE, false)) return false;
  bool ok = s_prefs.putString(WIFI_CONFIG_SSID_KEY, ssid);
  s_prefs.end();
  s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);
  return ok;
}

bool wifiConfigSetPwd(const char *pwd) {
  if (!pwd) pwd = "";
  size_t n = strlen(pwd);
  if (n >= WIFI_CONFIG_PWD_MAX) return false;
  if (!s_begun) {
    wifiConfigBegin();
  }
  s_prefs.end();
  if (!s_prefs.begin(WIFI_CONFIG_NAMESPACE, false)) return false;
  bool ok = s_prefs.putString(WIFI_CONFIG_PWD_KEY, pwd);
  s_prefs.end();
  s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);
  return ok;
}

void wifiConfigClear() {
  if (!s_begun) wifiConfigBegin();
  s_prefs.end();
  s_prefs.begin(WIFI_CONFIG_NAMESPACE, false);
  s_prefs.remove(WIFI_CONFIG_SSID_KEY);
  s_prefs.remove(WIFI_CONFIG_PWD_KEY);
  s_prefs.end();
  s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);
}

bool wifiConfigGetRadioEnabled() {
  if (!s_begun) wifiConfigBegin();
  return s_prefs.getUChar(WIFI_CONFIG_RADIO_EN_KEY, 1) != 0;
}

void wifiConfigSetRadioEnabled(bool enabled) {
  if (!s_begun) wifiConfigBegin();
  s_prefs.end();
  if (!s_prefs.begin(WIFI_CONFIG_NAMESPACE, false)) return;
  s_prefs.putUChar(WIFI_CONFIG_RADIO_EN_KEY, enabled ? 1 : 0);
  // Enabling the radio records the (now-vestigial) "Wi-Fi chosen" flag; on touch
  // wifiConfigWantsWifi() already treats radio-on as Wi-Fi, so this only matters
  // for back-compat. Non-touch builds ignore the flag.
  if (enabled) s_prefs.putUChar(WIFI_CONFIG_WIFI_CHOSEN_KEY, 1);
  s_prefs.end();
  s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);

  /* Defer the actual WiFi.mode / disconnect / begin to the main loop so we
   * never touch the WiFi state machine from within an LVGL event handler.
   * Calling WiFi.disconnect()+begin() from the LV event ctx was racing with
   * the main wifi loop and could leave the radio in WIFI_OFF mode. */
  s_wifi_apply_requested = true;
}

/* BLE radio on/off, persisted independently of the Wi-Fi radio so the two can be
 * toggled independently (and coexist). Default ON. Uses getUChar (quiet log_v),
 * not getString, so no NOT_FOUND console spam for a fresh device. */
bool wifiConfigGetBleEnabled() {
  if (!s_begun) wifiConfigBegin();
  return s_prefs.getUChar(WIFI_CONFIG_BLE_EN_KEY, 1) != 0;
}

void wifiConfigSetBleEnabled(bool enabled) {
  if (!s_begun) wifiConfigBegin();
  s_prefs.end();
  if (!s_prefs.begin(WIFI_CONFIG_NAMESPACE, false)) return;
  s_prefs.putUChar(WIFI_CONFIG_BLE_EN_KEY, enabled ? 1 : 0);
  s_prefs.end();
  s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);
}

bool wifiConfigGetWifiChosen() {
  if (!s_begun) wifiConfigBegin();
  return s_prefs.getUChar(WIFI_CONFIG_WIFI_CHOSEN_KEY, 0) != 0;
}

void wifiConfigSetWifiChosen(bool chosen) {
  if (!s_begun) wifiConfigBegin();
  s_prefs.end();
  if (!s_prefs.begin(WIFI_CONFIG_NAMESPACE, false)) return;
  s_prefs.putUChar(WIFI_CONFIG_WIFI_CHOSEN_KEY, chosen ? 1 : 0);
  s_prefs.end();
  s_begun = s_prefs.begin(WIFI_CONFIG_NAMESPACE, true);
}

bool wifiConfigWantsWifi() {
  // Radio off -> BLE (or no transport). Radio on + creds -> Wi-Fi (classic).
  if (!wifiConfigGetRadioEnabled()) return false;
  if (wifiConfigHasRuntime()) return true;
#ifdef HAS_TOUCH_UI
  // Touch default: Wi-Fi is the primary transport. The radio-enabled pref is
  // on out of the box, so a freshly-flashed device comes up on Wi-Fi (STA,
  // scannable) with BLE off — letting the setup wizard scan + pick a network
  // on-device with no creds typed yet. Switching to BLE is an explicit choice
  // (the Bluetooth toggle clears radio_en). The old "wifi_chosen" gate that
  // kept fresh devices on BLE is retired.
  return true;
#else
  return false;
#endif
}

void wifiConfigRequestApply() {
  s_wifi_apply_requested = true;
}

bool wifiConfigConsumeApplyRequest() {
  if (!s_wifi_apply_requested) return false;
  s_wifi_apply_requested = false;
  return true;
}

static volatile bool s_wifi_scan_active = false;
void wifiScanSetActive(bool active) { s_wifi_scan_active = active; }
bool wifiScanIsActive() { return s_wifi_scan_active; }

void wifiConfigApply() {
#if defined(HAS_TANMATSU)
  printf("[WIFI] apply radio_en=%d hasRuntime=%d mode=%d status=%d\n",
         (int)wifiConfigGetRadioEnabled(), (int)wifiConfigHasRuntime(),
         (int)WiFi.getMode(), (int)WiFi.status());
#endif
  if (!wifiConfigGetRadioEnabled()) {
    WiFi.disconnect(true);
    delay(50);
    WiFi.mode(WIFI_OFF);
    return;
  }
  if (!wifiConfigHasRuntime()) return;   // no cred -> leave the STA alone (eraseap wedges this radio)
  char ssid[WIFI_CONFIG_SSID_MAX];
  char pwd[WIFI_CONFIG_PWD_MAX];
  wifiConfigGetSsid(ssid, sizeof(ssid));
  wifiConfigGetPwd(pwd, sizeof(pwd));
  WiFi.mode(WIFI_STA);            // esp_wifi_remote needs an explicit STA mode before begin()
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, pwd[0] ? pwd : nullptr);
#if defined(HAS_TANMATSU)
  printf("[WIFI] begin ssid='%s' pwlen=%d\n", ssid, (int)strlen(pwd));
#endif
}

#endif
