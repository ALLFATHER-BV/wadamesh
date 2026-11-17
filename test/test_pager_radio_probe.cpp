// SPDX-License-Identifier: GPL-3.0-or-later
// Host test: g++ -std=c++17 -I variants/lilygo_tlora_pager test/test_pager_radio_probe.cpp -o pager_probe_test
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "PagerRadioProbe.h"

struct FakeProbe {
  uint32_t time = 0;
  uint32_t busyUntil = 0;
  uint32_t busyFor = 0;
  unsigned resets = 0, sxReads = 0, lrReads = 0;
  bool sxOk = true, lrOk = true;
  char sx[2][6] = {};
  uint8_t lr[2][4] = {};
  void reset() { ++resets; busyUntil = time + busyFor; }
  uint32_t now() const { return time; }
  void pause(uint32_t ms) { time += ms; }
  bool ready() const { return int32_t(time - busyUntil) >= 0; }
  bool readSX126Version(char* out, size_t n) {
    const unsigned i = sxReads++;
    if (!sxOk || i >= 2 || n != 6) return false;
    memcpy(out, sx[i], n);
    return true;
  }
  bool readLR1121Version(uint8_t* out, size_t n) {
    const unsigned i = lrReads++;
    if (!lrOk || i >= 2 || n != 4) return false;
    memcpy(out, lr[i], n);
    return true;
  }
  void goodSx() { memcpy(sx[0], "SX1261", 6); memcpy(sx[1], sx[0], 6); }
  void goodLr() { lr[0][0] = 1; lr[0][1] = 3; lr[0][2] = 4; lr[0][3] = 1; memcpy(lr[1], lr[0], 4); }
};

int main() {
  {
    FakeProbe p; p.goodSx(); p.goodLr(); p.busyFor = 3;
    assert(probePagerRadio(p) == PagerRadioFamily::SX1262);
    assert(p.resets == 1 && p.sxReads == 2 && p.lrReads == 0 && p.time == 3);
  }
  {
    FakeProbe p; p.goodLr(); p.busyFor = 2;
    assert(probePagerRadio(p) == PagerRadioFamily::LR1121);
    assert(p.resets == 2 && p.sxReads == 1 && p.lrReads == 2 && p.time == 4);
  }
  {
    FakeProbe p; p.goodSx(); p.sx[1][5] = '2';
    assert(probePagerRadio(p) == PagerRadioFamily::None);
    assert(p.resets == 2 && p.sxReads == 2);
  }
  {
    FakeProbe p; p.goodLr(); p.lr[1][3]++;
    assert(probePagerRadio(p) == PagerRadioFamily::None);
  }
  for (uint8_t other : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(0xdf), uint8_t(0xff)}) {
    FakeProbe p; p.goodLr(); p.lr[0][1] = p.lr[1][1] = other;
    assert(probePagerRadio(p) == PagerRadioFamily::None);
  }
  {
    FakeProbe p; p.goodSx(); p.busyFor = 251;
    assert(probePagerRadio(p) == PagerRadioFamily::None);
    assert(p.resets == 1 && p.sxReads == 0 && p.time == 250);
  }
  {
    FakeProbe p; p.goodLr(); p.lrOk = false;
    assert(probePagerRadio(p) == PagerRadioFamily::None);
  }
  {
    FakeProbe p; p.goodSx(); p.time = UINT32_MAX - 2; p.busyFor = 4;
    assert(probePagerRadio(p) == PagerRadioFamily::SX1262);
  }
  std::puts("test_pager_radio_probe: OK");
}
