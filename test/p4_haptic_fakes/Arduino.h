// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-ins for variants/tdisplay_p4/P4Haptic.cpp (test/test_p4_haptic.cpp).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
using std::min;
extern uint32_t g_ms;          // virtual time, advanced by vTaskDelay / delayMicroseconds
inline void delayMicroseconds(uint32_t) {}
