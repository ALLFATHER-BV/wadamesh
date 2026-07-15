// SPDX-License-Identifier: GPL-3.0-or-later
#include "PagerEncoder.h"

#if defined(HAS_PAGER_ENCODER) && defined(ESP32)

#include <Arduino.h>

#ifndef ROTARY_A
  #define ROTARY_A 40
#endif
#ifndef ROTARY_B
  #define ROTARY_B 41
#endif
#ifndef ROTARY_C
  #define ROTARY_C 7
#endif

// Mechanical detent ratio: most EC11-style encoders emit 4 quadrature
// transitions per detent (rest state -> rest state). NOT independently
// verified against this exact part — confirm on hardware (Milestone 7) and
// adjust if this encoder differs.
#ifndef PAGER_ENCODER_STEPS_PER_DETENT
  #define PAGER_ENCODER_STEPS_PER_DETENT 4
#endif

// Standard Gray-code quadrature transition table: index = (prev_AB << 2 |
// curr_AB), value = +1 (CW), -1 (CCW), or 0 (bounce/invalid transition).
static const int8_t kQuadTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0,
};

static volatile int32_t s_raw_delta = 0;
static volatile uint8_t s_ab_state = 0;
static bool s_inited = false;

static void IRAM_ATTR pagerEncoderISR() {
  const uint8_t a = (uint8_t)digitalRead(ROTARY_A);
  const uint8_t b = (uint8_t)digitalRead(ROTARY_B);
  const uint8_t curr = (uint8_t)((a << 1) | b);
  const uint8_t idx = (uint8_t)((s_ab_state << 2) | curr);
  s_raw_delta += kQuadTable[idx];
  s_ab_state = curr;
}

void pagerEncoderBegin() {
  if (s_inited) return;
  s_inited = true;
  pinMode(ROTARY_A, INPUT_PULLUP);
  pinMode(ROTARY_B, INPUT_PULLUP);
  pinMode(ROTARY_C, INPUT_PULLUP);
  s_ab_state = (uint8_t)((digitalRead(ROTARY_A) << 1) | digitalRead(ROTARY_B));
  attachInterrupt(digitalPinToInterrupt(ROTARY_A), pagerEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ROTARY_B), pagerEncoderISR, CHANGE);
}

int pagerEncoderReadDelta() {
  noInterrupts();
  int32_t raw = s_raw_delta;
  s_raw_delta = raw % PAGER_ENCODER_STEPS_PER_DETENT;   // keep the partial-detent remainder
  interrupts();
  // Confirmed inverted on hardware (2026-07-07): kQuadTable's CW/CCW sign convention
  // doesn't match this physical part's wiring -- turning the knob the way a user expects
  // to move forward/down moved focus and page-scroll backward/up instead. Negate here
  // (not the table) so every consumer (nav NEXT/PREV, Alt+turn tab-switch, Alt+turn page
  // scroll) inherits the corrected sense from one place.
  return -(int)(raw / PAGER_ENCODER_STEPS_PER_DETENT);
}

bool pagerEncoderClickHeld() {
  return digitalRead(ROTARY_C) == LOW;   // active-low, matches TDeckTrackball's click idiom
}

#endif
