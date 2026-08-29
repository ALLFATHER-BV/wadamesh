// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>

// Front buttons on the board's AW9523 @0x59 (P00-P07, active-low), per Attaky's
// hardware catalog:
//   P00 DOWN   P01 LEFT   P02 SELECT   P03 RIGHT   P04 UP
//   P05 BTN_L1 (top-left shoulder)     P06 BTN_R2 (top-right shoulder)
//   P07 POWER_BTN
// POWER_BTN short-press toggles the panel (a long press is the board's hardware
// power-cut, outside firmware control). The D-pad + SELECT feed the UI's focus
// navigation. L1/R2 are read but deliberately unbound — nothing claims them yet.

/** D-pad events drained by attakyNavKeyRead(). */
enum AttakyNavKey {
  ATTAKY_NAV_NONE = 0,
  ATTAKY_NAV_UP,
  ATTAKY_NAV_DOWN,
  ATTAKY_NAV_LEFT,
  ATTAKY_NAV_RIGHT,
  ATTAKY_NAV_SELECT,
};

/** Poll the expander. Call once per UI tick; rate-limits its own I2C traffic. */
void attakyKeysPoll();

/** True once per POWER_BTN press edge (consumes the event). */
bool attakyPowerKeyPressed();

/** Next queued D-pad event, or ATTAKY_NAV_NONE when the queue is empty. */
int attakyNavKeyRead();

/** Drop every queued D-pad event (screen-off wake, remote mode). */
void attakyNavKeysDiscard();

/** Whether the @0x59 expander answered at init (false = keys unavailable). */
bool attakyKeysPresent();
