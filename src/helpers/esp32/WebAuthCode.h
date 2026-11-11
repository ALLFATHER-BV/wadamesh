// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_random.h>
#endif
// Off-device (host tests) the caller provides millis() and esp_random().

// The one 6-digit code that unlocks every browser feature on the web port
// (File Transfer, VNC / Remote UI, Remote terminal). A browser sends it as the
// first WebSocket text frame "AUTH nnnnnn"; 3 wrong tries lock everyone out
// for 30 s. The code is created when the first feature turns on and dropped
// when the last one turns off, so enabling a second feature never rotates it.
class WebAuthCode {
public:
  void ensure() {
    if (_code != 0) return;
    _failures = 0;
    _blocked_until_ms = 0;
    _code = 100000u + esp_random() % 900000u;
  }
  void disarm() {
    _code = 0;
    _failures = 0;
    _blocked_until_ms = 0;
  }
  uint32_t code() const { return _code; }

  bool matches(const uint8_t* text, size_t len) const {
    if (_code == 0 || !text || len != 6) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < len; ++i) {
      if (text[i] < '0' || text[i] > '9') return false;
      value = value * 10u + static_cast<uint32_t>(text[i] - '0');
    }
    return value == _code;
  }
  bool allowed() const {
    return _blocked_until_ms == 0 ||
           static_cast<int32_t>(millis() - _blocked_until_ms) >= 0;
  }
  void noteFailure() {
    if (++_failures >= 3) {
      _failures = 0;
      _blocked_until_ms = millis() + 30000u;
    }
  }
  void noteSuccess() {
    _failures = 0;
    _blocked_until_ms = 0;
  }

private:
  volatile uint32_t _code = 0;
  volatile uint32_t _blocked_until_ms = 0;
  volatile uint8_t _failures = 0;
};

extern WebAuthCode g_web_auth;
