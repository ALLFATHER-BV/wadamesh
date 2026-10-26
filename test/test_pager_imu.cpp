// SPDX-License-Identifier: GPL-3.0-or-later
// The T-LoRa Pager's accelerometer glue (PagerImu.cpp) on a fake BHI260AP: the hub
// firmware is uploaded once per boot, on the first read, at 1 MHz with the shared
// bus clock put back afterwards (and once only even when it fails); samples are
// scaled to g and only handed out while fresh; the accelerometer stops 3 s after
// the last read and starts again on the next one.
//
// Build: g++ -std=c++17 -I test/pager_hw_fakes -I src test/test_pager_imu.cpp
#include <cassert>
#include <cstdio>
#include <cmath>
#include "pager_hw_fakes/Arduino.h"
#include "pager_hw_fakes/Wire.h"
#include "pager_hw_fakes/SensorBHI260AP.hpp"
#define TLORA_PAGER 1
#define ESP32 1
uint32_t testMillis = 1000;
TestSerial Serial;
TwoWire Wire;
FakeBhy g_bhy;
#include "../variants/lilygo_tlora_pager/PagerImu.cpp"

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
  float x, y, z;
  Wire.clock = 400000;
  assert(!pagerImuStarted());
  // First read: the upload happens at 1 MHz, the bus goes back to 400 kHz.
  assert(!pagerImuRead(&x, &y, &z));         // nothing in the FIFO yet
  assert(pagerImuStarted() && g_bhy.begins == 1);
  assert(g_bhy.upload_clock == 1000000 && Wire.clock == 400000);
  assert(g_bhy.configures == 1 && near(g_bhy.rate, 25.0f));
  // A sample: counts / 4096 = g. Face up, lying flat: z = +1 g.
  g_bhy.fifo.push_back({ 0, 0, 4096 });
  testMillis += 40;
  assert(pagerImuRead(&x, &y, &z) && near(x, 0) && near(y, 0) && near(z, 1.0f));
  g_bhy.fifo.push_back({ 2048, -4096, -2048 });
  testMillis += 40;
  assert(pagerImuRead(&x, &y, &z) && near(x, 0.5f) && near(y, -1.0f) && near(z, -0.5f));
  // No new sample for over a second: stale, not handed out.
  testMillis += 1100;
  assert(!pagerImuRead(&x, &y, &z));
  // Idle: still sampling 2.9 s after the last read, stopped at 3 s, no repeat stop.
  testMillis += 2900;
  pagerImuIdleTick();
  assert(near(g_bhy.rate, 25.0f));
  testMillis += 200;
  pagerImuIdleTick();
  assert(near(g_bhy.rate, 0.0f) && g_bhy.configures == 2);
  pagerImuIdleTick();
  assert(g_bhy.configures == 2);
  // The next read restarts it, without a second upload; the stop cleared the old sample.
  assert(!pagerImuRead(&x, &y, &z));
  assert(g_bhy.begins == 1 && g_bhy.configures == 3 && near(g_bhy.rate, 25.0f));
  g_bhy.fifo.push_back({ 0, 4096, 0 });
  testMillis += 40;
  assert(pagerImuRead(&x, &y, &z) && near(y, 1.0f));

  // A failed upload is not retried on every read (it would stall the loop each time).
  s_tried = s_ready = s_sampling = false; s_sample_ms = 0;
  g_bhy = FakeBhy{}; g_bhy.upload_ok = false;
  Wire.clock = 100000;
  for (int i = 0; i < 5; ++i) assert(!pagerImuRead(&x, &y, &z));
  assert(g_bhy.begins == 1 && Wire.clock == 100000 && g_bhy.configures == 0);
  std::printf("test_pager_imu: OK\n");
  return 0;
}
