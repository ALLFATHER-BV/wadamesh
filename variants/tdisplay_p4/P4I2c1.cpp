// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_TDISPLAY_P4)
#include "P4I2c1.h"
#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// LilyGo t_display_p4_config.h: keyboard expansion on 46/45, ES8311 on 20/21.
#define P4_I2C1_KB_SDA     46
#define P4_I2C1_KB_SCL     45
#define P4_I2C1_KB_HZ      100000UL
#define P4_I2C1_AUDIO_SDA  20
#define P4_I2C1_AUDIO_SCL  21
#define P4_I2C1_AUDIO_HZ   400000UL

static SemaphoreHandle_t s_mtx = nullptr;
static portMUX_TYPE      s_create_mux = portMUX_INITIALIZER_UNLOCKED;
static int               s_route = -1;   // none: nothing has begun Wire1 yet
static bool              s_fail_logged[2] = {};

TwoWire* p4I2c1Acquire(P4I2c1Route route, uint32_t wait_ms) {
  if (!s_mtx) {
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&s_create_mux);
    if (!s_mtx) { s_mtx = m; m = nullptr; }
    portEXIT_CRITICAL(&s_create_mux);
    if (m) vSemaphoreDelete(m);
    if (!s_mtx) return nullptr;
  }
  const TickType_t ticks = wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
  if (xSemaphoreTake(s_mtx, ticks) != pdTRUE) return nullptr;

  // Re-begun when it is on the other route, and also when it is not running at
  // all: a bus that is only remembered as begun fails every transaction after.
  if (s_route != (int)route || !i2cIsInit(1)) {
    if (i2cIsInit(1)) Wire1.end();
    const bool audio = (route == P4_I2C1_AUDIO);
    const bool ok = audio ? Wire1.begin(P4_I2C1_AUDIO_SDA, P4_I2C1_AUDIO_SCL, P4_I2C1_AUDIO_HZ)
                          : Wire1.begin(P4_I2C1_KB_SDA, P4_I2C1_KB_SCL, P4_I2C1_KB_HZ);
    // Not recorded as current when it failed, so the next call tries again.
    s_route = ok ? (int)route : -1;
    if (!ok) {
      if (!s_fail_logged[audio ? 1 : 0]) {
        s_fail_logged[audio ? 1 : 0] = true;
        printf("[P4I2C1] Wire1.begin for the %s route failed\n", audio ? "audio" : "keyboard");
      }
      xSemaphoreGive(s_mtx);
      return nullptr;
    }
  }
  return &Wire1;
}

void p4I2c1Release() {
  if (s_mtx) xSemaphoreGive(s_mtx);
}

#endif
