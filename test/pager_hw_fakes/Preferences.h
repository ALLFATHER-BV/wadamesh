// SPDX-License-Identifier: GPL-3.0-or-later
// Fake NVS: one process-wide store, so a "reboot" (a new board object) sees what
// the last one wrote.
#pragma once
#include <map>
#include <string>
#include <cstdint>
inline std::map<std::string, uint16_t>& testNvs() { static std::map<std::string, uint16_t> m; return m; }
class Preferences {
  std::string ns_;
public:
  bool begin(const char* ns, bool) { ns_ = ns; return true; }
  void end() {}
  uint16_t getUShort(const char* k, uint16_t d) {
    auto it = testNvs().find(ns_ + "/" + k); return it == testNvs().end() ? d : it->second;
  }
  size_t putUShort(const char* k, uint16_t v) { testNvs()[ns_ + "/" + k] = v; return 2; }
};
