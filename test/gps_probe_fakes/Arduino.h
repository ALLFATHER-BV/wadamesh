// SPDX-License-Identifier: GPL-3.0-or-later
// Just enough Arduino for src/helpers/WadaNmeaLocationProvider.h on the host
// (test/test_gps_baud_probe.cpp): a virtual clock, pins, and a UART whose bytes
// come from a simulated receiver that talks at one speed.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#define HIGH 1
#define LOW 0
#define OUTPUT 1

extern uint32_t g_test_ms;
inline uint32_t millis() { return g_test_ms; }
inline void delay(uint32_t ms) { g_test_ms += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }

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

// The receiver: emits NMEA at `true_baud`. A UART opened at another speed reads
// what a mis-clocked UART reads from it: noise (deterministic pseudo-random bytes).
// Bytes arrive in real time for the speed the receiver talks at.
class HardwareSerial : public Stream {
public:
  uint32_t true_baud = 0;          // 0: no receiver on the line
  uint32_t baud = 0;               // what begin() opened the UART at
  bool open = false;
  int begins = 0;                  // UART (re)opens
  std::vector<uint32_t> begun_at;  // every speed it was opened at, in order
  std::string sent;                // what the ESP wrote to the receiver
  std::vector<uint8_t> rx;         // the RX ring
  size_t rx_head = 0;
  uint32_t last_ms = 0;
  double carry = 0;
  uint32_t noise = 0x2545F491u;
  size_t sentence_pos = 0;
  int sentence_idx = 0;

  void begin(uint32_t b) { baud = b; open = true; ++begins; begun_at.push_back(b); rx.clear(); rx_head = 0; last_ms = millis(); carry = 0; }
  void end() { open = false; }
  void setPins(int, int) {}
  // Fill the ring for the time that passed since the last look.
  void pump() {
    const uint32_t now = millis();
    if (!open || !true_baud) { last_ms = now; return; }
    carry += (double)(now - last_ms) * (double)true_baud / 10000.0;   // 10 bits a byte
    last_ms = now;
    while (carry >= 1.0) {
      carry -= 1.0;
      rx.push_back(baud == true_baud ? nextNmea() : nextNoise());
    }
  }
  uint8_t nextNoise() {
    noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
    return (uint8_t)noise;
  }
  uint8_t nextNmea() {
    static const char* const k[] = {
      "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n",
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n",
      "$GPGSV,1,1,00*79\r\n",
    };
    const char* s = k[sentence_idx];
    const uint8_t c = (uint8_t)s[sentence_pos++];
    if (!s[sentence_pos]) { sentence_pos = 0; sentence_idx = (sentence_idx + 1) % 3; }
    return c;
  }
  int available() override { pump(); return (int)(rx.size() - rx_head); }
  int read() override {
    pump();
    if (rx_head >= rx.size()) return -1;
    const uint8_t c = rx[rx_head++];
    if (rx_head > 8192) { rx.erase(rx.begin(), rx.begin() + (long)rx_head); rx_head = 0; }
    return c;
  }
  size_t write(uint8_t b) override { sent.push_back((char)b); return 1; }
};
