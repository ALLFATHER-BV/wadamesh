// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

// Keeps "the time is current" across a restart on a board with no working clock chip.
//
// ClockFloorRTC only calls the time current after a hardware clock or a sync in this
// boot (#383). A board with neither therefore came back from every restart showing
// "Clock not set", though the clock it carried across was a few seconds behind at
// most: a restart is not a power cut, and the core restores the last ticked time.
//
// This is the evidence that makes that carried time trustworthy. It lives in RTC
// no-init memory, so it survives a restart and is lost with the power, and the board
// glue stamps it once a second while the time is current. At boot it is accepted only
// when ALL of these hold:
//   - the previous session's time was current (the magic is set);
//   - the reset was a software restart. A power-on, a crash, a watchdog, a brown-out
//     and a deep-sleep wake all fall back to the persisted floor as before;
//   - the restored clock is not behind the last stamp, so what came back is the
//     ticked time itself and not an older persisted floor;
//   - the clock has lost no more than MAX_BEHIND_SECS since the last real set. The
//     loss is measured with the RTC counter, which a restart does not reset, and it
//     adds up over consecutive restarts, so a chain of them cannot drift unnoticed.
// Anything else leaves the clock on the floor, exactly as it was before this existed.
struct ClockRestartCarry {
  static constexpr uint32_t MAGIC = 0x434C4B31u;      // "CLK1"
  static constexpr uint32_t MAX_BEHIND_SECS = 60;

  uint32_t magic;       // MAGIC while the stamped session's time is current
  uint32_t epoch;       // the clock's reading at the last stamp
  uint32_t rtc_secs;    // the RTC counter at the last stamp, in seconds since power-up
  uint32_t behind_secs; // what the carried clock has lost since the last real set

  // Once a second from the main loop. `carried` says the time is current only because
  // a restart carried it (ClockSource::Restart), so the loss so far still stands; any
  // real source (a sync, a healthy chip) clears it.
  void stamp(bool time_is_current, bool carried, uint32_t clock_now, uint32_t rtc_now_secs) {
    if (!time_is_current || rtc_now_secs == 0) { magic = 0; return; }
    epoch = clock_now;
    rtc_secs = rtc_now_secs;
    if (!carried) behind_secs = 0;
    magic = MAGIC;
  }

  // Once at boot, after the clock has been restored and before it is classified.
  // Always disarms: a boot that restarts again before its first stamp starts from
  // the floor instead of reusing evidence from two sessions back.
  bool consume(bool software_restart, uint32_t clock_now, uint32_t rtc_now_secs) {
    const bool armed = magic == MAGIC;
    magic = 0;
    if (!armed || !software_restart) return false;
    if (rtc_now_secs == 0 || rtc_now_secs < rtc_secs) return false;   // no counter, or it restarted with the power
    if (clock_now < epoch) return false;                              // an older floor came back, not the ticked time
    // Real time passed since the stamp, against how far the clock moved in it. The
    // extra second covers both readings being whole seconds.
    const uint32_t elapsed = rtc_now_secs - rtc_secs + 1;
    const uint32_t advanced = clock_now - epoch;
    const uint32_t lost = elapsed > advanced ? elapsed - advanced : 0;
    if (behind_secs > MAX_BEHIND_SECS || lost > MAX_BEHIND_SECS - behind_secs) return false;
    behind_secs += lost;
    return true;
  }
};
