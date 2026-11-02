// SPDX-License-Identifier: GPL-3.0-or-later
// RTClib's DateTime, as far as the provider uses it: a calendar date and time -> Unix seconds
// (UTC), the same days-from-civil arithmetic RTClib does for 2000..2099.
#pragma once
#include <stdint.h>
struct DateTime {
  uint32_t _t;
  DateTime(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    const int yy = (int)y - (mo <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153u * (mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long days = (long)era * 146097L + (long)doe - 719468L;
    _t = (uint32_t)(days * 86400L + h * 3600L + mi * 60L + s);
  }
  uint32_t unixtime() const { return _t; }
};
