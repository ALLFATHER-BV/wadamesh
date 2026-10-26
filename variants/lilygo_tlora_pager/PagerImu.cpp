// SPDX-License-Identifier: GPL-3.0-or-later
#include "PagerImu.h"

#if defined(TLORA_PAGER) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include <SensorBHI260AP.hpp>
#include <bosch/bhy2_parse.h>

// The firmware image LilyGoLib loads on this board (Klio: the plain sensor set
// plus Bosch's gesture learning). Including it defines the image arrays, so it
// belongs in exactly one translation unit: this one.
#define BOSCH_BHI260_KLIO
#include <BoschFirmware.h>

#define PAGER_IMU_ADDR      0x28
#define PAGER_IMU_RATE_HZ   25.0f
#define PAGER_IMU_IDLE_MS   3000UL   // stop sampling this long after the last read
#define PAGER_IMU_FRESH_MS  1000UL   // a sample older than this is not handed out

static SensorBHI260AP s_bhy;
static bool     s_tried = false;     // firmware upload attempted this boot
static bool     s_ready = false;     // firmware running
static bool     s_sampling = false;  // accelerometer configured on
static uint32_t s_last_read_ms = 0;
static uint32_t s_sample_ms = 0;
static float    s_x = 0, s_y = 0, s_z = 0;

static void accelCb(uint8_t sensor_id, uint8_t* data, uint32_t len, uint64_t* ts, void* user) {
  (void)len; (void)ts; (void)user;
  struct bhy2_data_xyz v;
  bhy2_parse_xyz(data, &v);
  const float k = s_bhy.getScaling(sensor_id);   // 1/4096 g per count
  s_x = v.x * k;
  s_y = v.y * k;
  s_z = v.z * k;
  s_sample_ms = millis();
}

static bool imuStart() {
  if (s_tried) return s_ready;
  s_tried = true;
  // The upload is ~120 KB over the shared bus: run it at 1 MHz as LilyGoLib does
  // (there at boot), then put the bus back. It blocks the loop for 1-2 s, once,
  // on the first wada.sys.accel() call. The only other Wire user, the notify
  // task (ES8311/XL9555 around a chime), could hit that window; worst case one
  // chime's codec writes fail.
  const uint32_t was = Wire.getClock();
  Wire.setClock(1000000UL);
  s_bhy.setFirmware(bosch_firmware_image, bosch_firmware_size, bosch_firmware_type);
  s_bhy.setBootFromFlash(false);
  const uint32_t t0 = millis();
  s_ready = s_bhy.begin(Wire, PAGER_IMU_ADDR);
  Wire.setClock(was ? was : 100000UL);
  if (s_ready) {
    s_bhy.setRemapAxes(SensorBHI260AP::BOTTOM_LAYER_TOP_LEFT_CORNER);
    s_ready = s_bhy.onResultEvent(SensorBHI260AP::ACCEL_PASSTHROUGH, accelCb);
  }
  Serial.printf("[IMU] bhi260ap %s (%lu ms)\n", s_ready ? "ok" : "FAILED",
                (unsigned long)(millis() - t0));
  return s_ready;
}

bool pagerImuRead(float* x_g, float* y_g, float* z_g) {
  if (!imuStart()) return false;
  const uint32_t now = millis();
  s_last_read_ms = now;
  if (!s_sampling) {
    s_sampling = s_bhy.configure(SensorBHI260AP::ACCEL_PASSTHROUGH, PAGER_IMU_RATE_HZ, 0);
    if (!s_sampling) return false;
  }
  s_bhy.update();                                  // drain the FIFO -> accelCb
  // accelCb stamps with millis() during update(), i.e. after now: compare
  // against a fresh clock, or a sample from this very call reads as stale.
  if (!s_sample_ms || millis() - s_sample_ms > PAGER_IMU_FRESH_MS) return false;
  *x_g = s_x; *y_g = s_y; *z_g = s_z;
  return true;
}

bool pagerImuStarted() { return s_tried; }

void pagerImuIdleTick() {
  if (!s_sampling) return;
  if (millis() - s_last_read_ms < PAGER_IMU_IDLE_MS) return;
  s_bhy.configure(SensorBHI260AP::ACCEL_PASSTHROUGH, 0, 0);   // rate 0 = off
  s_bhy.update();
  s_sampling = false;
  s_sample_ms = 0;
}

#endif  // TLORA_PAGER && ESP32
