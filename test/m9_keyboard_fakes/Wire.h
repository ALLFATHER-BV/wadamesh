// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

// Model the M9's addressed registers and destructive, single-key reads.
class TwoWire {
 public:
  uint8_t device = 0;
  bool online = true;
  bool readFails = false;
  int sda = -1, scl = -1;
  uint8_t backlight = 0;
  std::deque<uint8_t> keys;
  std::vector<uint8_t> writes;
  uint8_t address = 0, reg = 0;
  int result = 0;
  bool begin(int data, int clock) { sda = data; scl = clock; return true; }
  void beginTransmission(uint8_t addr) { address = addr; writes.clear(); }
  void write(uint8_t value) { writes.push_back(value); }
  uint8_t endTransmission(bool = true) {
    if (!online || !device || address != device) return 2;
    if (!writes.empty()) reg = writes[0];
    if (writes.size() == 2 && reg == 2) backlight = writes[1];
    return 0;
  }
  int requestFrom(int addr, int count) {
    if (!online || !device || addr != device || count != 1 || readFails) return 0;
    result = reg == 0 ? 3 : reg == 0xfe ? 0x10 : 0;
    if (reg == 1 && !keys.empty()) { result = keys.front(); keys.pop_front(); }
    return 1;
  }
  int read() { return result; }
};
extern TwoWire Wire, Wire1;
