// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for the bits of Arduino.h that variants/thinknode_m9/M9Keyboard.cpp uses.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

inline uint32_t g_fake_ms = 0;
inline uint32_t millis() { return g_fake_ms; }

// Serial.printf output is captured so a test can check the boot log line.
struct FakeSerial {
  char log[2048] = {0};
  void printf(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    strncat(log, line, sizeof log - strlen(log) - 1);
  }
  void println(const char* s) { printf("%s\n", s); }
};
inline FakeSerial Serial;
