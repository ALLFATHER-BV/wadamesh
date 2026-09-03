// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pure decode/encode half of HardwareRtcClock (issue #383), split out with
// no Arduino/Wire dependency so it can be exercised on the host — see
// test/test_hw_rtc_codec.cpp. This is where every "an untrustworthy hardware
// read must never become system or protocol time" rule actually lives, and the
// cases that matter (integrity bit latched, non-BCD nibbles, 31 February, a
// chip asserting 2043) are exactly the ones nobody can reproduce on demand with
// real hardware.
//
// The PCF8563 and the PCF85063A share this layout completely: seven registers,
// same field masks, same bit 7 of the first register meaning "my contents are
// not trustworthy" (VL on the 8563, OS on the 85063). Only the BASE ADDRESS of
// the block differs, and that lives in HardwareRtcClock.

#include <stdint.h>

enum class HwRtcStatus : uint8_t {
  NotProbed,      // begin() never ran (board has no declared chip)
  Absent,         // no ACK at the chip address, or a failed transfer
  WrongChip,      // answered, but failed the chip-identity proof
  IntegrityLost,  // VL/OS latched — the chip lost power, contents are garbage
  Decode,         // BCD or calendar field out of range
  Implausible,    // decoded fine, but the epoch is outside sane bounds
  Ok,
};

inline const char* hwRtcStatusName(HwRtcStatus s) {
  switch (s) {
    case HwRtcStatus::NotProbed:     return "not probed";
    case HwRtcStatus::Absent:        return "absent";
    case HwRtcStatus::WrongChip:     return "wrong chip";
    case HwRtcStatus::IntegrityLost: return "integrity lost";
    case HwRtcStatus::Decode:        return "decode failed";
    case HwRtcStatus::Implausible:   return "implausible date";
    case HwRtcStatus::Ok:            return "ok";
  }
  return "?";
}

// Epoch <-> proleptic-Gregorian calendar (Howard Hinnant's civil-days
// algorithms). Done here rather than through RTClib's DateTime because this
// header is also compiled into the two ESP-IDF builds (Tanmatsu, T-Display P4),
// which do not carry RTClib as a component, and into the host test, which has no
// Arduino at all.
inline int32_t hwRtcDaysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= (m <= 2);
  const int32_t  era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

inline void hwRtcCivilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d) {
  z += 719468;
  const int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const int32_t  yy  = (int32_t)yoe + era * 400;
  const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const uint32_t mp  = (5u * doy + 2u) / 153u;
  d = doy - (153u * mp + 2u) / 5u + 1u;
  m = mp + (mp < 10u ? 3u : (uint32_t)-9);
  y = yy + (int32_t)(m <= 2u);
}

inline uint8_t hwRtcDaysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t k[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return k[month - 1];
}

inline bool hwRtcBcdValid(uint8_t v) { return (v & 0x0F) <= 9 && ((v >> 4) & 0x0F) <= 9; }
inline uint8_t hwRtcBcd2Dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
inline uint8_t hwRtcDec2Bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// Seven raw registers -> epoch, refusing anything that cannot be believed.
// Order of the checks is the order of severity: a chip that says "do not trust
// me" is never decoded at all, however plausible its digits look.
inline bool hwRtcDecodeTime(const uint8_t r[7], uint32_t min_epoch, uint32_t max_epoch,
                            uint32_t& out_epoch, HwRtcStatus& why) {
  // Bit 7 of the seconds register: VL on the PCF8563, OS on the PCF85063A.
  // RTClib masks this off and hands back the stale contents regardless, which is
  // how a hard-powered-off M9 came back believing the time it shut down at.
  if (r[0] & 0x80) { why = HwRtcStatus::IntegrityLost; return false; }

  // The PCF8563's month bit 7 is its century flag; the 0x1F mask drops it.
  const uint8_t sec_b = r[0] & 0x7F, min_b = r[1] & 0x7F, hr_b = r[2] & 0x3F;
  const uint8_t day_b = r[3] & 0x3F, mon_b = r[5] & 0x1F, yr_b = r[6];

  if (!hwRtcBcdValid(sec_b) || !hwRtcBcdValid(min_b) || !hwRtcBcdValid(hr_b) ||
      !hwRtcBcdValid(day_b) || !hwRtcBcdValid(mon_b) || !hwRtcBcdValid(yr_b)) {
    why = HwRtcStatus::Decode; return false;
  }

  const uint8_t  sec = hwRtcBcd2Dec(sec_b), min = hwRtcBcd2Dec(min_b), hr = hwRtcBcd2Dec(hr_b);
  const uint8_t  day = hwRtcBcd2Dec(day_b), mon = hwRtcBcd2Dec(mon_b);
  const uint16_t year = (uint16_t)(2000 + hwRtcBcd2Dec(yr_b));

  if (sec > 59 || min > 59 || hr > 23 || mon < 1 || mon > 12 ||
      day < 1 || day > hwRtcDaysInMonth(year, mon)) {
    why = HwRtcStatus::Decode; return false;
  }

  const uint32_t epoch = (uint32_t)((int64_t)hwRtcDaysFromCivil((int32_t)year, mon, day) * 86400LL)
                       + hr * 3600u + min * 60u + sec;
  if (epoch <= min_epoch || epoch > max_epoch) { why = HwRtcStatus::Implausible; return false; }

  out_epoch = epoch;
  why = HwRtcStatus::Ok;
  return true;
}

// Epoch -> seven raw registers. Writing seconds with bit 7 clear is also what
// CLEARS the VL/OS latch, so an accepted external sync is what makes the chip
// trustworthy again. Fails outside the 2000-2099 window neither part can hold.
inline bool hwRtcEncodeTime(uint32_t epoch, uint8_t r[7]) {
  const int32_t  days = (int32_t)(epoch / 86400u);
  const uint32_t rem  = epoch % 86400u;
  int32_t  year = 0;
  uint32_t mon = 0, day = 0;
  hwRtcCivilFromDays(days, year, mon, day);
  if (year < 2000 || year > 2099) return false;

  r[0] = (uint8_t)(hwRtcDec2Bcd((uint8_t)(rem % 60u)) & 0x7F);
  r[1] = hwRtcDec2Bcd((uint8_t)((rem / 60u) % 60u));
  r[2] = hwRtcDec2Bcd((uint8_t)(rem / 3600u));
  r[3] = hwRtcDec2Bcd((uint8_t)day);
  r[4] = (uint8_t)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
  r[5] = hwRtcDec2Bcd((uint8_t)mon);         // century bit left 0: years 2000-2099
  r[6] = hwRtcDec2Bcd((uint8_t)(year % 100));
  return true;
}
