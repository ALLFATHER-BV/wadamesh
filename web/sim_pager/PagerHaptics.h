// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// T-LoRa Pager simulator: the vibration motor prints what it would play.
#include <stdint.h>
#include <stdio.h>
enum class PagerHaptic : uint8_t { Message, Direct, Mention, Tick };
enum PagerVibPattern : uint8_t {
  PAGER_VIB_OFF = 0, PAGER_VIB_ONCE, PAGER_VIB_TWICE, PAGER_VIB_THRICE, PAGER_VIB_LONG, PAGER_VIB_PULSE,
  PAGER_VIB_PATTERNS
};
static inline void pagerHapticsBegin() {}
static inline bool pagerHapticsPresent() { return true; }
static inline void pagerHapticsPlay(PagerHaptic k) {
  static const char* n[] = { "message", "direct", "mention", "tick" };
  if (k != PagerHaptic::Tick) printf("[sim] vibrate: %s\n", n[(int)k]);
}
static inline void pagerHapticsPattern(uint8_t pattern, uint8_t strength) {
  static const char* n[] = { "off", "once", "twice", "three times", "long", "pulse" };
  if (pattern && pattern < PAGER_VIB_PATTERNS) printf("[sim] vibrate: %s, strength %u\n", n[pattern], (unsigned)strength);
}
static inline void pagerHapticsClick(uint8_t) {}
static inline void pagerHapticsIdleTick() {}
