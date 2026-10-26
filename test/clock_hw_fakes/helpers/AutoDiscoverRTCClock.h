// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for the core's AutoDiscoverRTCClock (test/test_clock_floor.cpp): a
// hardware clock the test sets to any reading, including stopped and garbage ones.
#pragma once
#include <stdint.h>

namespace mesh {
class RTCClock {
public:
  virtual ~RTCClock() {}
  virtual uint32_t getCurrentTime() = 0;
  virtual void setCurrentTime(uint32_t time) = 0;
  virtual void tick() {}
};
}

class AutoDiscoverRTCClock : public mesh::RTCClock {
public:
  uint32_t hw = 0;             // what the chip reads
  bool hw_write_takes = true;  // false: a write that does not take (PCF8563 trouble)
  int writes = 0;
  explicit AutoDiscoverRTCClock(mesh::RTCClock&) {}
  uint32_t getCurrentTime() override { return hw; }
  void setCurrentTime(uint32_t t) override { ++writes; if (hw_write_takes) hw = t; }
};
