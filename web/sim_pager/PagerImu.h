// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
// T-LoRa Pager simulator: lying flat on a desk.
//   WADA_SIM_FACEDOWN=1   face down
//   WADA_SIM_PICKUP=MS    flat until MS after start, lifted for 0.6 s, then held tilted (reading)
//   WADA_SIM_BAG=1        carried in a bag: never still
static inline bool pagerImuRead(float* x, float* y, float* z) {
  static const bool down = getenv("WADA_SIM_FACEDOWN") != nullptr;
  static const bool bag = getenv("WADA_SIM_BAG") != nullptr;
  static const char* pk = getenv("WADA_SIM_PICKUP");
  const uint32_t t = millis();
  *x = 0; *y = 0; *z = down ? -1.0f : 1.0f;
  if (bag) {
    const float a = (float)t / 1000.0f;
    *x = 0.3f * sinf(a * 7.0f); *y = 0.9f + 0.15f * sinf(a * 5.0f); *z = 0.3f * cosf(a * 3.0f);
  } else if (pk) {
    const uint32_t at = (uint32_t)atol(pk);
    if (t >= at && t < at + 600) { *y = 0.3f + 0.4f * sinf((float)(t - at) / 60.0f); *z = 0.8f; }
    else if (t >= at + 600)      { *y = 0.6f; *z = 0.8f; }
  }
  return true;
}
static inline void pagerImuIdleTick() {}
static inline bool pagerImuStarted() { static bool once = false; const bool was = once; once = true; return was; }
