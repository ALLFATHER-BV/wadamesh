// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// The GPS serial-speed rules behind touchPrefsGetGpsBaud / touchPrefsSetGpsBaudFound
// (TouchPrefsStore.cpp), pure so the probe can be host-tested against them
// (test/test_gps_baud_probe.cpp).
//
// `chosen` is the user's pick in the settings blob (0 = none, "Auto"). `found` is the
// speed the UART probe last locked on while nothing was picked (CAP_GPS_BAUD_FIND
// boards), kept in its own NVS key so an older firmware never reads it as a choice.
namespace GpsBaudPrefs {

inline bool sane(uint32_t baud) { return baud >= 4800 && baud <= 921600; }

// The speed the UART opens at: the choice, else what the probe found, else the
// board's compile-time default.
inline uint32_t effective(uint32_t chosen, uint32_t found, uint32_t fallback) {
  if (chosen != 0) return chosen;
  return sane(found) ? found : fallback;
}

// Write the found speed only when it is sane and differs from what is stored: once
// per change of receiver, never per sentence.
inline bool foundNeedsWrite(uint32_t baud, bool have_found, uint32_t stored) {
  return sane(baud) && !(have_found && stored == baud);
}

}  // namespace GpsBaudPrefs
