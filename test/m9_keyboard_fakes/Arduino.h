// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
struct TestSerial {
  std::string output;
  void println(const char* text) { output += text; output += '\n'; }
  template<typename... Args> void printf(const char* format, Args... args) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    output += buffer;
  }
};
extern TestSerial Serial;
