// SPDX-License-Identifier: GPL-3.0-or-later
#include "BootTimeSync.h"

#if defined(ESP32)

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <string.h>
#include <time.h>

#include "TouchPrefsStore.h"
#include "WifiRuntimeStore.h"
#include "../ClockFloorRTC.h"   // the one set of sane epoch bounds

// Ticket #383 explicitly offers this fallback only to the T-Deck (no RTC) and
// the M9 (RTC integrity can be lost across its hard power cut). Keep the runtime
// gate as narrow as the UI capability gate so a migrated preference file cannot
// unexpectedly start Wi-Fi on another board.
#if defined(HAS_TDECK_GT911) || defined(HAS_THINKNODE_M9)
  #define BOOT_TIME_SYNC_SUPPORTED 1
#else
  #define BOOT_TIME_SYNC_SUPPORTED 0
#endif

const char* bootTimeSyncResultName(BootTimeSyncResult r) {
  switch (r) {
    case BootTimeSyncResult::Skipped:       return "skipped";
    case BootTimeSyncResult::NoCandidate:   return "no candidate";
    case BootTimeSyncResult::NoAssociation: return "no association";
    case BootTimeSyncResult::NoTime:        return "no time";
    case BootTimeSyncResult::Ok:            return "ok";
  }
  return "?";
}

#if !BOOT_TIME_SYNC_SUPPORTED

bool bootTimeSyncSntp(uint32_t, uint32_t&) { return false; }
BootTimeSyncResult bootTimeSyncRun(bool, uint32_t&) { return BootTimeSyncResult::Skipped; }

#else

bool bootTimeSyncSntp(uint32_t budget_ms, uint32_t& out_epoch) {
  // Same servers and the same user-selected TZ the running firmware uses, so the
  // display does not flip to UTC for the rest of boot. The TZ only affects
  // localtime(); the epoch handed back is UTC either way.
  char tz[48];
  touchPrefsBuildLocalTz(tz, sizeof tz);
  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime(tz, "pool.ntp.org", "time.google.com");

  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < budget_ms) {
    const uint32_t t = (uint32_t)time(nullptr);
    // Use SNTP's completion state rather than comparing against the prior
    // clock. A real correction is allowed to move backward when the persisted
    // floor was poisoned; the untouched ESP32 seed must still never pass.
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
        t > ClockFloorRTC::MIN_VALID_EPOCH && t <= ClockFloorRTC::MAX_PLAUSIBLE_EPOCH) {
      out_epoch = t;
      return true;
    }
    delay(100);
  }
  return false;
}

namespace {

struct Candidate {
  char    ssid[33];
  char    pwd[65];
  int32_t rssi;
  uint32_t rank;   // saved-network recency counter; breaks RSSI ties
};

// The explicitly active secured credential is the cheapest and most likely
// candidate, so #383 requires trying it before spending time on a scan. Open
// credentials still need scan metadata and the separate opt-in below.
bool getActiveCandidate(Candidate& out) {
  wifiConfigGetSsid(out.ssid, sizeof out.ssid);
  wifiConfigGetPwd(out.pwd, sizeof out.pwd);
  if (!out.ssid[0] || !out.pwd[0]) return false;
  out.rssi = INT32_MIN;
  out.rank = 0;
  return true;
}

// Try one candidate. Returns true once the association is up; never waits past
// either its own attempt budget or the caller's overall deadline.
bool associate(const Candidate& c, uint32_t deadline_ms) {
  WiFi.disconnect(false, true);   // clear a supplicant wedged by the previous attempt
  WiFi.begin(c.ssid, c.pwd[0] ? c.pwd : nullptr);

  const uint32_t attempt_end = millis() + kBootTimeSyncAssocMs;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) return true;
    const uint32_t now = millis();
    if ((int32_t)(now - attempt_end) >= 0) return false;
    if ((int32_t)(now - deadline_ms) >= 0) return false;
    delay(50);
  }
}

// Read-only candidate selection. Enumerates the saved-network store and the scan
// results; writes nothing back. In particular it never calls
// touchPrefsConnectWifiNet(), wifiConfigSetSsid()/SetRadioEnabled(), or any
// rank-bumping save API — a time-only attempt must leave the user's Wi-Fi
// configuration byte-identical.
int collectSavedCandidates(Candidate* out, int max_out, uint32_t deadline_ms,
                           const Candidate* already_tried) {
  const bool allow_open = touchPrefsGetBootWifiTimeOpen();
  int n = 0;

  // Nothing saved and marked Auto-Join means nothing is permitted, so do
  // not pay for a scan.
  bool any_auto_join = false;
  for (int i = 0; i < TOUCH_WIFI_NET_COUNT && !any_auto_join; ++i) {
    TouchWifiNet net;
    if (touchPrefsGetWifiNet(i, net) && net.used && net.auto_join && net.ssid[0]) any_auto_join = true;
  }
  if (!any_auto_join) return n;
  if ((int32_t)(millis() - deadline_ms) >= 0) return n;

  // One asynchronous scan, purely to decide WHICH saved networks are actually
  // in range and what authentication they advertise. Polling it against the
  // operation deadline keeps scanning inside the same global bound as
  // association and SNTP.
  int found = WiFi.scanNetworks(true, false);
  while (found == WIFI_SCAN_RUNNING && (int32_t)(millis() - deadline_ms) < 0) {
    delay(50);
    found = WiFi.scanComplete();
  }
  if (found <= 0) { WiFi.scanDelete(); return n; }

  for (int i = 0; i < TOUCH_WIFI_NET_COUNT && n < max_out; ++i) {
    TouchWifiNet net;
    if (!touchPrefsGetWifiNet(i, net) || !net.used || !net.auto_join || !net.ssid[0]) continue;

    const bool saved_open = !net.pwd[0];
    if (saved_open && !allow_open) continue;

    int best = -1;
    for (int s = 0; s < found; ++s) {
      if (WiFi.SSID(s) != net.ssid) continue;
      // Never downgrade a secured saved credential to an open evil twin with
      // the same SSID. Likewise, an open saved entry is eligible only when the
      // scan confirms that this AP is actually open.
      const bool ap_open = (WiFi.encryptionType(s) == WIFI_AUTH_OPEN);
      if (ap_open != saved_open) continue;
      if (best < 0 || WiFi.RSSI(s) > WiFi.RSSI(best)) best = s;
    }
    if (best < 0) continue;                       // saved, but not on the air here
    if (already_tried && strcmp(net.ssid, already_tried->ssid) == 0 &&
        strcmp(net.pwd, already_tried->pwd) == 0) continue;

    strlcpy(out[n].pwd, net.pwd, sizeof out[n].pwd);
    strlcpy(out[n].ssid, net.ssid, sizeof out[n].ssid);
    out[n].rssi = WiFi.RSSI(best);
    out[n].rank = net.rank;
    n++;
  }
  WiFi.scanDelete();

  // Strongest first, most-recently-used breaking ties. Insertion sort over at
  // most eight entries.
  for (int i = 1; i < n; ++i) {
    Candidate key = out[i];
    int j = i - 1;
    while (j >= 0 &&
           (out[j].rssi < key.rssi || (out[j].rssi == key.rssi && out[j].rank < key.rank))) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = key;
  }
  return n;
}

// Put the radio back exactly where the user left it: off, with nothing written
// to NVS (WiFi.persistent(false) is set before the first begin()) and no
// background reconnect armed.
void radioOff() {
  esp_sntp_stop();
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_OFF);
}

}  // namespace

BootTimeSyncResult bootTimeSyncRun(bool clock_is_current, uint32_t& out_epoch) {
  if (clock_is_current) return BootTimeSyncResult::Skipped;
  if (!touchPrefsGetBootWifiTime()) return BootTimeSyncResult::Skipped;

  // A soft reboot keeps ESP32 RTC time, so there is nothing to fetch and no
  // reason to make the user wait. Only a genuine power-on gets the session.
  if (esp_reset_reason() != ESP_RST_POWERON) return BootTimeSyncResult::Skipped;

  // Wi-Fi already on: the normal connection state machine and its SNTP path
  // handle this boot. Do not race them.
  if (wifiConfigWantsWifi()) return BootTimeSyncResult::Skipped;

  const uint32_t deadline_ms = millis() + kBootTimeSyncTotalMs;

  // Stage the STA WITHOUT touching the persisted Wi-Fi/BLE intent flags — no
  // wifiConfigSetRadioEnabled(), no user-facing enable/disable methods.
  WiFi.persistent(false);         // nothing this session does reaches NVS
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // no background retries left behind

  // ~1 KB of transient setup() stack (8 saved networks + the active credential).
  // Deliberately not static: this runs once, before the transports exist, and a
  // permanent buffer would cost the 2 MB boards RAM for the rest of the session.
  Candidate active{};
  const bool have_active = getActiveCandidate(active);
  bool had_candidate = have_active;
  bool associated = false;
  if (have_active) {
    Serial.printf("[boot-time] trying active network '%s'\n", active.ssid);
    associated = associate(active, deadline_ms);
  }

  if (!associated && (int32_t)(millis() - deadline_ms) < 0) {
    Candidate cands[TOUCH_WIFI_NET_COUNT];
    const int count = collectSavedCandidates(
        cands, (int)(sizeof cands / sizeof cands[0]), deadline_ms,
        have_active ? &active : nullptr);
    had_candidate = had_candidate || count > 0;
    for (int i = 0; i < count; ++i) {
      if ((int32_t)(millis() - deadline_ms) >= 0) break;
      Serial.printf("[boot-time] trying saved network '%s'\n", cands[i].ssid);
      if (associate(cands[i], deadline_ms)) { associated = true; break; }
    }
  }
  if (!associated) {
    radioOff();
    return had_candidate ? BootTimeSyncResult::NoAssociation
                         : BootTimeSyncResult::NoCandidate;
  }

  const uint32_t now = millis();
  const uint32_t remaining = ((int32_t)(now - deadline_ms) >= 0) ? 0 : (deadline_ms - now);
  uint32_t epoch = 0;
  const bool got = remaining > 0 && bootTimeSyncSntp(remaining, epoch);

  radioOff();   // success or failure, the radio goes back off before transports start
  if (!got) return BootTimeSyncResult::NoTime;
  out_epoch = epoch;
  return BootTimeSyncResult::Ok;
}

#endif  // BOOT_TIME_SYNC_SUPPORTED
#endif  // ESP32
