// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Opt-in cold-boot time acquisition over saved Wi-Fi (issue #383).
//
// A board with no working battery-backed clock — a T-Deck (no chip at all), or
// a ThinkNode M9 whose PCF8563 came back with its VL bit latched after the
// power slider cut VBAT — boots knowing nothing but its persisted send-timestamp
// floor. That floor is correct for replay ordering and WRONG as wall time: it is
// frozen at the moment of the shutdown, which is exactly what users see.
//
// The request on #383 was "connect to any open Wi-Fi on power-on and ask a time
// server". This deliberately does NOT do that. An unknown open SSID is
// unauthenticated, trivially spoofed by an evil twin or a captive portal, and
// the NTP that follows is unauthenticated UDP — joining one silently broadcasts
// the device's presence and changes the user's connectivity, to set a clock.
// What this does instead:
//
//   * runs only after a TRUE power-on reset (a soft reboot keeps ESP32 RTC time,
//     so it must not cost the user a slow boot),
//   * only when the clock is not already current (hardware RTC or a sync this
//     boot both mean there is nothing to fetch),
//   * only when the user turned it on,
//   * first against the user's explicitly active secured credential, then
//     against saved networks marked Auto-Join — never an unknown one,
//   * and saved OPEN networks only behind a second, separate opt-in, and only
//     when the scan itself reports WIFI_AUTH_OPEN.
//
// It is strictly read-only with respect to Wi-Fi state: it calls WiFi.begin()
// directly rather than touchPrefsConnectWifiNet()/wifiConfigSetSsid()/
// wifiConfigSetRadioEnabled(), so the active credential, the saved-network
// ranking and the persisted Wi-Fi/BLE intent flags all come out unchanged. It is
// also hard-bounded (see kBootTimeSync*Ms) so every failure mode — missing AP,
// bad credentials, captive portal, no DHCP, no DNS, dead NTP — costs the same
// small, known amount of boot time and then gets out of the way.
//
// Call site: main.cpp's setup(), right after wifiConfigBegin() and BEFORE
// serial_interface.begin() / TCP+WS startup / a cold BLE allocation. Running
// there means there is no live transport to suspend and rebuild: the normal boot
// path reads the untouched persisted intent immediately afterwards.

#if defined(ESP32)

#include <stdint.h>

// Per-attempt and whole-operation budgets. Boot cost is capped at the total
// regardless of how many candidates are in range.
static constexpr uint32_t kBootTimeSyncAssocMs = 4000;    // one association attempt
static constexpr uint32_t kBootTimeSyncTotalMs = 12000;   // scan + every attempt + SNTP

enum class BootTimeSyncResult : uint8_t {
  Skipped,        // soft reboot, disabled, clock already current, or Wi-Fi already on
  NoCandidate,    // nothing saved that this device is allowed to try
  NoAssociation,  // candidates existed, none associated inside the budget
  NoTime,         // associated, but no sane epoch arrived before the deadline
  Ok,             // out_epoch holds a validated epoch
};

const char* bootTimeSyncResultName(BootTimeSyncResult r);

// Runs the whole bounded session and leaves Wi-Fi exactly as it found it.
// `clock_is_current` is the caller's answer to "do I already know the wall
// time?" (rtc_clock.timeIsCurrent()); passing true always yields Skipped.
// On Ok, the caller applies out_epoch via rtc_clock.setCurrentTime() so the
// clock floor, the system clock and any board RTC adapter all update through
// the one guarded path.
BootTimeSyncResult bootTimeSyncRun(bool clock_is_current, uint32_t& out_epoch);

// Bounded SNTP acquisition, factored out of main.cpp's open-ended loop block so
// it can simply be called. Assumes an association already exists. Starts
// pool.ntp.org / time.google.com, polls until a sane epoch or the budget runs
// out, and returns an explicit answer.
bool bootTimeSyncSntp(uint32_t budget_ms, uint32_t& out_epoch);

#endif  // ESP32
