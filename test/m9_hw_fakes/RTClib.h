// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
struct DateTime {
  DateTime(uint16_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
  uint32_t unixtime() const { return 0; }
};
