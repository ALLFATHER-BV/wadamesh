// SPDX-License-Identifier: GPL-3.0-or-later
// LilyGo T-Display P4 (AMOLED) touch input — implements the shared touch-input
// API (HeltecV4CapTouch.h) for the board's HI8561 capacitive controller.
//
// The HI8561 shares I2C_1 (Wire, SDA=7 / SCL=8) with the XL9535 expander, the
// PCF8563 RTC and the BQ27220 gauge. Wire is already begun by the XL9535 driver
// (xl9535.begin(7,8) in main.cpp) and the controller's RST/INT lines are driven
// by the expander — powerOnSequence() releases TOUCH_RST (IO3) before we probe.
// So, like RakTapV2Touch, this driver uses the shared Wire and must NOT re-init
// or reset the bus.
//
// STATE MACHINE: the tap/swipe/long-press gesture logic below is the proven one
// shared with the CHSC6x (Heltec V4) and FT5x06 (RAK Tap) drivers — only the
// physical report read (hi8561ReadPoint) and the panel geometry differ.
//
// ⚠️ FIRST-BOOT STATUS (display-first): hi8561ReadPoint() currently reports NO
// touch — the panel boots and renders, but taps aren't delivered yet. Enabling
// real touch is a single localised change: fill in the HI8561 report parse (addr
// 0x48; see the TODO in hi8561ReadPoint) and verify the coordinate mapping on the
// device. A wrong report layout produces phantom presses that lock the UI, so it
// stays disabled until it can be validated against hardware. Everything else
// (probe, async poll task, gesture machine, rotation) is complete.

#if defined(HAS_HI8561_TOUCH) && defined(ESP32)

#include <helpers/input/HeltecV4CapTouch.h>   // shared touch-API contract (this file lives in variants/, not next to it)
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>

#ifndef PIN_TOUCH_SDA
  #define PIN_TOUCH_SDA 7
#endif
#ifndef PIN_TOUCH_SCL
  #define PIN_TOUCH_SCL 8
#endif

// RM69A10 AMOLED is a fixed 568×1232 portrait panel (no display rotation on this
// board — CAP_ROTATABLE=0). The controller reports in the same portrait space,
// so raw == screen and the default (identity) mapping applies.
// LVGL renders at 284x616 (half the 568x1232 panel; RM69A10Display upscales 2x on flush), so touch
// coordinates must be reported in that SAME 284x616 logical space — the GT9895's native 1060x2400
// grid is scaled straight to 284x616 here.
static const int SCR_W = 284;
static const int SCR_H = 616;
static const int RAW_W = 284;
static const int RAW_H = 616;

// NOTE: despite this file's name, the LilyGo T-Display P4 (RM69A10 variant) actually ships a Goodix
// GT9895 touch controller at 0x5D — NOT the HI8561 (that's a different display variant). An I2C bus
// scan on the real board found 0x5D. This driver talks to the GT9895. Ported from LilyGo
// cpp_bus_driver gt9895.cpp: the touch report is at register 0x00010308; each read = write that
// 32-bit address big-endian (4 bytes), then read the report. Scale factors default to 1.0 (the
// controller reports directly in panel coordinates), so no rescale is needed.
static const uint8_t GT9895_ADDR = 0x5D;
static const uint8_t GT9895_TOUCH_CMD[4] = { 0x00, 0x01, 0x03, 0x08 };  // register 0x00010308, big-endian
// The GT9895 reports in its native 1060x2400 digitiser grid; scale to the 568x1232 panel (LilyGo's
// GT9895_MAX_X_SIZE/MAX_Y_SIZE + RM69A10_SCREEN_W/H scale factors). Without this the raw coords
// over-run the panel (touch clamps past ~54% X / ~51% Y).
static const int GT9895_NATIVE_W = 1060;
static const int GT9895_NATIVE_H = 2400;

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

// Larger panel -> larger swipe threshold than the 240×320 boards.
#ifndef P4_TOUCH_SWIPE_MIN
  #define P4_TOUCH_SWIPE_MIN 60
#endif
#ifndef P4_TOUCH_TAP_MOVE_MAX
  #define P4_TOUCH_TAP_MOVE_MAX 24
#endif
#ifndef P4_TOUCH_LONG_MS
  #define P4_TOUCH_LONG_MS 1000
#endif

// ---- GT9895 low-level access ----

// Write an N-byte command (repeated start), then read M bytes.
static bool gt9895WriteRead(const uint8_t* cmd, uint8_t clen, uint8_t* out, uint8_t olen) {
  Wire.beginTransmission(GT9895_ADDR);
  Wire.write(cmd, clen);
  if (Wire.endTransmission(false) != 0) return false;   // repeated start, no stop
  uint8_t got = Wire.requestFrom((int)GT9895_ADDR, (int)olen);
  if (got != olen) return false;
  for (uint8_t i = 0; i < olen; i++) out[i] = Wire.read();
  return true;
}

// ---- Init (called by UITask loop every tick until success or give-up) ----

bool heltecV4CapTouchBegin() {
  if (s_init_ok)  return true;
  if (s_given_up) return false;

  ++s_retries;
  if (s_retries > 3) {
    s_given_up = true;
    Serial.println("[TOUCH] HI8561 giving up after 3 attempts — booting without touch");
    return false;
  }

  // Wire is up on 7/8 (xl9535.begin). Bump to 400 kHz + short timeout so a stuck
  // controller can't stall the UI loop.
  Wire.setClock(400000);
  Wire.setTimeOut(20);
  delay(10);

  // Bring-up: scan the I2C bus once so we can see exactly what touch controller is present
  // (HI8561=0x68, GT9895=0x5D/0x14, others) — the RM69A10 variant may pair with GT9895, not HI8561.
  if (s_retries == 1) {
    char sc[96]; int n = 0; n += snprintf(sc, sizeof sc, "[TOUCH] i2c7/8 scan:");
    for (uint8_t a = 0x08; a < 0x78 && n < (int)sizeof sc - 6; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) n += snprintf(sc + n, sizeof sc - n, " %02X", a);
    }
    Serial.println(sc);
  }

  Wire.beginTransmission(GT9895_ADDR);
  if (Wire.endTransmission() != 0) {
    snprintf(s_scan_str, sizeof s_scan_str, "i2c7/8: no ACK at 0x%02X", GT9895_ADDR);
    Serial.printf("[TOUCH] GT9895 not found (retry %d/3)\n", s_retries);
    return false;
  }

  s_init_ok = true;
  snprintf(s_scan_str, sizeof s_scan_str, "0x%02X gt9895 OK (retry %d)", GT9895_ADDR, s_retries - 1);
  Serial.printf("[TOUCH] %s\n", s_scan_str);
  return true;
}

// ---- Coordinate mapping (identity in portrait; rotation kept for parity) ----

static void mapRaw(uint16_t rx, uint16_t ry, uint16_t* ox, uint16_t* oy) {
  int sx, sy;
  switch (s_point_rotation) {
    case 1:  sx = (RAW_H - 1) - (int)ry; sy = (int)rx;                 break;  // ROT_90
    case 3:  sx = (int)ry;               sy = (RAW_W - 1) - (int)rx;   break;  // ROT_270
    default: sx = (int)rx;               sy = (int)ry;                 break;  // portrait / 180
  }
  if (sx < 0) sx = 0; if (sx >= SCR_W) sx = SCR_W - 1;
  if (sy < 0) sy = 0; if (sy >= SCR_H) sy = SCR_H - 1;
  *ox = (uint16_t)sx;
  *oy = (uint16_t)sy;
}

// ---- Physical report read ------------------------------------------------
// Returns 1 = finger down (rx/ry set to raw panel coords), 0 = no touch, -1 = I2C error.
//
// GT9895 single-touch read: 16 bytes = TOUCH_POINT_ADDRESS_OFFSET(8) + 1*SINGLE_TOUCH_POINT_DATA_SIZE(8).
//   buf[2] = finger count; point data at offset 8: [id/status][?][x_lo][x_hi][y_lo][y_hi][pressure][?]
//   (x/y little-endian; scale factor 1.0 => already panel coords).
// TODO(device): verify X/Y axis orientation + origin against on-screen taps (may need flip/swap in mapRaw).
static int hi8561ReadPoint(uint16_t* rx, uint16_t* ry) {
  uint8_t buf[16] = {0};
  if (!gt9895WriteRead(GT9895_TOUCH_CMD, 4, buf, sizeof buf)) return -1;
  uint8_t fingers = buf[2];
  if (fingers < 1 || fingers > 10) return 0;                 // no finger down
  uint32_t x = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8);
  uint32_t y = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8);
  // Scale the native 1060x2400 grid down to the 568x1232 panel.
  *rx = (uint16_t)(x * (uint32_t)SCR_W / GT9895_NATIVE_W);
  *ry = (uint16_t)(y * (uint32_t)SCR_H / GT9895_NATIVE_H);
  return 1;
}

static void hi8561Poll() {
  uint16_t rx = 0, ry = 0;
  if (hi8561ReadPoint(&rx, &ry) == 1) {
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

// ---- Gesture state machine (shared logic; mirrors RakTapV2Touch) ----

int heltecV4CapTouchCheck() {
  if (!s_init_ok) return BUTTON_EVENT_NONE;
  hi8561Poll();

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
      if (adx >= P4_TOUCH_SWIPE_MIN && adx > ady) s_swiping_now = true;
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
    if (adx >= P4_TOUCH_SWIPE_MIN && adx > (ady + 8)) {
      bool left = ((int)s_last_x - (int)s_start_x) < 0;
      s_swipe_x = left ? -1 : 1;
      s_swipe_y = 0;
      s_swipe_pending = true;
      return left ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (ady >= P4_TOUCH_SWIPE_MIN && ady > (adx + 8)) {
      s_swipe_x = 0;
      s_swipe_y = ((int)s_last_y - (int)s_start_y) < 0 ? -1 : 1;
      s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    if (dur >= 12 && dur < (unsigned long)P4_TOUCH_LONG_MS &&
        adx <= P4_TOUCH_TAP_MOVE_MAX && ady <= P4_TOUCH_TAP_MOVE_MAX) {
      s_tap_x = s_last_x;
      s_tap_y = s_last_y;
      s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
    }
    if (dur >= (unsigned long)P4_TOUCH_LONG_MS) {
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
  BaseType_t ok = xTaskCreatePinnedToCore(touchPollTask, "hi8561_touch", 3072,
                                          nullptr, 2, &s_poll_task, 0);
  if (ok == pdPASS) { s_async = true; return true; }
  return false;
}
bool heltecV4CapTouchIsAsyncPolling() { return s_async; }
bool heltecV4CapTouchIsSwiping() { return s_swiping_now; }
void heltecV4CapTouchSetRotation(uint8_t r) { s_ui_rotation = r & 3; }
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r & 3; }
const char* heltecV4CapTouchDebug() { return s_scan_str; }

#endif  // HAS_HI8561_TOUCH && ESP32
