// SPDX-License-Identifier: GPL-3.0-or-later
// Elecrow CrowPanel Advanced 10.1" ESP32-P4: GT911 capacitive touch, implementing the shared
// touch-input API (HeltecV4CapTouch.h).
//
// Wiring (schematic V1.0/V1.1/V1.2, all identical, and Elecrow's Lesson05 BSP): GT911 on the touch
// FPC, I2C0 SDA=GPIO45 / SCL=GPIO46 (shared with the ES8311 codec and the HY2.0 I2C port through
// BSS138 level shifters), RST=GPIO40, INT=GPIO42. ESP32Board::begin starts Wire there, and every
// init attempt below checks the bus is still up and begins it again if it is not.
//
// Why it checks: on beta_97 the bus was begun at boot and then torn down before touch ever ran.
// The generic esp32p4 Arduino variant defines BOARD_SDMMC_POWER_PIN 45 (the Espressif Function EV
// board's SD power switch), so every SD_MMC.begin() did pinMode(45, OUTPUT): the peripheral manager
// detached I2C0 (i2cDeinit) and SDA was then driven as a GPIO. build.sh now compiles SD_MMC without
// that block on this board (WADA_SDMMC_NO_POWER_PIN); the check here keeps touch alive whatever
// else might claim the pins later, and names the owner in the log if it happens.
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
#include "esp32-hal-i2c.h"      // i2cIsInit(): is the Arduino I2C0 master actually up?
#include "esp32-hal-periman.h"  // who owns a pin, for the failure log

#ifndef PIN_TOUCH_RST
  #define PIN_TOUCH_RST 40
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 42
#endif
#ifndef PIN_TOUCH_SDA
  #define PIN_TOUCH_SDA 45
#endif
#ifndef PIN_TOUCH_SCL
  #define PIN_TOUCH_SCL 46
#endif

static const int SCR_W = 1024;
static const int SCR_H = 600;

static uint8_t  s_addr = 0;              // 0x5D or 0x14, whichever answered
static uint16_t s_native_w = SCR_W;      // GT911 config X/Y maximum (read at init)
static uint16_t s_native_h = SCR_H;

static bool     s_init_ok  = false;
static bool     s_given_up = false;
static int      s_retries  = 0;
static uint32_t s_last_try_ms = 0;
static bool     s_bus_ever_up = false;
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

// Pad power for the touch lines. GPIO39-46 (GT911 SDA45/SCL46, RST40, INT42) sit in the VDDPST_5
// domain that the P4's on-chip LDO4 feeds (net ESP32_LDO4 via R109, schematic V1.0/V1.1/V1.2), and the
// SDA/SCL/RST/INT pull-ups (R97/R98/R74/R75) hang off that same rail. crowpanelP4PowerOn() acquires it
// first thing in app_main (target.cpp); without it the bus floats low and nothing can ACK.
extern bool crowpanelP4PadPowerOk();

// Bring up (or re-attach) the Arduino I2C0 master on SDA45/SCL46 and SAY whether it worked. Every
// attempt runs this first, so a bus that something else released in between is begun again rather
// than silently failing every transaction with "bus is not initialized".
static bool gtBusBegin(int attempt) {
  const uint32_t hz = 400000;   // Elecrow's bsp_i2c.c runs the GT911 at 400 kHz, internal pull-ups on
  const bool was_up = i2cIsInit(0);
  bool ok = was_up;
  if (was_up) {
    ok = Wire.setClock(hz);
  } else {
    if (attempt > 1 || s_bus_ever_up) {
      // It was up before (ESP32Board::begin) and is not now: some driver re-claimed a pin. Name the owner.
      Serial.printf("[TOUCH] i2c bus 0 was released (GPIO%d now %s, GPIO%d now %s); beginning it again\n",
                    PIN_TOUCH_SDA, perimanGetTypeName(perimanGetPinBusType(PIN_TOUCH_SDA)),
                    PIN_TOUCH_SCL, perimanGetTypeName(perimanGetPinBusType(PIN_TOUCH_SCL)));
    }
    ok = Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, hz);
  }
  if (ok) {
    s_bus_ever_up = true;
    Wire.setTimeOut(20);
    Serial.printf("[TOUCH] i2c bus 0 on SDA%d/SCL%d @ %u kHz: ok (%s)\n", PIN_TOUCH_SDA, PIN_TOUCH_SCL,
                  (unsigned)(hz / 1000), was_up ? "already up" : "begun now");
  } else {
    Serial.printf("[TOUCH] i2c bus 0 on SDA%d/SCL%d @ %u kHz: FAIL (%s; SDA owner %s, SCL owner %s)\n",
                  PIN_TOUCH_SDA, PIN_TOUCH_SCL, (unsigned)(hz / 1000),
                  was_up ? "setClock refused" : "Wire.begin refused",
                  perimanGetTypeName(perimanGetPinBusType(PIN_TOUCH_SDA)),
                  perimanGetTypeName(perimanGetPinBusType(PIN_TOUCH_SCL)));
  }
  return ok;
}

// Hardware reset with the GT911's address strap (Goodix GT911 datasheet, power-on timing):
//   RST low and INT at the strap level for > 100 us, RST high while INT still holds the level,
//   keep it > 5 ms so the address latches, then (0x5D) drive INT low for 50 ms, release INT to an
//   input and allow > 50 ms before the first I2C access. The datasheet's 50 ms is not enough on
//   this panel: an owner found its GT911 needs 100-300 ms before the first I2C handshake (Discord),
//   so every attempt waits CP_TOUCH_SETTLE_MS after the reset.
// INT low at the RST edge selects 0x5D, INT high selects 0x14. Attempt 1 straps 0x5D (Elecrow's
// MicroPython driver does exactly this), attempt 2 straps 0x14, attempt 3 only pulses RST and leaves
// INT to its pull-up R75 (what Elecrow's ESP-IDF BSP effectively does). Whichever address then ACKs
// is used, so a strap that did not take is harmless.
#ifndef CP_TOUCH_SETTLE_MS
  #define CP_TOUCH_SETTLE_MS 300   // reset released -> first I2C access (see above)
#endif
static const char* gtHardReset(int attempt) {
  const bool strap_14 = (attempt == 2);
  const bool strap    = (attempt != 3);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  if (strap) {
    pinMode(PIN_TOUCH_INT, OUTPUT);
    digitalWrite(PIN_TOUCH_INT, strap_14 ? HIGH : LOW);
  } else {
    pinMode(PIN_TOUCH_INT, INPUT);
  }
  delay(11);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(6);
  if (strap) {
    digitalWrite(PIN_TOUCH_INT, LOW);
    delay(50);
  }
  pinMode(PIN_TOUCH_INT, INPUT);
  delay(CP_TOUCH_SETTLE_MS);
  return strap ? (strap_14 ? "INT high (0x14)" : "INT low (0x5D)") : "RST only, INT on its pull-up";
}

// Minimum spacing between attempts: UITask retries every loop, which would burn all three in a few
// milliseconds while the controller is still booting.
#ifndef CP_TOUCH_RETRY_MS
  #define CP_TOUCH_RETRY_MS 700
#endif

bool heltecV4CapTouchBegin() {
  if (s_init_ok)  return true;
  if (s_given_up) return false;
  const uint32_t now = millis();
  if (s_last_try_ms && (uint32_t)(now - s_last_try_ms) < CP_TOUCH_RETRY_MS) return false;
  s_last_try_ms = now ? now : 1;
  if (++s_retries > 3) {
    s_given_up = true;
    Serial.println("[TOUCH] GT911 giving up after 3 attempts, booting without touch");
    return false;
  }
  const int attempt = s_retries;

  // 1. Power: LDO4 (pad domain of GPIO39-46 and the touch pull-ups).
  const bool pwr = crowpanelP4PadPowerOk();
  Serial.printf("[TOUCH] attempt %d/3: pad power LDO4 3.3 V %s\n", attempt, pwr ? "held" : "NOT held (touch lines unpowered)");

  // 2. Bus.
  if (!gtBusBegin(attempt)) {
    snprintf(s_scan_str, sizeof s_scan_str, "i2c0 45/46: bus begin failed");
    return false;
  }

  // 3. Reset + address strap.
  const char* strap = gtHardReset(attempt);

  // 4. Scan: every address that ACKs, so a tester log shows exactly what lives on 45/46
  //    (GT911 0x5D/0x14, ES8311 codec 0x18, anything on the HY2.0 port).
  char sc[112];
  int n = snprintf(sc, sizeof sc, "[TOUCH] i2c0 45/46 scan after %s:", strap);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    if (!probe(a)) continue;
    found++;
    if (n < (int)sizeof sc - 6) n += snprintf(sc + n, sizeof sc - n, " %02X", a);
  }
  if (!found) snprintf(sc + n, sizeof sc - n, " (nothing ACKed)");
  Serial.println(sc);

  // 5. Pick the GT911 address.
  s_addr = probe(0x5D) ? 0x5D : (probe(0x14) ? 0x14 : 0);
  if (!s_addr) {
    snprintf(s_scan_str, sizeof s_scan_str, "i2c0 45/46: %d device(s), no GT911 at 0x5D/0x14", found);
    Serial.printf("[TOUCH] GT911 not found at 0x5D or 0x14 (attempt %d/3)\n", attempt);
    return false;
  }

  // 6. Identify: product ID "911" at 0x8140..0x8143 (ASCII), firmware version 0x8144..0x8145 (LE),
  //    config version 0x8047, X/Y output maximum 0x8048..0x804B (LE), touch count 0x804C.
  uint8_t id[4] = {0}, fw[2] = {0}, cfg[6] = {0};
  const bool id_ok  = gtRead(0x8140, id, 4) && gtRead(0x8144, fw, 2);
  const bool cfg_ok = gtRead(0x8047, cfg, 6);
  if (cfg_ok) {
    const uint16_t xm = (uint16_t)(cfg[1] | (cfg[2] << 8));
    const uint16_t ym = (uint16_t)(cfg[3] | (cfg[4] << 8));
    if (xm >= 100 && xm <= 4096 && ym >= 100 && ym <= 4096) { s_native_w = xm; s_native_h = ym; }
  }
  char pid[5];
  for (int i = 0; i < 4; i++) pid[i] = (id[i] >= 0x20 && id[i] < 0x7F) ? (char)id[i] : '\0';
  pid[4] = '\0';
  if (!id_ok) {
    Serial.printf("[TOUCH] 0x%02X ACKs but the GT911 ID read failed (attempt %d/3)\n", s_addr, attempt);
    snprintf(s_scan_str, sizeof s_scan_str, "0x%02X ACK, GT911 ID read failed", s_addr);
    return false;
  }
  Serial.printf("[TOUCH] GT911 at 0x%02X: product '%s' fw 0x%04X, config v0x%02X%s %ux%u, %u touch points\n",
                s_addr, pid[0] ? pid : "?", (unsigned)(fw[0] | (fw[1] << 8)), (unsigned)cfg[0],
                cfg_ok ? "" : " (config read failed, assuming)", (unsigned)s_native_w, (unsigned)s_native_h,
                (unsigned)(cfg_ok ? (cfg[5] & 0x0F) : 0));
  gtWrite8(0x814E, 0);   // clear any stale "buffer ready"

  s_init_ok = true;
  snprintf(s_scan_str, sizeof s_scan_str, "0x%02X gt911 '%s' fw %04X %ux%u", s_addr, pid[0] ? pid : "?",
           (unsigned)(fw[0] | (fw[1] << 8)), (unsigned)s_native_w, (unsigned)s_native_h);
  return true;
}

// Scale to the panel, then turn the point into the frame LVGL shows (#701): s_point_rotation
// is the LVGL software rotation, the same maps as the T-Display P4's mapRaw (lvglTouchRead in
// UITask undoes them for LVGL). Swipe detection and the direct readers work in this frame.
static void mapRaw(uint16_t rx, uint16_t ry, uint16_t* ox, uint16_t* oy) {
  int px = (int)rx, py = (int)ry;
  if (s_native_w != SCR_W) px = px * SCR_W / s_native_w;
  if (s_native_h != SCR_H) py = py * SCR_H / s_native_h;
  if (px < 0) px = 0; if (px > SCR_W - 1) px = SCR_W - 1;
  if (py < 0) py = 0; if (py > SCR_H - 1) py = SCR_H - 1;
  int sx, sy;
  switch (s_point_rotation) {
    case 1:  sx = (SCR_H - 1) - py; sy = px;               break;  // ROT_90 (600x1024)
    case 2:  sx = (SCR_W - 1) - px; sy = (SCR_H - 1) - py; break;  // ROT_180
    case 3:  sx = py;               sy = (SCR_W - 1) - px; break;  // ROT_270 (600x1024)
    default: sx = px;               sy = py;               break;  // native landscape
  }
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
