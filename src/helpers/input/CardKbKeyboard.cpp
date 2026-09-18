// SPDX-License-Identifier: GPL-3.0-or-later
#include "CardKbKeyboard.h"

#if defined(HAS_CARDKB) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t  kCardKbAddress        = 0x5F;
constexpr uint32_t kPresentPollMs        = 12;
constexpr uint32_t kAbsentProbeMs        = 1000;
constexpr uint8_t  kDisconnectThreshold = 3;

uint8_t s_ring[8] = {};
uint8_t s_head = 0;
uint8_t s_tail = 0;
uint8_t s_failures = 0;
bool s_present = false;
uint32_t s_next_poll_ms = 0;

void ringPush(uint8_t key) {
  const uint8_t next = (uint8_t)((s_head + 1) & 7u);
  if (next != s_tail) {
    s_ring[s_head] = key;
    s_head = next;
  }
}

void ringClear() {
  s_head = s_tail = 0;
}
}  // namespace

void cardKbPoll() {
  const uint32_t now = millis();
  if ((int32_t)(now - s_next_poll_ms) < 0) return;
  s_next_poll_ms = now + (s_present ? kPresentPollMs : kAbsentProbeMs);
  const bool was_present = s_present;

  // The board bus is initialized by ESP32Board. Do not call Wire.begin() here:
  // V4-R8 shares this same TwoWire instance with touch, RTC and sensors.
  const int received = Wire.requestFrom((int)kCardKbAddress, 1);
  if (received != 1) {
    while (Wire.available()) (void)Wire.read();
    if (s_failures < kDisconnectThreshold) ++s_failures;
    if (s_failures >= kDisconnectThreshold && s_present) {
      s_present = false;
      ringClear();
      s_next_poll_ms = now + kAbsentProbeMs;
    }
    return;
  }

  s_failures = 0;
  s_present = true;
  if (!was_present) s_next_poll_ms = now + kPresentPollMs;
  const int key = Wire.read();
  if (key > 0) ringPush((uint8_t)key);
}

int cardKbReadKey() {
  if (s_tail == s_head) return 0;
  const uint8_t key = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 7u);
  return key;
}

bool cardKbPresent() {
  return s_present;
}

#endif