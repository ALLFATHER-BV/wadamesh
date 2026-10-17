// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// TCA9535 I/O expander on the SenseCAP Indicator. It carries the SX1262 control
// lines, the LCD and touch resets, the RP2040 reset and the radio strap, so the
// radio and the panel both depend on it. Header-only and hardware-free: the I2C
// transport is an ExpanderBus, which the host test replaces with a fake.

#include <stddef.h>
#include <stdint.h>

namespace indicator {

// Pin n is port n>>3, bit n&7.
namespace pins {
constexpr uint8_t kLoraNss = 0;
constexpr uint8_t kLoraReset = 1;
constexpr uint8_t kLoraBusy = 2;      // input
constexpr uint8_t kLoraDio1 = 3;      // input
constexpr uint8_t kLcdCs = 4;
constexpr uint8_t kLcdReset = 5;
constexpr uint8_t kTouchInt = 6;      // input
constexpr uint8_t kTouchReset = 7;
constexpr uint8_t kRp2040Reset = 8;
constexpr uint8_t kRadioStrap = 11;   // input, high = TCXO
}  // namespace pins

// The expander's open-drain /INT output (active low) reaches the ESP32 on this GPIO.
constexpr uint8_t kExpanderIntGpio = 42;

struct ExpanderBus {
  virtual bool read(uint8_t reg, uint8_t* data, size_t n) = 0;
  virtual bool write(uint8_t reg, const uint8_t* data, size_t n) = 0;
  virtual ~ExpanderBus() = default;
};

class Tca9535 {
public:
  explicit Tca9535(ExpanderBus& bus) : _bus(bus) {}

  // The shadows come from the chip, not from the power-on defaults: a warm reset
  // or a previous firmware may have left pins driven, and writing 0xFF/0x00 back
  // would glitch them. Polarity inversion is cleared, since a previous firmware
  // that set it would invert BUSY, DIO1 and the strap; it changes no output, and
  // it is written only when it is not already 0. The input read releases /INT.
  bool begin() {
    uint8_t out[2], cfg[2], pol[2], in[2];
    if (!_bus.read(kRegOutput0, out, 2) || !_bus.read(kRegConfig0, cfg, 2) ||
        !_bus.read(kRegPolarity0, pol, 2)) return false;
    _out[0] = out[0]; _out[1] = out[1];
    _cfg[0] = cfg[0]; _cfg[1] = cfg[1];
    static constexpr uint8_t kNoInversion[2] = {0x00, 0x00};
    if ((pol[0] | pol[1]) && !_bus.write(kRegPolarity0, kNoInversion, 2)) return false;
    return readInputs(in);
  }

  // Latch the level before turning the pin into an output, so it never drives
  // the stale level for a moment.
  bool setOutput(uint8_t pin, bool high) {
    if (pin > 15) return false;
    const uint8_t port = pin >> 3;
    const uint8_t mask = (uint8_t)(1U << (pin & 7));
    const uint8_t out[2] = {_out[0], _out[1]};
    const uint8_t cfg[2] = {_cfg[0], _cfg[1]};
    if (high) _out[port] |= mask;
    else _out[port] &= (uint8_t)~mask;
    _cfg[port] &= (uint8_t)~mask;
    // A pin that already is an output needs only the level (the radio's NSS
    // toggles on every SPI command).
    if (_bus.write(kRegOutput0, _out, 2) &&
        (_cfg[port] == cfg[port] || _bus.write(kRegConfig0, _cfg, 2)))
      return true;
    _out[0] = out[0]; _out[1] = out[1];
    _cfg[0] = cfg[0]; _cfg[1] = cfg[1];
    return false;
  }

  bool setInput(uint8_t pin) {
    if (pin > 15) return false;
    const uint8_t cfg[2] = {_cfg[0], _cfg[1]};
    _cfg[pin >> 3] |= (uint8_t)(1U << (pin & 7));
    if (_bus.write(kRegConfig0, _cfg, 2)) return true;
    _cfg[0] = cfg[0]; _cfg[1] = cfg[1];
    return false;
  }

  bool readPort0(uint8_t& value) {
    if (!_bus.read(kRegInput0, &value, 1)) return false;
    notify(value);
    return true;
  }

  // A port-1 pin reads both input bytes, so a port-0 change that this read
  // clears from /INT still reaches the observer.
  bool readPin(uint8_t pin, bool& high) {
    if (pin > 15) return false;
    const uint8_t mask = (uint8_t)(1U << (pin & 7));
    if (pin < 8) {
      uint8_t value = 0;
      if (!readPort0(value)) return false;
      high = (value & mask) != 0;
      return true;
    }
    uint8_t in[2];
    if (!readInputs(in)) return false;
    high = (in[1] & mask) != 0;
    return true;
  }

  uint8_t output(uint8_t port) const { return _out[port & 1]; }
  uint8_t config(uint8_t port) const { return _cfg[port & 1]; }

  // Every read of input port 0 reports the raw byte here. Reading the port clears
  // /INT, so whoever watches DIO1 must see the reads other callers make too.
  void setPort0Observer(void (*fn)(uint8_t value, void* ctx), void* ctx) {
    _observer = fn;
    _observerCtx = ctx;
  }

private:
  static constexpr uint8_t kRegInput0 = 0x00;
  static constexpr uint8_t kRegOutput0 = 0x02;
  static constexpr uint8_t kRegPolarity0 = 0x04;
  static constexpr uint8_t kRegConfig0 = 0x06;

  bool readInputs(uint8_t in[2]) {
    if (!_bus.read(kRegInput0, in, 2)) return false;
    notify(in[0]);
    return true;
  }

  void notify(uint8_t value) {
    if (_observer) _observer(value, _observerCtx);
  }

  ExpanderBus& _bus;
  uint8_t _out[2] = {0, 0};
  uint8_t _cfg[2] = {0xFF, 0xFF};
  void (*_observer)(uint8_t value, void* ctx) = nullptr;
  void* _observerCtx = nullptr;
};

// The strap is read five times; three or more highs win, so one noisy read
// cannot flip the TCXO selection.
inline bool strapMajority(const bool samples[5]) {
  int high = 0;
  for (int i = 0; i < 5; ++i) high += samples[i] ? 1 : 0;
  return high >= 3;
}

}  // namespace indicator
