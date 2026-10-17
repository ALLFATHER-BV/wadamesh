// SPDX-License-Identifier: GPL-3.0-or-later
// Elecrow CrowPanel Advanced 10.1" ESP32-P4: GT911 capacitive touch, implementing the shared
// touch-input API (HeltecV4CapTouch.h).
//
// Wiring (schematic V1.0/V1.1/V1.2, all identical, and Elecrow's Lesson05 BSP): GT911 on the touch
// FPC, I2C0 SDA=GPIO45 / SCL=GPIO46 (shared with the codec and the HY2.0 I2C port; Wire is begun
// there by ESP32Board::begin via PIN_BOARD_SDA/SCL), RST=GPIO40, INT=GPIO42, both active low.
// Elecrow configures the controller for 1024x600 with no swap or mirror, so raw == screen; the
// driver still reads the GT911's own X/Y maximum at init and scales if it reports another grid.
//
// The gesture machine is the one shared with the T-Display P4 / CHSC6x / FT5x06 drivers; only the
// report read differs.
#if defined(HAS_CROWPANEL_P4_101) && defined(ESP32)

#include <helpers/input/HeltecV4CapTouch.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>

#ifndef PIN_TOUCH_RST
  #define PIN_TOUCH_RST 40
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 42
#endif

static const int SCR_W = 1024;
static const int SCR_H = 600;

static uint8_t  s_addr = 0;              // 0x5D or 0x14, whichever answered
static uint16_t s_native_w = SCR_W;      // GT911 config X/Y maximum (read at init)
static uint16_t s_native_h = SCR_H;

static bool     s_init_ok  = false;
static bool     s_given_up = false;
static int      s_retries  = 0;
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

// 10.1" at 1024x600 is ~118 px per inch, so the thresholds are in the same physical range as the
// T-Display P4's (60 px swipe at its 2x-scaled logical space).
#ifndef CP_TOUCH_SWIPE_MIN
  #define CP_TOUCH_SWIPE_MIN 70
#endif
#ifndef CP_TOUCH_TAP_MOVE_MAX
  #define CP_TOUCH_TAP_MOVE_MAX 24
#endif
#ifndef CP_TOUCH_LONG_MS
  #define CP_TOUCH_LONG_MS 1000
#endif

// ---- GT911 register access (16-bit big-endian register addresses) ----
static bool gtRead(uint16_t reg, uint8_t* out, uint8_t n) {
  Wire.beginTransmission(s_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) out[i] = Wire.read();
  return true;
}
static bool gtWrite8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(s_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}
static bool probe(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

// Hardware reset with INT held LOW through the rising edge of RST: the GT911 then latches I2C
// address 0x5D (Goodix datasheet power-on timing). INT is released to an input afterwards.
static void gtHardReset() {
  pinMode(PIN_TOUCH_INT, OUTPUT);
  digitalWrite(PIN_TOUCH_INT, LOW);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(20);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(60);   // address latched >= 5 ms after RST rises; keep INT low until the controller is up
  pinMode(PIN_TOUCH_INT, INPUT);
  delay(50);
}

bool heltecV4CapTouchBegin() {
  if (s_init_ok)  return true;
  if (s_given_up) return false;
  if (++s_retries > 3) {
    s_given_up = true;
    Serial.println("[TOUCH] GT911 giving up after 3 attempts, booting without touch");
    return false;
  }

  Wire.setClock(400000);
  Wire.setTimeOut(20);
  if (s_retries == 1) gtHardReset();

  if (s_retries == 1) {   // log the bus once: tells a tester exactly what answered on 45/46
    char sc[96]; int n = snprintf(sc, sizeof sc, "[TOUCH] i2c45/46 scan:");
    for (uint8_t a = 0x08; a < 0x78 && n < (int)sizeof sc - 6; a++)
      if (probe(a)) n += snprintf(sc + n, sizeof sc - n, " %02X", a);
    Serial.println(sc);
  }

  s_addr = probe(0x5D) ? 0x5D : (probe(0x14) ? 0x14 : 0);
  if (!s_addr) {
    snprintf(s_scan_str, sizeof s_scan_str, "i2c45/46: no GT911 at 0x5D/0x14");
    Serial.printf("[TOUCH] GT911 not found (retry %d/3)\n", s_retries);
    return false;
  }

  // Product ID "911" at 0x8140, config X/Y maximum at 0x8048..0x804B (little-endian).
  uint8_t id[4] = {0}, cfg[4] = {0};
  gtRead(0x8140, id, 4);
  if (gtRead(0x8048, cfg, 4)) {
    const uint16_t xm = (uint16_t)(cfg[0] | (cfg[1] << 8));
    const uint16_t ym = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (xm >= 100 && xm <= 4096 && ym >= 100 && ym <= 4096) { s_native_w = xm; s_native_h = ym; }
  }
  gtWrite8(0x814E, 0);   // clear any stale "buffer ready"

  s_init_ok = true;
  snprintf(s_scan_str, sizeof s_scan_str, "0x%02X gt911 '%c%c%c%c' %ux%u",
           s_addr, id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?', id[3] ? id[3] : ' ',
           (unsigned)s_native_w, (unsigned)s_native_h);
  Serial.printf("[TOUCH] %s\n", s_scan_str);
  return true;
}

static void mapRaw(uint16_t rx, uint16_t ry, uint16_t* ox, uint16_t* oy) {
  int sx = (int)rx, sy = (int)ry;
  if (s_native_w != SCR_W) sx = sx * SCR_W / s_native_w;
  if (s_native_h != SCR_H) sy = sy * SCR_H / s_native_h;
  if (sx < 0) sx = 0; if (sx > SCR_W - 1) sx = SCR_W - 1;
  if (sy < 0) sy = 0; if (sy > SCR_H - 1) sy = SCR_H - 1;
  *ox = (uint16_t)sx;
  *oy = (uint16_t)sy;
}

// 1 = finger down (rx/ry raw), 0 = no touch, -1 = I2C error.
static int gtReadPoint(uint16_t* rx, uint16_t* ry) {
  uint8_t st = 0;
  if (!gtRead(0x814E, &st, 1)) return -1;
  if (!(st & 0x80)) return s_have_touch ? 1 : 0;    // no new frame yet: keep the last state
  const uint8_t n = st & 0x0F;
  int ret = 0;
  if (n >= 1 && n <= 5) {
    uint8_t p[8] = {0};                              // point 1: id, xL, xH, yL, yH, sizeL, sizeH, res
    if (gtRead(0x8150, p, 8)) {
      *rx = (uint16_t)(p[0] | (p[1] << 8));
      *ry = (uint16_t)(p[2] | (p[3] << 8));
      ret = 1;
    }
  }
  gtWrite8(0x814E, 0);                               // hand the buffer back to the controller
  return ret;
}

static void gtPoll() {
  uint16_t rx = s_dbg_rawx, ry = s_dbg_rawy;
  const int r = gtReadPoint(&rx, &ry);
  if (r == 1) {
    s_dbg_rawx = rx;
    s_dbg_rawy = ry;
    mapRaw(rx, ry, &s_cur_x, &s_cur_y);
    s_have_touch = true;
  } else if (r == 0) {
    s_have_touch = false;
  }
}

void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = s_dbg_rawx;
  if (ry) *ry = s_dbg_rawy;
}

int heltecV4CapTouchCheck() {
  if (!s_init_ok) return BUTTON_EVENT_NONE;
  gtPoll();

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
      if (adx >= CP_TOUCH_SWIPE_MIN && adx > ady) s_swiping_now = true;
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
    if (adx >= CP_TOUCH_SWIPE_MIN && adx > (ady + 8)) {
      bool left = ((int)s_last_x - (int)s_start_x) < 0;
      s_swipe_x = left ? -1 : 1;
      s_swipe_y = 0;
      s_swipe_pending = true;
      return left ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (ady >= CP_TOUCH_SWIPE_MIN && ady > (adx + 8)) {
      s_swipe_x = 0;
      s_swipe_y = ((int)s_last_y - (int)s_start_y) < 0 ? -1 : 1;
      s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    if (dur >= 12 && dur < (unsigned long)CP_TOUCH_LONG_MS &&
        adx <= CP_TOUCH_TAP_MOVE_MAX && ady <= CP_TOUCH_TAP_MOVE_MAX) {
      s_tap_x = s_last_x;
      s_tap_y = s_last_y;
      s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
    }
    if (dur >= (unsigned long)CP_TOUCH_LONG_MS) return BUTTON_EVENT_LONG_PRESS;
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

static TaskHandle_t s_poll_task = nullptr;
static volatile bool s_async = false;
static uint32_t s_period_ms = 8;

static void touchPollTask(void*) {
  for (;;) {
    heltecV4CapTouchCheck();
    vTaskDelay(pdMS_TO_TICKS(s_period_ms));
  }
}

bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (s_async || !s_init_ok) return false;
  s_period_ms = period_ms < 4 ? 4 : (period_ms > 100 ? 100 : period_ms);
  if (xTaskCreatePinnedToCore(touchPollTask, "gt911_touch", 3072, nullptr, 2, &s_poll_task, 0) == pdPASS) {
    s_async = true;
    return true;
  }
  return false;
}
bool heltecV4CapTouchIsAsyncPolling() { return s_async; }
bool heltecV4CapTouchIsSwiping() { return s_swiping_now; }
void heltecV4CapTouchSetRotation(uint8_t r) { s_ui_rotation = r & 3; }
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r & 3; }
const char* heltecV4CapTouchDebug() { return s_scan_str; }

#endif  // HAS_CROWPANEL_P4_101 && ESP32
