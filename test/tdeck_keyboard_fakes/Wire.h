// SPDX-License-Identifier: GPL-3.0-or-later
// The T-Deck keyboard's C3 on I2C 0x55, scripted: a five-byte read returns the next
// queued frame (zeros when none: idle), a one-byte read the next queued legacy byte.
// Commands written (0x03 raw frame request, 0x04 back to the key mode) are recorded.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>
#include <array>

class TwoWire {
 public:
  std::deque<std::array<uint8_t, 5>> frames;
  std::deque<uint8_t> legacy;
  std::vector<uint8_t> commands;
  std::vector<uint8_t> pending, out;
  size_t out_pos = 0;
  void beginTransmission(uint8_t) { pending.clear(); }
  void write(uint8_t v) { pending.push_back(v); }
  uint8_t endTransmission(bool = true) {
    if (pending.size() == 1) commands.push_back(pending[0]);
    return 0;
  }
  int requestFrom(int, int count) {
    out.clear(); out_pos = 0;
    if (count == 5) {
      std::array<uint8_t, 5> f{};
      if (!frames.empty()) { f = frames.front(); frames.pop_front(); }
      out.assign(f.begin(), f.end());
    } else if (count == 1) {
      uint8_t k = 0;
      if (!legacy.empty()) { k = legacy.front(); legacy.pop_front(); }
      out.push_back(k);
    }
    return (int)out.size();
  }
  int available() { return (int)(out.size() - out_pos); }
  int read() { return out_pos < out.size() ? out[out_pos++] : -1; }
};
extern TwoWire Wire;
