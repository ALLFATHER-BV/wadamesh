// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

class LatchedModifier {
 public:
  static constexpr uint32_t DOUBLE_TAP_MS = 400;

  void press() {
    held_ = true;
    used_while_held_ = false;
  }

  void release(uint32_t now_ms) {
    held_ = false;
    if (discard_release_) {
      discard_release_ = false;
      used_while_held_ = false;
      return;
    }
    if (used_while_held_) return;
    tap(now_ms);
  }

  void tap(uint32_t now_ms) {
    if (locked_) {
      clearLatched();
      return;
    }
    if (one_shot_ && (uint32_t)(now_ms - last_tap_ms_) <= DOUBLE_TAP_MS) {
      one_shot_ = false;
      locked_ = true;
      last_tap_ms_ = 0;
      return;
    }
    one_shot_ = true;
    last_tap_ms_ = now_ms;
  }

  bool consumeForKey() {
    const bool active_now = held_ || one_shot_ || locked_;
    if (held_) used_while_held_ = true;
    if (one_shot_) {
      one_shot_ = false;
      last_tap_ms_ = 0;
    }
    return active_now;
  }

  void markHeldUsed() {
    if (!held_) return;
    used_while_held_ = true;
    one_shot_ = false;
    last_tap_ms_ = 0;
  }

  void clearLatched() {
    one_shot_ = false;
    locked_ = false;
    last_tap_ms_ = 0;
  }

  void discard() {
    clearLatched();
    if (held_) {
      used_while_held_ = true;
      discard_release_ = true;
    }
  }

  void baseline(bool physically_held) {
    clearLatched();
    held_ = physically_held;
    used_while_held_ = physically_held;
    discard_release_ = physically_held;
  }

  bool held() const { return held_; }
  bool latched() const { return one_shot_ || locked_; }
  bool active() const { return held_ || one_shot_ || locked_; }
  bool locked() const { return locked_; }

 private:
  bool held_ = false;
  bool used_while_held_ = false;
  bool one_shot_ = false;
  bool locked_ = false;
  bool discard_release_ = false;
  uint32_t last_tap_ms_ = 0;
};