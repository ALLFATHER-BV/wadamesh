// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// DIO1 of the SX1262 reaches the ESP32 only through the TCA9535, and any read of
// input port 0 clears the expander's /INT. So the radio callback cannot hang off
// a GPIO edge: it fires from whichever read first sees DIO1 go high, be that the
// dispatch task woken by /INT or a BUSY poll made by RadioLib. Header-only and
// hardware-free; the host test drives it with raw port-0 bytes.

#include <stdint.h>

#include "IndicatorExpander.h"

namespace indicator {

// Virtual pin numbers 0x40..0x4F name expander pins (P_LORA_NSS=0x40 and so on).
constexpr uint32_t kExpanderPinFlag = 0x40;

constexpr bool isExpanderPin(uint32_t pin) { return (pin & ~0x0FU) == kExpanderPinFlag; }

constexpr uint8_t expanderBit(uint32_t pin) { return (uint8_t)(pin & 0x0F); }

// Edge detector over the port-0 reads. The caller serialises every call (the
// HAL holds IndicatorIo::BusLock); the callback runs inside onPort0 and must not
// block.
class Dio1Edge {
public:
  // `last` keeps the level seen by the latest read, armed or not: arming while
  // DIO1 is already high must not count as a rise.
  void arm(void (*cb)()) {
    _cb = cb;
    _armed = cb != nullptr;
  }

  void disarm() { _armed = false; }

  bool onPort0(uint8_t port0) {
    const bool high = (port0 & kDio1Mask) != 0;
    const bool rise = high && !_last;
    _last = high;
    if (!rise || !_armed) return false;
    _cb();
    return true;
  }

  bool armed() const { return _armed; }

private:
  static constexpr uint8_t kDio1Mask = (uint8_t)(1U << pins::kLoraDio1);

  void (*_cb)() = nullptr;
  bool _armed = false;
  bool _last = false;
};

}  // namespace indicator
