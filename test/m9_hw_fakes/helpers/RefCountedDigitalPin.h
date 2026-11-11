// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../Arduino.h"

class RefCountedDigitalPin {
  int _pin;
  int _active;
  int _claims = 0;
public:
  RefCountedDigitalPin(int pin, int active = HIGH) : _pin(pin), _active(active) {}
  void begin() {
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, _active ? LOW : HIGH);
  }
  void claim() {
    if (_claims++ == 0) digitalWrite(_pin, _active);
  }
  void release() {
    if (_claims > 0 && --_claims == 0) digitalWrite(_pin, _active ? LOW : HIGH);
  }
};
