// LilyGo T-Deck touch input — implements the shared touch-input API
// (declared in HeltecV4CapTouch.h) for the T-Deck's GT911 capacitive
// controller on the I2C bus (SDA 18 / SCL 8, addr 0x5D).
//
// The T-Deck UI is fixed landscape 320x240 (see UITask::begin), and LVGL
// renders natively at that resolution (hardware panel rotation), so this
// driver outputs coordinates already in the 320x240 landscape screen space.
// The raw->screen mapping is calibrated against the panel; flip the TDECK_TS_*
// switches below if taps land mirrored/rotated.
#if defined(HAS_TOUCH_UI) && !defined(HAS_HELTEC_V4_CAP_TOUCH) && !defined(HAS_RAK_TAP_V2) && defined(ESP32)

#include "HeltecV4CapTouch.h"
#include "TDeckKeyboard.h"                 // shares this I2C bus; polled from the touch task
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>   // BUTTON_EVENT_*

#ifndef PIN_TOUCH_SDA
  #define PIN_TOUCH_SDA 18
#endif
#ifndef PIN_TOUCH_SCL
  #define PIN_TOUCH_SCL 8
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 16
#endif

// Landscape screen size the UI renders at.
static const int SCR_W = 320;
static const int SCR_H = 240;

// --- Calibration switches (adjust if taps land wrong) ---------------------
// GT911 raw coordinate range and how it maps to the 320x240 landscape screen.
#ifndef TDECK_TS_SWAP_XY
  #define TDECK_TS_SWAP_XY 0   // 1: swap raw x/y before mapping
#endif
#ifndef TDECK_TS_FLIP_X
  #define TDECK_TS_FLIP_X 0    // 1: mirror horizontally
#endif
#ifndef TDECK_TS_FLIP_Y
  #define TDECK_TS_FLIP_Y 0    // 1: mirror vertically
#endif

static const uint8_t GT911_ADDRS[] = { 0x5D, 0x14 };
static uint8_t  s_addr = 0;
static bool     s_init_ok = false;
static char     s_scan_str[160] = "scan: (not run)";

// Last RAW GT911 point (pre-mapping) — exposed for on-screen calibration.
static volatile uint16_t s_dbg_rawx = 0, s_dbg_rawy = 0;

// Gesture / state machine (mirrors the Heltec driver's contract).
static bool     s_have_touch = false;        // latest GT911 pressed state
static uint16_t s_cur_x = 0, s_cur_y = 0;    // latest mapped point
static bool     s_down = false;              // gesture in progress
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

static uint8_t  s_ui_rotation = 0;     // stored; the T-Deck is fixed landscape
static uint8_t  s_point_rotation = 0;  // so these are informational only

#ifndef TDECK_TOUCH_SWIPE_MIN
  #define TDECK_TOUCH_SWIPE_MIN 40
#endif
#ifndef TDECK_TOUCH_TAP_MOVE_MAX
  #define TDECK_TOUCH_TAP_MOVE_MAX 16
#endif
#ifndef TDECK_TOUCH_LONG_MS
  #define TDECK_TOUCH_LONG_MS 1000
#endif

static bool gt911ReadReg(uint16_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(s_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((int)s_addr, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
  return true;
}

static void gt911WriteReg(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(s_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(val);
  Wire.endTransmission();
}

// Map a raw GT911 point to the 320x240 landscape screen space.
//
// The GT911 reports in its native PORTRAIT frame (configured max 240 x 320,
// see the scan diag). The UI runs hardware-landscape 320 x 240 (ST7789 panel
// rotation 270), so the axes are swapped: portrait-Y (0..320) maps to screen-X
// (0..320), and portrait-X (0..240) maps to screen-Y (0..240) mirrored. This
// matches LilyGo's own T-Deck landscape touch calibration (swapXY + mirrorY)
// and was confirmed against on-screen raw readings. Flip TDECK_TS_FLIP_X /
// TDECK_TS_FLIP_Y if a future panel revision lands a corner inverted.
static void mapRaw(uint16_t rx, uint16_t ry, uint16_t* ox, uint16_t* oy) {
  int sx = (int)ry;                 // portrait-Y -> screen-X
  int sy = (SCR_H - 1) - (int)rx;   // portrait-X -> screen-Y (mirrored)
#if TDECK_TS_SWAP_XY
  { int t = sx; sx = sy; sy = t; }
#endif
#if TDECK_TS_FLIP_X
  sx = (SCR_W - 1) - sx;
#endif
#if TDECK_TS_FLIP_Y
  sy = (SCR_H - 1) - sy;
#endif
  if (sx < 0) sx = 0; if (sx >= SCR_W) sx = SCR_W - 1;
  if (sy < 0) sy = 0; if (sy >= SCR_H) sy = SCR_H - 1;
  *ox = (uint16_t)sx;
  *oy = (uint16_t)sy;
}

// Poll the GT911 once, updating s_have_touch / s_cur_x/y from the latest frame.
static void gt911Poll() {
  uint8_t status;
  if (!gt911ReadReg(0x814E, &status, 1)) return;
  if (!(status & 0x80)) return;                 // no new data — keep prev state
  uint8_t n = status & 0x0F;
  if (n > 0) {
    uint8_t p[8];
    if (gt911ReadReg(0x8150, p, 8)) {
      // GT911 point record at 0x8150: [0]=X low, [1]=X high, [2]=Y low,
      // [3]=Y high, [4..5]=size, [6]=reserved. (The track-id byte lives at
      // 0x814F, BEFORE this record — decoding from p[1] shifted every field one
      // byte and produced wildly out-of-range coordinates.)
      uint16_t rx = (uint16_t)(p[0] | (p[1] << 8));
      uint16_t ry = (uint16_t)(p[2] | (p[3] << 8));
      s_dbg_rawx = rx;
      s_dbg_rawy = ry;
      mapRaw(rx, ry, &s_cur_x, &s_cur_y);
      s_have_touch = true;
    }
  } else {
    s_have_touch = false;                        // finger lifted
  }
  gt911WriteReg(0x814E, 0);                       // ack the frame
}

bool heltecV4CapTouchBegin() {
  // ONE-SHOT: the UI retries this every loop while touch isn't inited, so the
  // I2C probe/scan must run only once — otherwise we re-scan the bus every
  // frame and the whole device crawls. After the first attempt, return the
  // cached result instantly with no bus traffic.
  static bool s_attempted = false;
  if (s_attempted) return s_init_ok;
  s_attempted = true;

  // --- Peripheral power: the keyboard / GT911 / I2C pull-ups all hang off the
  // BOARD_POWERON rail (GPIO10). It's driven HIGH in TDeckBoard::begin(), but
  // re-assert + settle here so the bus is definitely powered when we probe — a
  // fully silent bus (keyboard AND touch both absent) is almost always this
  // rail not reaching the devices. ---
#ifdef PIN_PERF_POWERON
  pinMode(PIN_PERF_POWERON, OUTPUT);
  digitalWrite(PIN_PERF_POWERON, HIGH);
  delay(120);
#endif

  // Sample idle bus levels with the I2C peripheral detached (plain INPUT, no
  // internal pull). With the rail powered and external pull-ups present both
  // read HIGH; a LOW/floating line means no power/pull-up reaches the bus, which
  // is exactly why nothing ACKs. Reported in the scan string as L<sda><scl>.
  pinMode(PIN_TOUCH_SDA, INPUT);
  pinMode(PIN_TOUCH_SCL, INPUT);
  delayMicroseconds(50);
  int lvl_sda = digitalRead(PIN_TOUCH_SDA);
  int lvl_scl = digitalRead(PIN_TOUCH_SCL);

  // I2C bus recovery: if a peripheral is mid-byte and holding SDA low, clock SCL
  // up to 9 times so it can finish, then leave the lines idle-high.
  pinMode(PIN_TOUCH_SCL, OUTPUT);
  digitalWrite(PIN_TOUCH_SCL, HIGH);
  pinMode(PIN_TOUCH_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9 && digitalRead(PIN_TOUCH_SDA) == LOW; ++i) {
    digitalWrite(PIN_TOUCH_SCL, LOW);  delayMicroseconds(6);
    digitalWrite(PIN_TOUCH_SCL, HIGH); delayMicroseconds(6);
  }

  // Force a clean re-route onto 18/8. ESP32 Arduino's Wire was already begun on
  // DEFAULT pins in ESP32Board::begin(); a second begin() with new pins does not
  // always re-assign them, so end() first guarantees the pins actually move.
  Wire.end();
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  Wire.setClock(400000);
  // CRITICAL: bound the I2C timeout so a non-responding controller fails fast
  // (ESP32 Arduino's default is ~1 s — at that, polling stalls everything).
  Wire.setTimeOut(20);
#if PIN_TOUCH_INT >= 0
  pinMode(PIN_TOUCH_INT, INPUT);
#endif
  delay(20);

  // Probe ONLY the expected T-Deck devices — keyboard 0x55, GT911 touch 0x5D /
  // 0x14. A full 1..0x77 sweep risked dozens of slow timeouts on a floating or
  // wedged bus (the ESP32 Wire timeout isn't always honoured on the bus-busy
  // wait), which blocks this call long enough to look like a freeze. Three
  // targeted probes can't stall for long, and still tell us what's answering.
  static const uint8_t kProbe[] = { 0x55, 0x5D, 0x14 };
  int found = 0;
  int o = snprintf(s_scan_str, sizeof s_scan_str, "i2c18/8 L%d%d:", lvl_sda, lvl_scl);
  for (uint8_t i = 0; i < sizeof(kProbe); ++i) {
    Wire.beginTransmission(kProbe[i]);
    if (Wire.endTransmission() == 0) {
      o += snprintf(s_scan_str + o, sizeof s_scan_str - o, " %02X", kProbe[i]);
      ++found;
      for (uint8_t g = 0; g < sizeof(GT911_ADDRS); ++g)
        if (kProbe[i] == GT911_ADDRS[g]) { s_addr = kProbe[i]; s_init_ok = true; }
    }
  }
  if (found == 0) o += snprintf(s_scan_str + o, sizeof s_scan_str - o, " none");
  // Targeted GT911 confirmation: read the product-ID register (0x8140 -> ascii
  // "911\0") so a real GT911 is distinguishable from an unrelated ACK, plus the
  // configured output resolution (0x8048 X-max, 0x804A Y-max) which tells us the
  // raw coordinate range to calibrate the landscape mapping against.
  if (s_init_ok) {
    uint8_t pid[4] = {0};
    if (gt911ReadReg(0x8140, pid, 4) && o < (int)sizeof(s_scan_str) - 14)
      o += snprintf(s_scan_str + o, sizeof s_scan_str - o,
                    " gt@%02X:%c%c%c", s_addr,
                    pid[0] ? pid[0] : '?', pid[1] ? pid[1] : '?',
                    pid[2] ? pid[2] : '?');
    uint8_t res[4] = {0};
    if (gt911ReadReg(0x8048, res, 4) && o < (int)sizeof(s_scan_str) - 14) {
      uint16_t xmax = (uint16_t)(res[0] | (res[1] << 8));
      uint16_t ymax = (uint16_t)(res[2] | (res[3] << 8));
      o += snprintf(s_scan_str + o, sizeof s_scan_str - o, " max%ux%u", xmax, ymax);
    }
  }
  return s_init_ok;
}

// Last RAW GT911 point, before any landscape mapping — for on-screen touch
// calibration. (x,y) are in the GT911's native config frame.
void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = s_dbg_rawx;
  if (ry) *ry = s_dbg_rawy;
}

int heltecV4CapTouchCheck() {
  if (!s_init_ok) return BUTTON_EVENT_NONE;
  gt911Poll();

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
    // Horizontal-swipe-in-progress flag (so LVGL can abort a click that turns
    // into a side-swipe). Vertical drags stay scroll.
    if (!s_swiping_now) {
      int dx = (int)s_last_x - (int)s_start_x;
      int dy = (int)s_last_y - (int)s_start_y;
      int adx = dx < 0 ? -dx : dx;
      int ady = dy < 0 ? -dy : dy;
      if (adx >= TDECK_TOUCH_SWIPE_MIN && adx > ady) s_swiping_now = true;
    }
    return BUTTON_EVENT_NONE;
  }

  // Not touching.
  if (s_down) {
    s_down = false;
    s_live = false;
    s_swiping_now = false;
    unsigned long dur = millis() - s_down_at;
    int dx = (int)s_last_x - (int)s_start_x;
    int dy = (int)s_last_y - (int)s_start_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx >= TDECK_TOUCH_SWIPE_MIN && adx > (ady + 8)) {
      bool left = dx < 0;
      s_swipe_x = left ? -1 : 1;
      s_swipe_y = 0;
      s_swipe_pending = true;
      return left ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (ady >= TDECK_TOUCH_SWIPE_MIN && ady > (adx + 8)) {
      s_swipe_x = 0;
      s_swipe_y = (dy < 0) ? -1 : 1;
      s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    if (dur >= 12 && dur < (unsigned long)TDECK_TOUCH_LONG_MS &&
        adx <= TDECK_TOUCH_TAP_MOVE_MAX && ady <= TDECK_TOUCH_TAP_MOVE_MAX) {
      s_tap_x = s_last_x;
      s_tap_y = s_last_y;
      s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
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

// Background poll on core 0 so the GT911 I2C round-trips never stall the LVGL
// render / mesh loop on core 1 (exactly why the V4 driver runs async too).
static TaskHandle_t s_poll_task = nullptr;
static volatile bool s_async = false;
static uint32_t s_period_ms = 8;

static void touchPollTask(void* arg) {
  (void)arg;
#if defined(HAS_TDECK_KEYBOARD)
  tdeckKeyboardBegin();
  uint8_t kb_div = 0;
#endif
  for (;;) {
    heltecV4CapTouchCheck();
#if defined(HAS_TDECK_KEYBOARD)
    // Poll the keyboard from THIS task too (same I2C bus) so the two devices
    // never get hit from two cores at once. ~every 4th tick (~32 ms) is plenty.
    tdeckKeyboardFlushBacklight();   // backlight requests land within ~one tick (~8 ms); key scan stays /4
    if (++kb_div >= 4) { kb_div = 0; tdeckKeyboardPoll(); }
#endif
    vTaskDelay(pdMS_TO_TICKS(s_period_ms));
  }
}

bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (s_async || !s_init_ok) return false;
  s_period_ms = period_ms < 4 ? 4 : (period_ms > 100 ? 100 : period_ms);
  BaseType_t ok = xTaskCreatePinnedToCore(touchPollTask, "tdeck_touch", 3072,
                                          nullptr, 2, &s_poll_task, 0);
  if (ok == pdPASS) { s_async = true; return true; }
  return false;
}
bool heltecV4CapTouchIsAsyncPolling() { return s_async; }
bool heltecV4CapTouchIsSwiping() { return s_swiping_now; }
void heltecV4CapTouchSetRotation(uint8_t r) { s_ui_rotation = r & 3; }
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r & 3; }

// Human-readable I2C bus-scan result (built once in heltecV4CapTouchBegin).
// Surfaced on the on-screen diag overlay so GT911 bring-up is debuggable
// without a serial console.
const char* heltecV4CapTouchDebug() { return s_scan_str; }

#endif
