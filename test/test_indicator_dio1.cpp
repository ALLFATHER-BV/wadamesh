// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the SenseCAP Indicator DIO1 edge detector (IndicatorDio1.h): the
// virtual-pin routing and the rule that every read of expander port 0 -- the
// dispatch task's and the BUSY polls' alike -- fires the radio callback exactly
// once per DIO1 rise.
//
//   c++ -std=c++17 -Wall -Wextra -I src -I variants/sensecap_indicator test/test_indicator_dio1.cpp -o /tmp/ind_dio1 && /tmp/ind_dio1

#include <assert.h>
#include <stdio.h>

#include "IndicatorDio1.h"

using namespace indicator;

static int s_calls = 0;
static void onDio1() { ++s_calls; }

static void virtual_pin_routing() {
  static_assert(kExpanderPinFlag == 0x40, "virtual pin flag");
  assert(isExpanderPin(0x43));
  assert(isExpanderPin(0x40));
  assert(!isExpanderPin(41));
  assert(!isExpanderPin(0));
  assert(expanderBit(0x43) == 3);
  assert(expanderBit(0x40) == pins::kLoraNss);
}

static void rising_edge_fires_once() {
  s_calls = 0;
  Dio1Edge edge;
  edge.arm(onDio1);
  assert(edge.armed());
  assert(!edge.onPort0(0x00));
  assert(edge.onPort0(0x08));
  assert(s_calls == 1);
  assert(!edge.onPort0(0x08));
  assert(s_calls == 1);
}

static void edge_seen_in_busy_read_fires_once() {
  s_calls = 0;
  Dio1Edge edge;
  edge.arm(onDio1);
  assert(!edge.onPort0(0x04));   // BUSY high, DIO1 low
  assert(edge.onPort0(0x08));    // the HAL's BUSY poll sees DIO1 rise
  assert(!edge.onPort0(0x08));   // the dispatch task's read that follows
  assert(s_calls == 1);
}

static void level_high_without_edge_does_not_refire() {
  s_calls = 0;
  Dio1Edge edge;
  edge.arm(onDio1);
  assert(edge.onPort0(0x08));
  assert(!edge.onPort0(0x48));   // touch INT changed, DIO1 still high
  assert(!edge.onPort0(0x4C));   // BUSY toggles too
  assert(s_calls == 1);
}

static void no_callback_while_disarmed() {
  s_calls = 0;
  Dio1Edge edge;
  edge.disarm();
  assert(!edge.armed());
  assert(!edge.onPort0(0x00));
  assert(!edge.onPort0(0x08));
  assert(s_calls == 0);

  edge.arm(onDio1);              // DIO1 is high now: no phantom edge on re-arm
  assert(!edge.onPort0(0x08));
  assert(s_calls == 0);

  assert(!edge.onPort0(0x00));
  assert(edge.onPort0(0x08));
  assert(s_calls == 1);
}

int main() {
  virtual_pin_routing();
  rising_edge_fires_once();
  edge_seen_in_busy_read_fires_once();
  level_high_without_edge_does_not_refire();
  no_callback_while_disarmed();
  printf("all indicator dio1 tests passed\n");
  return 0;
}
