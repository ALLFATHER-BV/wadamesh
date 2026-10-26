// SPDX-License-Identifier: GPL-3.0-or-later
//
// T-LoRa Pager vibration motor: a TI DRV2605 haptic driver (I2C 0x5A, powered by
// XL9555 DRV_EN, which TLoraPagerBoard::begin() switches on) driving an ERM
// motor. Set up the way LilyGoLib does for this board: ERM open loop, effect
// library 1, internal trigger.
//
// Effects run from the chip's own ROM library, so a pattern costs one short I2C
// burst and no CPU time while it plays. Between patterns the chip is parked in
// standby (pagerHapticsIdleTick()), a few µA instead of its ready-state draw.
#pragma once

#include <stdint.h>

enum class PagerHaptic : uint8_t {
  Message,   // one strong buzz
  Direct,    // two buzzes
  Mention,   // three short buzzes
  Tick,      // one strong click (ROM effect 1), for key/encoder feedback
};

/** Probe + configure. Safe to call when the chip is absent: every other call is
 *  then a no-op. Logs one line to Serial either way. */
void pagerHapticsBegin();

/** True once the chip answered with a DRV260x id and took the configuration. */
bool pagerHapticsPresent();

/** Start a pattern, stopping one still playing. Non-blocking. */
void pagerHapticsPlay(PagerHaptic kind);

// The patterns Settings > Sound > Vibration offers per message kind.
enum PagerVibPattern : uint8_t {
  PAGER_VIB_OFF = 0, PAGER_VIB_ONCE, PAGER_VIB_TWICE, PAGER_VIB_THRICE, PAGER_VIB_LONG, PAGER_VIB_PULSE,
  PAGER_VIB_PATTERNS
};
/** Play pattern (PagerVibPattern) at strength 0 light, 1 medium, 2 strong. */
void pagerHapticsPattern(uint8_t pattern, uint8_t strength);
/** One key/knob click: 1 light, 2 strong (0 = nothing). */
void pagerHapticsClick(uint8_t level);

/** Call periodically (cheap: one comparison until a pattern has run out). Puts
 *  the chip back in standby once the pattern has finished. */
void pagerHapticsIdleTick();
