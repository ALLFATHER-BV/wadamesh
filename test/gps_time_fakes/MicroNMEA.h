// SPDX-License-Identifier: GPL-3.0-or-later
// The parts of MicroNMEA (stevemarple/MicroNMEA, LGPL) the provider uses, for the clock-sync
// host test (test/test_gps_time_sync.cpp). Unlike test/gps_probe_fakes this one decodes the
// RMC time of day, status and date, and clear() resets them the way the library does
// (year/month/day 0, hour/minute/second 99, not valid), so the provider's clock path runs
// on what the receiver actually said.
#pragma once
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include "Arduino.h"

class MicroNMEA {
  char* _buf; uint8_t _len; char* _ptr;
  bool _valid = false;
  uint16_t _year = 0; uint8_t _month = 0, _day = 0, _hour = 99, _minute = 99, _second = 99;
  static int two(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }
  // Field n (0 = the talker+type) of the terminated sentence, or nullptr.
  const char* field(int n) const {
    const char* p = _buf;
    for (int i = 0; i < n; ++i) { p = strchr(p, ','); if (!p) return nullptr; ++p; }
    return p;
  }
public:
  MicroNMEA(void* buf, uint8_t len) : _buf((char*)buf), _len(len), _ptr((char*)buf) { _buf[0] = 0; }
  static bool testChecksum(const char* s) {
    if (*s != '$') return false;
    uint8_t x = 0;
    const char* p = s + 1;
    while (*p && *p != '*') x ^= (uint8_t)*p++;
    if (*p != '*') return false;
    static const char hex[] = "0123456789ABCDEF";
    return p[1] == hex[x >> 4] && p[2] == hex[x & 0xF];
  }
  bool process(char c) {
    if (c == '\0' || c == '\n' || c == '\r') {
      *_ptr = '\0';
      _ptr = _buf;
      if (*_buf == '$' && testChecksum(_buf) && !strncmp(_buf + 3, "RMC", 3)) {
        const char* t = field(1); const char* st = field(2); const char* d = field(9);
        if (t && t[0] != ',') { _hour = two(t); _minute = two(t + 2); _second = two(t + 4); }
        if (d && d[0] != ',') { _day = two(d); _month = two(d + 2); _year = 2000 + two(d + 4); }
        _valid = st && st[0] == 'A';
      }
      return *_buf != '\0';
    }
    *_ptr = c;
    if (_ptr < &_buf[_len - 1]) ++_ptr;
    return false;
  }
  const char* getSentence() const { return _buf; }
  void clear() { _valid = false; _year = _month = _day = 0; _hour = _minute = _second = 99; }
  bool isValid() const { return _valid; }
  long getLatitude() const { return 48117300; }
  long getLongitude() const { return 11516667; }
  bool getAltitude(long& a) const { a = 545400; return true; }
  uint8_t getNumSatellites() const { return 8; }
  uint16_t getYear() const { return _year; }
  uint8_t getMonth() const { return _month; }
  uint8_t getDay() const { return _day; }
  uint8_t getHour() const { return _hour; }
  uint8_t getMinute() const { return _minute; }
  uint8_t getSecond() const { return _second; }
  long getSpeed() const { return LONG_MIN; }
  long getCourse() const { return LONG_MIN; }
  Stream& sendSentence(Stream& s, const char* sentence) { s.print(sentence); return s; }
};
