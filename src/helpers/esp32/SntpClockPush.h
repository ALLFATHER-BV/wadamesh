// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// When the Wi-Fi loop starts SNTP and when it writes an SNTP result into the mesh clock.
// Pure (no lwIP, no Arduino) so test/test_sntp_clock_push.cpp drives it on the host.
//
// SNTP keeps running once started and re-syncs on its own (lwIP's default is hourly). Each
// sync sets ONLY the ESP32 system clock. The mesh clock (rtc_clock) used to get the first
// result and nothing after it, which matters on a board with an RTC chip (ThinkNode M9,
// T-LoRa Pager): the chip kept whatever it was last written, and HardwareRtcClock's hourly
// resync from the chip (#583) then pulled the system clock back to that stale value. So a
// clock that went wrong after the first sync (a stale GPS set, drift) stayed wrong, and an
// SNTP correction lasted at most an hour. Now every completed sync is written through, which
// also writes the chip and lifts the clock floor.
//
// rekick: the user pressed "Sync clock from system" with Wi-Fi up. Restarting SNTP asks the
// server now instead of copying a system clock that is the same clock on most boards.
#include <stdint.h>

struct SntpClockPush {
  bool kicked = false;          // SNTP started for this association (configTzTime ran)
  bool ever_pushed = false;     // at least one result reached the mesh clock this boot
  volatile bool rekick = false; // set from the UI: restart SNTP and report the result
  bool report_next = false;     // the next push answers a user request (show "Clock synced")

  // A pass of the Wi-Fi loop while associated, before anything else: true = run
  // configTzTime() (start or restart SNTP) now.
  bool wantKick() {
    if (rekick) { rekick = false; kicked = false; report_next = true; }
    if (kicked) return false;
    kicked = true;
    return true;
  }

  // After a kick, every pass: `completed` is esp_sntp_get_sync_status() == COMPLETED (that
  // call reads AND resets it, so it is true once per sync), `t` is time(nullptr). True = write
  // `t` into the mesh clock. Sets *report when that answers the user's request.
  bool shouldPush(bool completed, uint32_t t, uint32_t min_valid, uint32_t max_plausible, bool* report) {
    if (report) *report = false;
    if (!kicked || !completed) return false;
    if (t <= min_valid || t > max_plausible) return false;
    ever_pushed = true;
    if (report && report_next) *report = true;
    report_next = false;
    return true;
  }

  // The association dropped. Before any result arrived, start afresh on the next one; after
  // one, SNTP is running and keeps re-syncing across reconnects by itself.
  void linkDown() { if (kicked && !ever_pushed) kicked = false; }
};
