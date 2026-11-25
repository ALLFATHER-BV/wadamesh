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
//
// A fixed code (#837, saved by the UI) replaces the random one for every session.
// A code that never changes can be worked through 3 tries at a time, so while one is
// set each lockout doubles (30 s, 1 min, 2 min ... capped at 1 h) and the lockout
// survives the code being dropped and re-armed. 3 tries an hour makes the million
// codes a matter of decades. weakFixed() refuses the codes people guess first.
class WebAuthCode {
public:
  static constexpr uint32_t kMaxCode      = 999999u;
  static constexpr uint32_t kLockMs       = 30000u;
  static constexpr uint32_t kMaxLockMs    = 3600000u;

  void ensure() {
    if (_code != 0) return;
    if (!_fixed) clearLockout();   // a fixed code keeps its guess history across sessions
    _code = _fixed ? _fixed : 100000u + esp_random() % 900000u;
  }
  // 0 = random per session. 000000 can't be fixed: a zero code means "disarmed".
  // Applies at once to a session in progress, so the code shown is the code that works.
  // A NEW code starts with a clean lockout: guesses at the old one say nothing about it.
  void setFixed(uint32_t code) {
    if (code > kMaxCode || weakFixed(code)) code = 0;
    if (code == _fixed) return;
    _fixed = code;
    clearLockout();
    if (_code == 0) return;
    _code = 0;
    ensure();
  }
  uint32_t fixedCode() const { return _fixed; }

  // Codes too easy to guess to keep for good: one digit repeated, a run up or down
  // (123456, 987654), and a repeated pair or triple (121212, 123123).
  static bool weakFixed(uint32_t code) {
    if (code == 0 || code > kMaxCode) return true;
    uint8_t d[6];
    for (int i = 5; i >= 0; --i) { d[i] = (uint8_t)(code % 10u); code /= 10u; }
    bool same = true, up = true, down = true, pair = true, triple = true;
    for (int i = 1; i < 6; ++i) {
      if (d[i] != d[0]) same = false;
      if (d[i] != (uint8_t)((d[i - 1] + 1) % 10)) up = false;
      if (d[i] != (uint8_t)((d[i - 1] + 9) % 10)) down = false;
      if (d[i] != d[i % 2]) pair = false;
      if (d[i] != d[i % 3]) triple = false;
    }
    return same || up || down || pair || triple;
  }

  void disarm() {
    _code = 0;
    if (!_fixed) clearLockout();
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
      uint32_t ms = kLockMs;
      if (_fixed) {
        for (uint8_t i = 0; i < _lockouts && ms < kMaxLockMs; ++i) ms *= 2u;
        if (ms > kMaxLockMs) ms = kMaxLockMs;
        if (_lockouts < 255) ++_lockouts;
      }
      _blocked_until_ms = millis() + ms;
      if (_blocked_until_ms == 0) _blocked_until_ms = 1;   // 0 means "not blocked"
    }
  }
  void noteSuccess() { clearLockout(); }
  // Whole seconds until the next try is allowed (0 = allowed now), for the browser's error.
  uint32_t retryAfterSecs() const {
    if (allowed()) return 0;
    return (uint32_t)(_blocked_until_ms - millis()) / 1000u + 1u;
  }

private:
  void clearLockout() {
    _failures = 0;
    _lockouts = 0;
    _blocked_until_ms = 0;
  }

  volatile uint32_t _code = 0;
  volatile uint32_t _fixed = 0;
  volatile uint32_t _blocked_until_ms = 0;
  volatile uint8_t _failures = 0;
  volatile uint8_t _lockouts = 0;   // lockouts since the last success (fixed code only)
};

extern WebAuthCode g_web_auth;
