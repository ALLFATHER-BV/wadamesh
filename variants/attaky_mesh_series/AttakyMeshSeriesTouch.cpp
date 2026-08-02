// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/helpers/input/HeltecV4CapTouch.h"

#if (defined(HAS_HELTEC_V4_CAP_TOUCH) || defined(HAS_TOUCH_UI)) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include "AttakySharedI2C.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>

#ifndef PIN_BOARD_SDA
  #define PIN_BOARD_SDA 2
#endif
#ifndef PIN_BOARD_SCL
  #define PIN_BOARD_SCL 1
#endif

static const uint8_t FT6636_ADDR        = 0x38;
static const uint8_t FT6636_REG_MODE    = 0x00;
static const uint8_t FT6636_REG_TD_ST   = 0x02;
static const uint8_t FT6636_REG_VENDOR  = 0xA8;

static const uint8_t AW9523_ADDR        = 0x59;
static const uint8_t AW9523_REG_OUT_P1  = 0x03;
static const uint8_t AW9523_REG_CFG_P1  = 0x05;
static const uint8_t AW9523_REG_LEDM_P1 = 0x13;
static const uint8_t AW9523_P13_BIT     = (1u << 3);

#ifndef HELTEC_V4_TOUCH_LONG_MS
  #define HELTEC_V4_TOUCH_LONG_MS 1000
#endif
#ifndef HELTEC_V4_TOUCH_LONG_MOVE_MAX
  #define HELTEC_V4_TOUCH_LONG_MOVE_MAX 18
#endif
#ifndef HELTEC_V4_TOUCH_SWIPE_MIN
  #define HELTEC_V4_TOUCH_SWIPE_MIN 36
#endif
#ifndef HELTEC_V4_TOUCH_SWIPE_INVERT
  #define HELTEC_V4_TOUCH_SWIPE_INVERT 0
#endif

static bool s_present = false;
static bool s_init_attempted = false;
static bool s_init_ok = false;
static unsigned long s_last_init_try_ms = 0;
static bool s_tap_pending = false;
static uint16_t s_tap_x = 0, s_tap_y = 0;
static bool s_swipe_pending = false;
static int8_t s_swipe_x = 0, s_swipe_y = 0;
static bool s_live = false;
static uint16_t s_live_x = 0, s_live_y = 0;
static uint16_t s_raw_x = 0, s_raw_y = 0;
static uint8_t s_vendor_id = 0;
static char s_debug[48] = "";
static bool s_swiping_now = false;

static uint8_t s_ui_rotation = 0;
void heltecV4CapTouchSetRotation(uint8_t lvgl_rot) { s_ui_rotation = lvgl_rot & 3; }

static void rotateSwipeDelta(int dx, int dy, int* odx, int* ody) {
  switch (s_ui_rotation) {
    case 1:  *odx = -dy; *ody =  dx; break;
    case 2:  *odx = -dx; *ody = -dy; break;
    case 3:  *odx =  dy; *ody = -dx; break;
    default: *odx =  dx; *ody =  dy; break;
  }
}

static uint8_t s_point_rotation = 0;
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r & 3; }

static void applyPointRotation(uint16_t* x, uint16_t* y) {
  if (s_point_rotation == 0) return;
  const int W = 240;
  const int H = 320;
  const int px = *x, py = *y;
  int lx, ly;
  switch (s_point_rotation) {
    case 1:  lx = (H - 1) - py;  ly = px;           break;
    case 2:  lx = (W - 1) - px;  ly = (H - 1) - py; break;
    case 3:  lx = py;            ly = (W - 1) - px; break;
    default: lx = px;            ly = py;           break;
  }
  if (lx < 0) lx = 0;
  if (ly < 0) ly = 0;
  *x = (uint16_t)lx;
  *y = (uint16_t)ly;
}

static void mapTouchToDisplay(uint16_t in_x, uint16_t in_y, uint16_t* out_x, uint16_t* out_y) {
#ifndef DISPLAY_WIDTH
  #define DISPLAY_WIDTH 240
#endif
#ifndef DISPLAY_HEIGHT
  #define DISPLAY_HEIGHT 320
#endif
#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 0
#endif

  int x = in_x;
  int y = in_y;
  int w = DISPLAY_WIDTH;
  int h = DISPLAY_HEIGHT;
  switch (DISPLAY_ROTATION & 3) {
    case 1: {
      int nx = h - 1 - y;
      int ny = x;
      x = nx;
      y = ny;
    } break;
    case 2:
#if defined(UI_LVGL)
      break;
#else
      x = w - 1 - x;
      y = h - 1 - y;
      break;
#endif
    case 3: {
      int nx = y;
      int ny = w - 1 - x;
      x = nx;
      y = ny;
    } break;
    default:
      break;
  }
  // Landscape rotations swap output dimensions.
  int w_out = w, h_out = h;
  if ((DISPLAY_ROTATION & 3) == 1 || (DISPLAY_ROTATION & 3) == 3) { w_out = h; h_out = w; }
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= w_out) x = w_out - 1;
  if (y >= h_out) y = h_out - 1;
  if (out_x) *out_x = (uint16_t)x;
  if (out_y) *out_y = (uint16_t)y;
}

static bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n) {
  if (!attakyI2cLock(20)) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) { attakyI2cUnlock(); return false; }
  uint8_t got = Wire.requestFrom((int)addr, (int)n);
  if (got != n) { attakyI2cUnlock(); return false; }
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  attakyI2cUnlock();
  return true;
}

static bool aw9523_read(uint8_t reg, uint8_t* val) {
  return i2c_read_reg(AW9523_ADDR, reg, val, 1);
}

static bool aw9523_write(uint8_t reg, uint8_t val) {
  if (!attakyI2cLock(20)) return false;
  Wire.beginTransmission(AW9523_ADDR);
  Wire.write(reg);
  Wire.write(val);
  bool ok = (Wire.endTransmission() == 0);
  attakyI2cUnlock();
  return ok;
}

static bool aw9523_set_bit(uint8_t reg, uint8_t bitmask, bool set) {
  uint8_t v = 0;
  if (!aw9523_read(reg, &v)) return false;
  uint8_t nv = set ? (uint8_t)(v | bitmask) : (uint8_t)(v & ~bitmask);
  if (nv == v) return true;
  return aw9523_write(reg, nv);
}

static void ft6636_hw_reset() {
  bool ok = true;
  ok &= aw9523_set_bit(AW9523_REG_LEDM_P1, AW9523_P13_BIT, true);
  ok &= aw9523_set_bit(AW9523_REG_CFG_P1,  AW9523_P13_BIT, false);
  ok &= aw9523_set_bit(AW9523_REG_OUT_P1,  AW9523_P13_BIT, false);
  if (!ok) {
    delay(300);
    return;
  }
  delay(10);
  aw9523_set_bit(AW9523_REG_OUT_P1, AW9523_P13_BIT, true);
  delay(300);
}

static int ft6636_read_touch(uint16_t* x, uint16_t* y) {
  uint8_t buf[7];
  if (!i2c_read_reg(FT6636_ADDR, FT6636_REG_TD_ST, buf, 7)) {
    static uint32_t ec = 0; if ((ec++ & 0x3F) == 0) Serial.printf("[TCH] i2c read FAIL\n");
    return -1;
  }
  uint8_t touch_count = buf[0] & 0x0F;
  uint8_t event_flag  = (buf[1] >> 6) & 0x03;
  uint16_t px = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
  uint16_t py = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
  bool down = (touch_count >= 1) && (event_flag != 1);
  if (!down) return -1;
  if (x) *x = px;
  if (y) *y = py;
  return 0;
}

bool heltecV4CapTouchBegin() {
  if (s_init_ok) return true;
  unsigned long now = millis();
  if (s_init_attempted && (unsigned long)(now - s_last_init_try_ms) < 400) return false;
  s_init_attempted = true;
  s_last_init_try_ms = now;

  attakyI2cInit();

  ft6636_hw_reset();

  uint8_t vid = 0;
  if (!i2c_read_reg(FT6636_ADDR, FT6636_REG_VENDOR, &vid, 1) || vid == 0) {
    s_vendor_id = vid;
    s_present = false;
    s_init_ok = false;
    snprintf(s_debug, sizeof(s_debug), "FT6636 absent (vid=0x%02X)", vid);
    return false;
  }
  s_vendor_id = vid;
  s_present = true;

  if (attakyI2cLock(20)) {
    Wire.beginTransmission(FT6636_ADDR);
    Wire.write(FT6636_REG_MODE);
    Wire.write((uint8_t)0x00);
    Wire.endTransmission();
    attakyI2cUnlock();
  }

  snprintf(s_debug, sizeof(s_debug), "FT6636 ok (vid=0x%02X)", vid);
  s_init_ok = true;
  return true;
}

int heltecV4CapTouchCheck() {
  if (!s_init_ok || !s_present) {
    s_live = false;
    return BUTTON_EVENT_NONE;
  }

  static bool down = false;
  static unsigned long down_at = 0;
  static bool long_dispatched = false;
  static uint16_t start_x = 0, start_y = 0;
  static uint16_t last_x = 0, last_y = 0;
  // Ignore one transient miss before ending a gesture.
  static uint8_t release_misses = 0;
  constexpr uint8_t RELEASE_DEBOUNCE = 2;

  uint16_t x = 0, y = 0;
  int r = ft6636_read_touch(&x, &y);
  bool now = (r == 0);
  if (now) { s_raw_x = x; s_raw_y = y; }

  if (!now && down && release_misses < RELEASE_DEBOUNCE) {
    ++release_misses;
    return BUTTON_EVENT_NONE;
  }
  if (now) release_misses = 0;

  if (now && !down) {
    down = true;
    down_at = millis();
    long_dispatched = false;
    start_x = last_x = x;
    start_y = last_y = y;
    s_swiping_now = false;
    mapTouchToDisplay(start_x, start_y, &s_live_x, &s_live_y);
    applyPointRotation(&s_live_x, &s_live_y);
    s_live = true;
  } else if (now && down) {
    last_x = x;
    last_y = y;
    mapTouchToDisplay(last_x, last_y, &s_live_x, &s_live_y);
    applyPointRotation(&s_live_x, &s_live_y);
    s_live = true;
    // Only horizontal swipes suppress LVGL clicks.
    if (!s_swiping_now) {
      int rdx = static_cast<int>(last_x) - static_cast<int>(start_x);
      int rdy = static_cast<int>(last_y) - static_cast<int>(start_y);
      int dx, dy;
      rotateSwipeDelta(rdx, rdy, &dx, &dy);
      int adx = dx < 0 ? -dx : dx;
      int ady = dy < 0 ? -dy : dy;
      if (adx >= HELTEC_V4_TOUCH_SWIPE_MIN && adx > ady) {
        s_swiping_now = true;
      }
    }
  } else if (!now && down) {
    s_live = false;
    down = false;
    s_swiping_now = false;
    unsigned long dur = (down_at > 0) ? (unsigned long)(millis() - down_at) : 0;
    down_at = 0;
    uint16_t sx = 0, sy = 0, ex = 0, ey = 0;
    mapTouchToDisplay(start_x, start_y, &sx, &sy);
    mapTouchToDisplay(last_x, last_y, &ex, &ey);
    int rdx = (int)ex - (int)sx;
    int rdy = (int)ey - (int)sy;
    int dx, dy;
    rotateSwipeDelta(rdx, rdy, &dx, &dy);
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (!long_dispatched && adx >= HELTEC_V4_TOUCH_SWIPE_MIN && adx > (ady + 8)) {
      bool swipe_left = dx < 0;
#if HELTEC_V4_TOUCH_SWIPE_INVERT
      swipe_left = !swipe_left;
#endif
      s_swipe_x = swipe_left ? -1 : 1;
      s_swipe_y = 0;
      s_swipe_pending = true;
      // Button events carry horizontal swipe direction.
      return swipe_left ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (!long_dispatched && ady >= HELTEC_V4_TOUCH_SWIPE_MIN && ady > (adx + 8)) {
      s_swipe_x = 0;
      s_swipe_y = (dy < 0) ? -1 : 1;
      s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    if (!long_dispatched && dur >= 12 && dur < (unsigned long)HELTEC_V4_TOUCH_LONG_MS) {
      s_tap_x = ex;
      s_tap_y = ey;
      applyPointRotation(&s_tap_x, &s_tap_y);
      s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
    }
    long_dispatched = false;
  } else if (down && !long_dispatched && down_at > 0 &&
             (unsigned long)(millis() - down_at) >= (unsigned long)HELTEC_V4_TOUCH_LONG_MS) {
    int dx = (int)last_x - (int)start_x;
    int dy = (int)last_y - (int)start_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx > HELTEC_V4_TOUCH_LONG_MOVE_MAX || ady > HELTEC_V4_TOUCH_LONG_MOVE_MAX) {
      return BUTTON_EVENT_NONE;
    }
    long_dispatched = true;
    return BUTTON_EVENT_LONG_PRESS;
  }

  return BUTTON_EVENT_NONE;
}

bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  if (!s_tap_pending) return false;
  s_tap_pending = false;
  if (x) *x = s_tap_x;
  if (y) *y = s_tap_y;
  return true;
}

bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (!s_live) return false;
  if (x) *x = s_live_x;
  if (y) *y = s_live_y;
  return true;
}

bool heltecV4CapTouchPopSwipe(int8_t* x_dir, int8_t* y_dir) {
  if (!s_swipe_pending) return false;
  s_swipe_pending = false;
  if (x_dir) *x_dir = s_swipe_x;
  if (y_dir) *y_dir = s_swipe_y;
  s_swipe_x = 0;
  s_swipe_y = 0;
  return true;
}

// Poll touch independently from LVGL rendering.
static TaskHandle_t s_poll_task = nullptr;
static uint32_t     s_poll_period_ms = 8;
static volatile bool s_async_active = false;

static void touchPollTaskFn(void* /*arg*/) {
  s_async_active = true;
  const TickType_t period = pdMS_TO_TICKS(s_poll_period_ms ? s_poll_period_ms : 8);
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    (void)heltecV4CapTouchCheck();
    vTaskDelayUntil(&last_wake, period);
  }
}

bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (s_poll_task != nullptr) return false;
  if (!s_init_ok) return false;
  if (period_ms < 4) period_ms = 4;
  if (period_ms > 100) period_ms = 100;
  s_poll_period_ms = period_ms;
  BaseType_t ok = xTaskCreatePinnedToCore(
      touchPollTaskFn,
      "touch_poll",
      4096,
      nullptr,
      1,
      &s_poll_task,
      0);
  if (ok != pdPASS) {
    s_poll_task = nullptr;
    return false;
  }
  return true;
}

bool heltecV4CapTouchIsAsyncPolling() {
  return s_async_active;
}

bool heltecV4CapTouchIsSwiping() {
  return s_swiping_now;
}

const char* heltecV4CapTouchDebug() { return s_debug; }

void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = s_raw_x;
  if (ry) *ry = s_raw_y;
}

#endif
