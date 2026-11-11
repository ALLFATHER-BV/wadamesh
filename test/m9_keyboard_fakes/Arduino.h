// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
#ifndef OUTPUT
#define OUTPUT 0x03
#endif
#ifndef INPUT
#define INPUT 0x01
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef HIGH
#define HIGH 1
#endif
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
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
