// SPDX-License-Identifier: GPL-3.0-or-later
// LilyGo T-Watch S3 touch input — implements the shared touch-input API
// (declared in HeltecV4CapTouch.h) for the T-Watch S3's FT6x36-family
// capacitive controller (I2C1 addr 0x38).
//
// The controller sits on the SECOND I2C bus (Wire1: SDA=39 / SCL=40) that the
// T-Watch S3 reserves for touch — NOT the sensor bus (Wire: SDA=10 / SCL=11)
// that ESP32Board::begin() initialises. This driver owns Wire1.
//
// Protocol reference: LovyanGFX Touch_FT5x06.cpp (FreeBSD license); the
// FT6x36/FT6336 register map is byte-compatible with FT5x06 for the status
// and coordinate reads used here.

#if defined(HAS_TWATCH_S3) && defined(ESP32)

#include "HeltecV4CapTouch.h"
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>

#ifndef PIN_TOUCH_SDA
  #define PIN_TOUCH_SDA 39
#endif
#ifndef PIN_TOUCH_SCL
  #define PIN_TOUCH_SCL 40
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 16
#endif

// The T-Watch S3 panel is a square 240x240 — the touch controller reports in
// the same frame as the display, so no landscape swap is needed. Rotation is
// handled by s_point_rotation (identity for portrait; set by the UI at boot).
static const int SCR_W = 240;
static const int SCR_H = 240;
static const int RAW_W = 240;
static const int RAW_H = 240;

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

#ifndef TWATCH_TOUCH_SWIPE_MIN
  #define TWATCH_TOUCH_SWIPE_MIN 40
#endif
#ifndef TWATCH_TOUCH_TAP_MOVE_MAX
  #define TWATCH_TOUCH_TAP_MOVE_MAX 16
#endif
#ifndef TWATCH_TOUCH_LONG_MS
  #define TWATCH_TOUCH_LONG_MS 1000
#endif

// ---- Wire1 I2C (owned by this driver) ----

static bool ft5x06WriteReg(uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(FT5x06_ADDR);
  Wire1.write(reg);
  Wire1.write(val);
  return Wire1.endTransmission() == 0;
}

static bool ft5x06ReadReg(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire1.beginTransmission(FT5x06_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  uint8_t got = Wire1.requestFrom((int)FT5x06_ADDR, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; ++i) buf[i] = Wire1.read();
  return true;
}

// ---- Init (called by UITask loop every tick until success or give-up) ----

bool heltecV4CapTouchBegin() {
  if (s_init_ok) return true;
  if (s_given_up) return false;

  ++s_retries;
  if (s_retries > 3) {
    s_given_up = true;
    Serial.println("[TOUCH] giving up after 3 failed attempts — booting without touch");
    return false;
  }

  // The touch bus (Wire1) is NOT initialised by ESP32Board::begin() (that is
  // the sensor bus). Bring it up here and own it.
  if (s_retries == 1) {
    Wire1.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    Wire1.setClock(400000);
    Wire1.setTimeOut(20);
  }

#if PIN_TOUCH_INT >= 0
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
#endif
  delay(10);

  Wire1.beginTransmission(FT5x06_ADDR);
  if (Wire1.endTransmission() != 0) {
    snprintf(s_scan_str, sizeof s_scan_str, "i2c1 39/40: no ACK at 0x%02X", FT5x06_ADDR);
    Serial.printf("[TOUCH] FT6x36 not found (retry %d/3)\n", s_retries);
    return false;
  }

  if (!ft5x06WriteReg(FT5x06_REG_DEVMODE, 0x00)) {
    snprintf(s_scan_str, sizeof s_scan_str, "0x%02X init-fail(devmode)", FT5x06_ADDR);
    Serial.println("[TOUCH] FT6x36 devmode write failed");
    return false;
  }

  s_init_ok = true;
  snprintf(s_scan_str, sizeof s_scan_str, "0x%02X ft6x36 OK (retry %d)", FT5x06_ADDR, s_retries - 1);
  Serial.printf("[TOUCH] %s\n", s_scan_str);
  return true;
}

// ---- Coordinate mapping ----

static void mapRaw(uint16_t rx, uint16_t ry, uint16_t* ox, uint16_t* oy) {
  int sx, sy;
  // Square panel: rotation just permutes the axes; portrait is identity.
  switch (s_point_rotation) {
    case 1:  // LV_DISP_ROT_90
      sx = (RAW_H - 1) - (int)ry;
      sy = (int)rx;
      break;
    case 2:  // LV_DISP_ROT_180
      sx = (RAW_W - 1) - (int)rx;
      sy = (RAW_H - 1) - (int)ry;
      break;
    case 3:  // LV_DISP_ROT_270
      sx = (int)ry;
      sy = (RAW_W - 1) - (int)rx;
      break;
    default: // LV_DISP_ROT_NONE — identity
      sx = (int)rx;
      sy = (int)ry;
      break;
  }
#if TWATCH_TS_FLIP_X
  sx = (SCR_W - 1) - sx;
#endif
#if TWATCH_TS_FLIP_Y
  sy = (SCR_H - 1) - sy;
#endif
  if (sx < 0) sx = 0; if (sx >= SCR_W) sx = SCR_W - 1;
  if (sy < 0) sy = 0; if (sy >= SCR_H) sy = SCR_H - 1;
  *ox = (uint16_t)sx;
  *oy = (uint16_t)sy;
}

// ---- Poll (single I2C transaction) ----

static int ft5x06ReadTouchData(uint16_t* rx, uint16_t* ry) {
  Wire1.beginTransmission(FT5x06_ADDR);
  Wire1.write(FT5x06_REG_TDSTATUS);
  if (Wire1.endTransmission(false) != 0) return -1;

  uint8_t buf[7];
  uint8_t got = Wire1.requestFrom((int)FT5x06_ADDR, (uint8_t)7);
  if (got < 5) return -1;
  for (uint8_t i = 0; i < got; ++i) buf[i] = Wire1.read();

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
      if (adx >= TWATCH_TOUCH_SWIPE_MIN && adx > ady) s_swiping_now = true;
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
    if (adx >= TWATCH_TOUCH_SWIPE_MIN && adx > (ady + 8)) {
      bool left = ((int)s_last_x - (int)s_start_x) < 0;
      s_swipe_x = left ? -1 : 1;
      s_swipe_y = 0;
      s_swipe_pending = true;
      return left ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (ady >= TWATCH_TOUCH_SWIPE_MIN && ady > (adx + 8)) {
      s_swipe_x = 0;
      s_swipe_y = ((int)s_last_y - (int)s_start_y) < 0 ? -1 : 1;
      s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    if (dur >= 12 && dur < (unsigned long)TWATCH_TOUCH_LONG_MS &&
        adx <= TWATCH_TOUCH_TAP_MOVE_MAX && ady <= TWATCH_TOUCH_TAP_MOVE_MAX) {
      s_tap_x = s_last_x;
      s_tap_y = s_last_y;
      s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
    }
    if (dur >= (unsigned long)TWATCH_TOUCH_LONG_MS) {
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
  BaseType_t ok = xTaskCreatePinnedToCore(touchPollTask, "twatch_touch", 3072,
                                          nullptr, 2, &s_poll_task, 0);
  if (ok == pdPASS) { s_async = true; return true; }
  return false;
}
bool heltecV4CapTouchIsAsyncPolling() { return s_async; }
bool heltecV4CapTouchIsSwiping() { return s_swiping_now; }
void heltecV4CapTouchSetRotation(uint8_t r) { s_ui_rotation = r & 3; }
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r & 3; }
const char* heltecV4CapTouchDebug() { return s_scan_str; }

#endif  // HAS_TWATCH_S3
