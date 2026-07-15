// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// T-LoRa Pager rotary encoder: a true quadrature A/B pair (GPIO 40/41) with
// the center press on GPIO 7. Decoded via ISR edge-counting in the same
// spirit as TDeckTrackball.cpp's direction pins, but this is a genuine
// quadrature pair rather than 4 independent direction pulses, so the ISR does
// a standard Gray-code state-transition lookup (both edges of both A and B)
// instead of a plain per-pin counter, and pagerEncoderReadDelta() returns
// signed detents rather than a 2D motion vector.
#if defined(HAS_PAGER_ENCODER) && defined(ESP32)

#include <stdint.h>

/** Configure the A/B quadrature pins + press pin and attach the ISRs.
 *  One-shot. */
void pagerEncoderBegin();

/** Signed detents accumulated since the last call (0 if none). Positive =
 *  clockwise. Safe to call from any single consistent context. */
int pagerEncoderReadDelta();

/** True while the encoder's center button is held (active-low GPIO 7). */
bool pagerEncoderClickHeld();

#endif
