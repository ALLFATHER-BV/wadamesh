// SPDX-License-Identifier: GPL-3.0-or-later
// RAK WisMesh Tap V2 touch input — implements the shared touch-input API
// for the Tap V2's FT5x06 capacitive controller (I2C addr 0x38).
//
// The FT5x06 shares the main I2C bus (Wire, SDA=9 / SCL=40) with any
// other I2C sensors on the RAK3312 board. The bus is already initialised
// by ESP32Board::begin() — this driver must NOT re-init or take ownership.
//
// CRASH NOTE (v6→v7): Wire1.begin(9,40) causes panic because ESP32-S3's
// I2C1 controller and the already-running I2C0 controller fight over the
// same GPIO pins. The fix is to use Wire (I2C0) on the shared bus.
//
// Protocol reference: LovyanGFX Touch_FT5x06.cpp (FreeBSD license)

#if defined(HAS_RAK_TAP_V2) && defined(ESP32)

#include "HeltecV4CapTouch.h"
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>

#ifndef PIN_TOUCH_SDA
  #define PIN_TOUCH_SDA 9
#endif
#ifndef PIN_TOUCH_SCL
  #define PIN_TOUCH_SCL 40
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 39
#endif

// Output (landscape LVGL) screen dimensions — used for clamping after rotation.
static const int SCR_W = 320;
static const int SCR_H = 240;

// FT5x06 native report resolution — the chip sees the physical 240×320
// panel BEFORE any ST7789 display rotation. Rotation formulas use these
// raw-portrait dimensions; after the transform, the result is clamped to
// the landscape output above. Mirrors HeltecV4CapTouch applyPointRotation
// which uses W=240/H=320 as the pre-rotation baseline.
static const int RAW_W = 240;
static const int RAW_H = 320;

static const uint8_t FT5x06_ADDR          = 0x38;
static const uint8_t FT5x06_REG_DEVMODE   = 0x00;
static const uint8_t FT5x06_REG_TDSTATUS  = 0x02;

static bool     s_init_ok = false;
static bool     s_given_up = false;
static int      s_retries = 0;
static char     s_scan_str[128] = "scan: (not run)";

static volatile uint16_t s_dbg_rawx = 0, s_dbg_rawy = 0;

static bool     s_have_touch = false;
static uint16_t s_cur_x = 0, s_cur_y = 0;
static bool     s_down = false;
static unsigned long s_down_at = 0;
static uint16_t s_start_x = 0, s_start_y = 0;
static uint16_t s_last_x = 0, s_last_y = 0;
static bool     s_live = false;
static uint16_t s_live_x = 0, s_live_y = 0;
static bool     s_tap_pending = false;
static uint16_t s_tap_x = 0, s_tap_y = 0;
static bool     s_swiping_now = false;
static bool     s_swipe_pending = false;
static int8_t   s_swipe_x = 0, s_swipe_y = 0;

static uint8_t  s_ui_rotation = 0;
static uint8_t  s_point_rotation = 0;

#ifndef RAK_TOUCH_SWIPE_MIN
  #define RAK_TOUCH_SWIPE_MIN 40
#endif
#ifndef RAK_TOUCH_TAP_MOVE_MAX
  #define RAK_TOUCH_TAP_MOVE_MAX 16
#endif
#ifndef RAK_TOUCH_LONG_MS
  #define RAK_TOUCH_LONG_MS 1000
#endif

// ---- Wire I2C: use the main bus (already initialised by ESP32Board) ----

static bool ft5x06WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(FT5x06_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool ft5x06ReadReg(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(FT5x06_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((int)FT5x06_ADDR, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
  return true;
}

// ---- Init (called by UITask loop every tick until success or give-up) ----

bool heltecV4CapTouchBegin() {
  if (s_init_ok) return true;
  if (s_given_up) return false;

  // Retry up to 3 times; after that give up permanently so the device
  // boots without touch rather than crashing.
  ++s_retries;
  if (s_retries > 3) {
    s_given_up = true;
    Serial.println("[TOUCH] giving up after 3 failed attempts — booting without touch");
    return false;
  }

  // The Wire bus is on SDA=9/SCL=40, already begun by ESP32Board::begin().
  // Bump speed for touch (the board default is 100kHz).
  // Short timeout prevents a stuck FT5x06 from stalling the main UI loop.
  Wire.setClock(400000);
  Wire.setTimeOut(20);

#if PIN_TOUCH_INT >= 0
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
#endif
  delay(10);

  // Probe
  Wire.beginTransmission(FT5x06_ADDR);
  if (Wire.endTransmission() != 0) {
    snprintf(s_scan_str, sizeof s_scan_str, "i2c9/40: no ACK at 0x%02X", FT5x06_ADDR);
    Serial.printf("[TOUCH] FT5x06 not found (retry %d/3)\n", s_retries);
    return false;
  }

  // Basic init: set normal operating mode
  if (!ft5x06WriteReg(FT5x06_REG_DEVMODE, 0x00)) {
    snprintf(s_scan_str, sizeof s_scan_str, "0x%02X init-fail(devmode)", FT5x06_ADDR);
    Serial.println("[TOUCH] FT5x06 devmode write failed");
    return false;
  }

  s_init_ok = true;
  snprintf(s_scan_str, sizeof s_scan_str, "0x%02X ft5x06 OK (retry %d)", FT5x06_ADDR, s_retries - 1);
  Serial.printf("[TOUCH] %s\n", s_scan_str);
  return true;
}

// ---- Coordinate mapping ----

static void mapRaw(uint16_t rx, uint16_t ry, uint16_t* ox, uint16_t* oy) {
  int sx, sy;
  // Runtime rotation transform — mirrors applyPointRotation in HeltecV4CapTouch.
  // s_point_rotation is set by heltecV4CapTouchSetPointRotation at boot.
  switch (s_point_rotation) {
    case 1:  // LV_DISP_ROT_90:  (RAW_H-1-py, px)  — raw portrait→landscape
      sx = (RAW_H - 1) - (int)ry;
      sy = (int)rx;
      break;
    case 3:  // LV_DISP_ROT_270: (py, RAW_W-1-px) — raw portrait→landscape
      sx = (int)ry;
      sy = (RAW_W - 1) - (int)rx;
      break;
    default: // portrait / ROT_180: identity
      sx = (int)rx;
      sy = (int)ry;
      break;
  }
  // Optional calibration flips (applied AFTER the rotation transform).
  // Define RAK_TS_FLIP_X / RAK_TS_FLIP_Y in build_flags if your panel's
  // X or Y axis reports inverted relative to the display coordinate space.
#if RAK_TS_FLIP_X
  sx = (SCR_W - 1) - sx;
#endif
#if RAK_TS_FLIP_Y
  sy = (SCR_H - 1) - sy;
#endif
  if (sx < 0) sx = 0; if (sx >= SCR_W) sx = SCR_W - 1;
  if (sy < 0) sy = 0; if (sy >= SCR_H) sy = SCR_H - 1;
  *ox = (uint16_t)sx;
  *oy = (uint16_t)sy;
}

// ---- Poll (single I2C transaction) ----

static int ft5x06ReadTouchData(uint16_t* rx, uint16_t* ry) {
  Wire.beginTransmission(FT5x06_ADDR);
  Wire.write(FT5x06_REG_TDSTATUS);
  if (Wire.endTransmission(false) != 0) return -1;

  uint8_t buf[7];
  uint8_t got = Wire.requestFrom((int)FT5x06_ADDR, (uint8_t)7);
  if (got < 5) return (got >= 1 && (buf[0] & 0x0F) == 0) ? 0 : -1;
  for (uint8_t i = 0; i < got; ++i) buf[i] = Wire.read();

  if ((buf[0] & 0x0F) == 0) return 0;

  *rx = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
  *ry = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
  return 1;
}

static void ft5x06Poll() {
  uint16_t rx = 0, ry = 0;
  if (ft5x06ReadTouchData(&rx, &ry) == 1) {
    s_dbg_rawx = rx;
    s_dbg_rawy = ry;
    mapRaw(rx, ry, &s_cur_x, &s_cur_y);
    s_have_touch = true;
  } else {
    s_have_touch = false;
  }
}

void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = s_dbg_rawx;
  if (ry) *ry = s_dbg_rawy;
}

// ---- Gesture state machine ----

int heltecV4CapTouchCheck() {
  if (!s_init_ok) return BUTTON_EVENT_NONE;
  ft5x06Poll();

  if (s_have_touch) {
    s_live = true;
    s_live_x = s_cur_x;
    s_live_y = s_cur_y;
    if (!s_down) {
      s_down = true;
      s_down_at = millis();
      s_start_x = s_cur_x;
      s_start_y = s_cur_y;
      s_swiping_now = false;
    }
    s_last_x = s_cur_x;
    s_last_y = s_cur_y;
    if (!s_swiping_now) {
      int adx = abs((int)s_last_x - (int)s_start_x);
      int ady = abs((int)s_last_y - (int)s_start_y);
      if (adx >= RAK_TOUCH_SWIPE_MIN && adx > ady) s_swiping_now = true;
    }
    return BUTTON_EVENT_NONE;
  }

  if (s_down) {
    s_down = false;
    s_live = false;
    s_swiping_now = false;
    unsigned long dur = millis() - s_down_at;
    int adx = abs((int)s_last_x - (int)s_start_x);
    int ady = abs((int)s_last_y - (int)s_start_y);
    if (adx >= RAK_TOUCH_SWIPE_MIN && adx > (ady + 8)) {
      bool left = ((int)s_last_x - (int)s_start_x) < 0;
      s_swipe_x = left ? -1 : 1;
      s_swipe_y = 0;
      s_swipe_pending = true;
      return left ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (ady >= RAK_TOUCH_SWIPE_MIN && ady > (adx + 8)) {
      s_swipe_x = 0;
      s_swipe_y = ((int)s_last_y - (int)s_start_y) < 0 ? -1 : 1;
      s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    if (dur >= 12 && dur < (unsigned long)RAK_TOUCH_LONG_MS &&
        adx <= RAK_TOUCH_TAP_MOVE_MAX && ady <= RAK_TOUCH_TAP_MOVE_MAX) {
      s_tap_x = s_last_x;
      s_tap_y = s_last_y;
      s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
    }
    if (dur >= (unsigned long)RAK_TOUCH_LONG_MS) {
      return BUTTON_EVENT_LONG_PRESS;
    }
  } else {
    s_live = false;
  }
  return BUTTON_EVENT_NONE;
}

bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (!s_live) return false;
  if (x) *x = s_live_x;
  if (y) *y = s_live_y;
  return true;
}
bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  if (!s_tap_pending) return false;
  s_tap_pending = false;
  if (x) *x = s_tap_x;
  if (y) *y = s_tap_y;
  return true;
}
bool heltecV4CapTouchPopSwipe(int8_t* xd, int8_t* yd) {
  if (!s_swipe_pending) return false;
  s_swipe_pending = false;
  if (xd) *xd = s_swipe_x;
  if (yd) *yd = s_swipe_y;
  return true;
}

// ---- Background poll (core 0) ----

static TaskHandle_t s_poll_task = nullptr;
static volatile bool s_async = false;
static uint32_t s_period_ms = 8;

static void touchPollTask(void* arg) {
  (void)arg;
  for (;;) {
    heltecV4CapTouchCheck();
    vTaskDelay(pdMS_TO_TICKS(s_period_ms));
  }
}

bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (s_async || !s_init_ok) return false;
  s_period_ms = period_ms < 4 ? 4 : (period_ms > 100 ? 100 : period_ms);
  BaseType_t ok = xTaskCreatePinnedToCore(touchPollTask, "raktap_touch", 3072,
                                          nullptr, 2, &s_poll_task, 0);
  if (ok == pdPASS) { s_async = true; return true; }
  return false;
}
bool heltecV4CapTouchIsAsyncPolling() { return s_async; }
bool heltecV4CapTouchIsSwiping() { return s_swiping_now; }
void heltecV4CapTouchSetRotation(uint8_t r) { s_ui_rotation = r & 3; }
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r & 3; }
const char* heltecV4CapTouchDebug() { return s_scan_str; }

#endif
