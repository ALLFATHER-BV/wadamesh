// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>

// Front buttons on the board's AW9523 @0x59 (P00-P07, active-low). Only
// POWER_BTN (P07) is used: short-press toggles the panel. A long press is the
// board's hardware power-cut, outside firmware control.

/** Poll the expander. Call once per UI tick; rate-limits its own I2C traffic. */
void attakyKeysPoll();

/** True once per POWER_BTN press edge (consumes the event). */
bool attakyPowerKeyPressed();

/** Whether the @0x59 expander answered at init (false = keys unavailable). */
bool attakyKeysPresent();
