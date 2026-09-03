// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the believability rules a battery-backed RTC read has to pass
// before it is allowed to become system or protocol time (issue #383).
//
// These are the cases that cannot be produced on demand with real hardware: you
// cannot ask a PCF8563 to latch its VL bit, or to hand back 31 February, on a
// bench. They are also exactly the cases that broke in the field — a T-Display
// P4 asserting 2043, and a hard-powered-off ThinkNode M9 whose stale registers
// read back as "the wall clock froze when I pulled the slider".
//
//   g++ -std=c++17 -I src -o /tmp/t test/test_hw_rtc_codec.cpp && /tmp/t

#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "helpers/HwRtcCodec.h"

// The same window ClockFloorRTC enforces (its own constants are compile-time
// expressions over __DATE__, which is not what we want pinned in a test).
static constexpr uint32_t kMin = 1715770351UL;                  // 15 May 2024
static constexpr uint32_t kMax = (2029u - 1970u) * 31557600u;   // ~ build year + 3

// Build a raw register block the way either chip lays it out.
static void regs(uint8_t out[7], int y, int mo, int d, int h, int mi, int s,
                 bool integrity_lost = false) {
  out[0] = (uint8_t)(hwRtcDec2Bcd((uint8_t)s) | (integrity_lost ? 0x80 : 0x00));
  out[1] = hwRtcDec2Bcd((uint8_t)mi);
  out[2] = hwRtcDec2Bcd((uint8_t)h);
  out[3] = hwRtcDec2Bcd((uint8_t)d);
  out[4] = 0;                       // weekday: never trusted on the read path
  out[5] = hwRtcDec2Bcd((uint8_t)mo);
  out[6] = hwRtcDec2Bcd((uint8_t)(y % 100));
}

static uint32_t epochOf(int y, int mo, int d, int h, int mi, int s) {
  struct tm tmv = {};
  tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
  tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = s;
  return (uint32_t)timegm(&tmv);
}

int main() {
  uint8_t r[7];
  uint32_t got = 0;
  HwRtcStatus why = HwRtcStatus::NotProbed;

  // ---- the happy path, against an independent epoch implementation ----
  regs(r, 2026, 9, 2, 14, 5, 33);
  assert(hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Ok);
  assert(got == epochOf(2026, 9, 2, 14, 5, 33));

  // A leap day is a real date and must survive.
  regs(r, 2028, 2, 29, 0, 0, 0);
  assert(hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(got == epochOf(2028, 2, 29, 0, 0, 0));

  // ---- integrity: the chip says its own contents are untrustworthy ----
  // The digits here are perfectly plausible — this is the M9 case, where the
  // registers held the shutdown time and the old path believed them.
  regs(r, 2026, 9, 2, 14, 5, 33, /*integrity_lost=*/true);
  got = 0;
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::IntegrityLost);
  assert(got == 0);   // nothing is handed back on a refusal

  // ---- non-BCD nibbles ----
  regs(r, 2026, 9, 2, 14, 5, 33);
  r[1] = 0x7F;   // minutes: both nibbles out of range
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Decode);

  regs(r, 2026, 9, 2, 14, 5, 33);
  r[6] = 0xAB;   // year
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Decode);

  // ---- calendar fields that are valid BCD but not a date ----
  regs(r, 2026, 2, 29, 0, 0, 0);   // 2026 is not a leap year
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Decode);

  regs(r, 2026, 0, 15, 0, 0, 0);   // month 0 — how a PCF85063A read with the
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));   // 8563's register map lands
  assert(why == HwRtcStatus::Decode);

  regs(r, 2026, 13, 1, 0, 0, 0);
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Decode);

  regs(r, 2026, 9, 0, 0, 0, 0);    // day 0
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Decode);

  regs(r, 2026, 9, 2, 25, 0, 0);   // hour 25
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Decode);

  // ---- plausibility: decodes fine, cannot be a real clock ----
  regs(r, 2043, 1, 1, 0, 0, 0);    // the T-Display P4's live failure
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Implausible);

  regs(r, 2001, 1, 1, 0, 0, 0);    // before the firmware could have existed
  assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
  assert(why == HwRtcStatus::Implausible);

    // The lower bound is the ESP32 core's exact unset-clock seed, not a valid
    // synchronization. Equality must fail too or it can be promoted to current.
    time_t min_tt = (time_t)kMin;
    struct tm min_tm; gmtime_r(&min_tt, &min_tm);
    regs(r, min_tm.tm_year + 1900, min_tm.tm_mon + 1, min_tm.tm_mday,
      min_tm.tm_hour, min_tm.tm_min, min_tm.tm_sec);
    assert(!hwRtcDecodeTime(r, kMin, kMax, got, why));
    assert(why == HwRtcStatus::Implausible);

  // ---- encode/decode round-trip, including the weekday the chips want ----
  for (uint32_t t : { epochOf(2024, 5, 16, 0, 0, 0), epochOf(2026, 9, 2, 14, 5, 33),
                      epochOf(2028, 2, 29, 23, 59, 59), epochOf(2027, 12, 31, 12, 0, 0) }) {
    uint8_t w[7];
    assert(hwRtcEncodeTime(t, w));
    assert((w[0] & 0x80) == 0);   // writing seconds is what clears the VL/OS latch
    // Weekday, 0 = Sunday, checked against the C library rather than the same
    // arithmetic the encoder used.
    const time_t tt = (time_t)t;
    struct tm tmv; gmtime_r(&tt, &tmv);
    assert(w[4] == (uint8_t)tmv.tm_wday);
    uint32_t back = 0;
    assert(hwRtcDecodeTime(w, kMin, kMax, back, why));
    assert(back == t);
  }

  // Outside the 2000-2099 window neither part can represent, the encoder must
  // refuse rather than silently wrap the two-digit year.
  uint8_t w[7];
  assert(!hwRtcEncodeTime(epochOf(1999, 12, 31, 23, 59, 59), w));
  assert(!hwRtcEncodeTime(epochOf(2100, 1, 1, 0, 0, 0), w));

  printf("test_hw_rtc_codec: all cases pass\n");
  return 0;
}
