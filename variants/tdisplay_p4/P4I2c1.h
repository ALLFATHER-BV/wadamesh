// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// I2C controller 1 (Wire1) serves two pin pairs on this board: the keyboard
// expansion (TCA8418 @0x34 + its XL9555 @0x20) on SDA 46 / SCL 45, and the
// ES8311 audio codec on 20/21. The P4 has two general-purpose I2C controllers
// and controller 0 is the main board bus (XL9535, touch, RTC, gauge), so the
// two take turns on controller 1.
//
// p4I2c1Acquire() takes the bus, points Wire1 at the route asked for (re-pinning
// only when it is on the other one, or not running at all) and returns it;
// p4I2c1Release() hands it back. Every Wire1 transaction on this board must sit
// between the two: the codec is brought up lazily, on the first sound, from
// whichever task plays it, while the keyboard polls from the UI loop.
#if defined(HAS_TDISPLAY_P4)

#include <stdint.h>

class TwoWire;

enum P4I2c1Route : uint8_t {
  P4_I2C1_KEYBOARD,
  P4_I2C1_AUDIO,
};

// nullptr when the bus is busy for longer than wait_ms, or would not start.
TwoWire* p4I2c1Acquire(P4I2c1Route route, uint32_t wait_ms);
void     p4I2c1Release();

#endif
