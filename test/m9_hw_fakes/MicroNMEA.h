// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <climits>
#include <cstdint>
#include <cstring>
#include "Arduino.h"

class MicroNMEA {
  char* _buf;
  uint8_t _len;
  char* _ptr;
  bool _valid = false;
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
      if (*_buf == '$' && testChecksum(_buf) && !strncmp(_buf + 3, "RMC", 3)) _valid = _buf[14] == 'A';
      return *_buf != '\0';
    }
    *_ptr = c;
    if (_ptr < &_buf[_len - 1]) ++_ptr;
    return false;
  }
  const char* getSentence() const { return _buf; }
  void clear() { _valid = false; }
  bool isValid() const { return _valid; }
  long getLatitude() const { return 48117300; }
  long getLongitude() const { return 11516667; }
  bool getAltitude(long& a) const { a = 545400; return true; }
  uint8_t getNumSatellites() const { return 8; }
  uint16_t getYear() const { return 0; }
  uint8_t getMonth() const { return 0; }
  uint8_t getDay() const { return 0; }
  uint8_t getHour() const { return 0; }
  uint8_t getMinute() const { return 0; }
  uint8_t getSecond() const { return 0; }
  long getSpeed() const { return LONG_MIN; }
  long getCourse() const { return LONG_MIN; }
  Stream& sendSentence(Stream& s, const char* sentence) { s.print(sentence); return s; }
};
