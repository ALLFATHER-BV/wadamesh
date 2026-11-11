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

// Mechanical detent ratio: the Pager's encoder emits 4 quadrature transitions
// per detent and rests with A and B equal. 2 (half-step parts that rest on
// both equal states) is also handled below.
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

// Detents are counted when the knob arrives back at a rest position, not by
// dividing a running transition count by 4. The old divide kept the remainder
// across detents: one missed or bounced edge left the count off by 2 at rest,
// and from then on every change of direction swallowed a detent (+2 - 4 = -2,
// which divides to 0) until another stray edge happened to realign it. Users
// saw "rocking between two detents drops inputs, turning twice the same way
// responds" (#643). Re-syncing at every rest position removes that state.
static volatile int32_t s_detents = 0;   // whole detents not yet read
static volatile int8_t  s_acc = 0;       // transitions since the last rest position
static volatile uint8_t s_ab_state = 0;
static uint8_t s_rest = 0x3;             // A/B at rest (both high with the pull-ups)
static bool s_inited = false;

static inline bool IRAM_ATTR pagerEncoderAtRest(uint8_t ab) {
#if PAGER_ENCODER_STEPS_PER_DETENT == 2
  return ab == 0x0 || ab == 0x3;
#else
  return ab == s_rest;
#endif
}

static void IRAM_ATTR pagerEncoderISR() {
  const uint8_t a = (uint8_t)digitalRead(ROTARY_A);
  const uint8_t b = (uint8_t)digitalRead(ROTARY_B);
  const uint8_t curr = (uint8_t)((a << 1) | b);
  if (curr == s_ab_state) return;   // the other pin's edge, already seen
  int8_t acc = (int8_t)(s_acc + kQuadTable[(s_ab_state << 2) | curr]);
  if (acc > 8) acc = 8;
  if (acc < -8) acc = -8;
  s_ab_state = curr;
  if (pagerEncoderAtRest(curr)) {
    // More than half a detent's transitions in one direction: one detent.
    // Tolerates a missed edge on a fast turn; a partial turn that comes back
    // to the same rest sums to 0 and counts nothing.
    const int8_t half = PAGER_ENCODER_STEPS_PER_DETENT / 2;
    if (acc >= half) s_detents++;
    else if (acc <= -half) s_detents--;
    acc = 0;
  }
  s_acc = acc;
}

void pagerEncoderBegin() {
  if (s_inited) return;
  s_inited = true;
  pinMode(ROTARY_A, INPUT_PULLUP);
  pinMode(ROTARY_B, INPUT_PULLUP);
  pinMode(ROTARY_C, INPUT_PULLUP);
  s_ab_state = (uint8_t)((digitalRead(ROTARY_A) << 1) | digitalRead(ROTARY_B));
  // The knob sits in a detent at boot; take that as the rest position. A/B
  // differing means it was caught mid-turn, so fall back to both high.
  s_rest = (s_ab_state == 0x0 || s_ab_state == 0x3) ? s_ab_state : 0x3;
  s_acc = 0;
  s_detents = 0;
  attachInterrupt(digitalPinToInterrupt(ROTARY_A), pagerEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ROTARY_B), pagerEncoderISR, CHANGE);
}

int pagerEncoderReadDelta() {
  noInterrupts();
  const int32_t d = s_detents;
  s_detents = 0;
  interrupts();
  // Confirmed inverted on hardware (2026-07-07): kQuadTable's CW/CCW sign convention
  // doesn't match this physical part's wiring -- turning the knob the way a user expects
  // to move forward/down moved focus and page-scroll backward/up instead. Negate here
  // (not the table) so every consumer (nav NEXT/PREV, Alt+turn tab-switch, Alt+turn page
  // scroll) inherits the corrected sense from one place.
  return -(int)d;
}

bool pagerEncoderClickHeld() {
  return digitalRead(ROTARY_C) == LOW;   // active-low, matches TDeckTrackball's click idiom
}

#endif
