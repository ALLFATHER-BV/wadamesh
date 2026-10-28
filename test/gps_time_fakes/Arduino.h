// SPDX-License-Identifier: GPL-3.0-or-later
// Just enough Arduino for src/helpers/WadaNmeaLocationProvider.h in the clock-sync host test
// (test/test_gps_time_sync.cpp): a virtual clock, pins, and a UART fed by a simulated GPS that
// sends one RMC a second carrying the TRUE current UTC time, while it is powered and awake.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#define HIGH 1
#define LOW 0
#define OUTPUT 1

extern uint32_t g_test_ms;
extern uint32_t g_utc_at_ms0;   // true UTC (Unix seconds) when g_test_ms was 0
inline uint32_t millis() { return g_test_ms; }
inline void delay(uint32_t ms) { g_test_ms += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
inline uint32_t trueUtc() { return g_utc_at_ms0 + g_test_ms / 1000; }

class Stream {
public:
  virtual ~Stream() {}
  virtual int available() = 0;
  virtual int read() = 0;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* b, size_t n) { for (size_t i = 0; i < n; ++i) write(b[i]); return n; }
  virtual void flush() {}
  size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
  size_t print(char c) { return write((uint8_t)c); }
};

// The receiver. While `talking` (powered, past its wake-up delay) it emits a valid RMC at
// the top of every second with that second's real UTC date and time.
class HardwareSerial : public Stream {
public:
  bool talking = false;
  uint32_t quiet_until_ms = 0;   // after a wake: nothing on the wire until then
  std::string rx;
  uint32_t last_sec = 0xFFFFFFFFu;
  void wake(uint32_t delay_ms) { talking = true; quiet_until_ms = millis() + delay_ms; }
  void sleep() { talking = false; rx.clear(); }
  void pump() {
    if (!talking || (int32_t)(millis() - quiet_until_ms) < 0) return;
    const uint32_t sec = millis() / 1000;
    if (sec == last_sec) return;
    last_sec = sec;
    const uint32_t t = trueUtc();
    const long days = (long)(t / 86400);
    const uint32_t sod = t % 86400;
    // civil-from-days
    const long z = days + 719468; const long era = z / 146097; const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const unsigned y = (unsigned)(yoe + era * 400) + (m <= 2);
    char body[96];
    snprintf(body, sizeof body, "GPRMC,%02u%02u%02u.00,A,4807.038,N,01131.000,E,000.0,000.0,%02u%02u%02u,,,A",
             (unsigned)(sod / 3600), (unsigned)(sod / 60 % 60), (unsigned)(sod % 60), d, m, y % 100);
    uint8_t x = 0;
    for (const char* p = body; *p; ++p) x ^= (uint8_t)*p;
    char line[112];
    snprintf(line, sizeof line, "$%s*%02X\r\n", body, x);
    rx += line;
  }
  int available() override { pump(); return (int)rx.size(); }
  int read() override {
    pump();
    if (rx.empty()) return -1;
    const uint8_t c = (uint8_t)rx[0];
    rx.erase(0, 1);
    return c;
  }
  size_t write(uint8_t) override { return 1; }
  void begin(uint32_t) {}
  void end() {}
  void setPins(int, int) {}
};
