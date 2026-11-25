// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>

#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1

extern uint32_t fakeMillis;
extern bool fakeBusy;
extern unsigned fakeResets;
inline uint32_t millis() { return fakeMillis; }
inline void delay(uint32_t ms) { fakeMillis += ms; }
inline void delayMicroseconds(uint32_t) {}
inline void pinMode(int, int) {}
inline int digitalRead(int) { return fakeBusy ? HIGH : LOW; }
inline void digitalWrite(int pin, int value) {
  if (pin == 47 && value == LOW) ++fakeResets;
}
