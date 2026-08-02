// SPDX-License-Identifier: GPL-3.0-or-later

#include "AttakySharedI2C.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_i2c_mtx = nullptr;
static bool              s_inited  = false;

void attakyI2cInit() {
  if (s_inited) return;
  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL, 400000);
#if defined(ESP32)
  Wire.setTimeOut(20);
#endif
  s_i2c_mtx = xSemaphoreCreateMutex();
  s_inited  = true;
}

bool attakyI2cLock(uint32_t timeout_ms) {
  if (!s_i2c_mtx) return true;
  return xSemaphoreTake(s_i2c_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void attakyI2cUnlock() {
  if (s_i2c_mtx) xSemaphoreGive(s_i2c_mtx);
}
