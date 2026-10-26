#pragma once
// Host stand-in for the core's AutoDiscoverRTCClock (test/test_time_zones.cpp): a battery-backed
// RTC chip whose reading the test sets directly, so ClockFloorRTC's floor logic runs unmodified.
#include <stdint.h>

namespace mesh {
class RTCClock {
public:
  virtual ~RTCClock() {}
  virtual uint32_t getCurrentTime() = 0;
  virtual void setCurrentTime(uint32_t time) = 0;
};
}  // namespace mesh

struct FakeRtcChip {
  static uint32_t& now() { static uint32_t t = 0; return t; }       // what the chip reads
  static uint32_t& writes() { static uint32_t n = 0; return n; }    // how often it was set
};

class AutoDiscoverRTCClock : public mesh::RTCClock {
public:
  explicit AutoDiscoverRTCClock(mesh::RTCClock&) {}
  uint32_t getCurrentTime() override { return FakeRtcChip::now(); }
  void setCurrentTime(uint32_t t) override { FakeRtcChip::now() = t; FakeRtcChip::writes()++; }
};
