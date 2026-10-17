// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_SENSECAP_INDICATOR) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/input/HeltecV4CapTouch.h>
#include <helpers/ui/MomentaryButton.h>

#include "IndicatorExpander.h"
#include "IndicatorIo.h"

// FT6336 on the shared Wire bus (SDA39/SCL40), address 0x48. Registers used:
//   0x02 TD_STATUS (low nibble = touch count), followed by P1_XH/XL/YH/YL (0x03..0x06)
//   0xA3 ID_G_CIPHER (chip id, 0x64 for FT6336U), 0xA8 G_FOCALTECH_ID (vendor id)
// Every transaction holds IndicatorIo::BusLock across register write + read.

namespace {
constexpr uint8_t kAddr = 0x48;
constexpr uint8_t kRegStatus = 0x02;
constexpr uint8_t kRegChipId = 0xA3;
constexpr uint8_t kRegVendorId = 0xA8;
constexpr int kMax = 479;
// UITask calls begin on every loop pass until it succeeds. Only the first call
// pulses reset and waits for the controller; later calls re-probe at most this often.
constexpr uint32_t kReprobeMs = 2000;

bool s_ready = false;
bool s_down = false;
bool s_live = false;
bool s_tapPending = false;
bool s_swipePending = false;
bool s_swiping = false;
uint16_t s_x = 0, s_y = 0, s_startX = 0, s_startY = 0, s_tapX = 0, s_tapY = 0;
uint16_t s_rawX = 0, s_rawY = 0;
int8_t s_swipeX = 0, s_swipeY = 0;
uint32_t s_downAt = 0;
TaskHandle_t s_task = nullptr;
bool s_async = false;
uint32_t s_periodMs = 8;
char s_debug[48] = "SenseCAP Indicator FT6336";

// Reads `len` bytes starting at `reg`. One BusLock scope covers write + read.
bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  IndicatorIo::BusLock lock;
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((int)kAddr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

int clampCoord(int v) { return v < 0 ? 0 : (v > kMax ? kMax : v); }

bool readPoint(uint16_t& x, uint16_t& y) {
  uint8_t b[5];
  if (!readRegs(kRegStatus, b, sizeof(b))) return false;
  const uint8_t n = b[0] & 0x0F;
  if (n == 0 || n > 2) return false;
  const uint16_t rx = ((b[1] & 0x0F) << 8) | b[2];
  const uint16_t ry = ((b[3] & 0x0F) << 8) | b[4];
  s_rawX = rx; s_rawY = ry;
  x = (uint16_t)clampCoord(kMax - (int)rx);
  y = (uint16_t)clampCoord(kMax - (int)ry);
  return true;
}

void poll() {
  uint16_t x = 0, y = 0;
  const bool touched = readPoint(x, y);
  if (touched) {
    s_x = x; s_y = y; s_live = true;
    if (!s_down) { s_down = true; s_startX = x; s_startY = y; s_downAt = millis(); s_swiping = false; }
    const int dx = (int)x - s_startX, dy = (int)y - s_startY;
    const int adx = abs(dx), ady = abs(dy);
    if (!s_swiping && adx >= 40 && adx > ady) s_swiping = true;
    return;
  }
  s_live = false;
  if (!s_down) return;
  s_down = false;
  const int dx = (int)s_x - s_startX, dy = (int)s_y - s_startY;
  const int adx = abs(dx), ady = abs(dy);
  s_swiping = false;
  if (adx >= 40 && adx > ady + 8) {
    s_swipeX = dx < 0 ? -1 : 1; s_swipeY = 0; s_swipePending = true;
  } else if (ady >= 40 && ady > adx + 8) {
    s_swipeX = 0; s_swipeY = dy < 0 ? -1 : 1; s_swipePending = true;
  } else if ((uint32_t)(millis() - s_downAt) >= 12 && adx <= 16 && ady <= 16) {
    s_tapX = s_x; s_tapY = s_y; s_tapPending = true;
  }
}

void pollTask(void*) {
  for (;;) { heltecV4CapTouchCheck(); vTaskDelay(pdMS_TO_TICKS(s_periodMs)); }
}

void setReset(bool high) {
  IndicatorIo::BusLock lock;
  IndicatorIo::chip().setOutput(indicator::pins::kTouchReset, high);
}
}  // namespace

bool heltecV4CapTouchBegin() {
  static bool s_resetDone = false;
  static bool s_failLogged = false;
  static uint32_t s_lastProbe = 0;
  if (s_ready) return true;
  if (!s_resetDone) {
    // Reset pulse; the bus lock is not held across the sleeps.
    s_resetDone = true;
    setReset(false);
    delay(10);
    setReset(true);
    delay(300);
  } else if ((uint32_t)(millis() - s_lastProbe) < kReprobeMs) {
    return false;
  }
  s_lastProbe = millis();

  uint8_t id = 0, vendor = 0;
  if (!readRegs(kRegChipId, &id, 1)) {
    snprintf(s_debug, sizeof(s_debug), "SenseCAP Indicator FT6336 (no ack)");
    if (!s_failLogged) {
      s_failLogged = true;
      Serial.println("[touch] FT6336 not responding at 0x48, re-probing every 2 s");
    }
    return false;
  }
  const bool haveVendor = readRegs(kRegVendorId, &vendor, 1);
  snprintf(s_debug, sizeof(s_debug), "SenseCAP Indicator FT6336 id=0x%02X", id);
  Serial.printf("[touch] FT6336 chip id 0x%02X (expect 0x64), vendor 0x%02X\n", id, haveVendor ? vendor : 0);
  s_ready = true;
  return true;
}

int heltecV4CapTouchCheck() { if (!s_ready) return BUTTON_EVENT_NONE; poll(); return BUTTON_EVENT_NONE; }
bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  if (!s_tapPending) return false; s_tapPending = false;
  if (x) *x = s_tapX;
  if (y) *y = s_tapY;
  return true;
}
bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (!s_live) return false;
  if (x) *x = s_x;
  if (y) *y = s_y;
  return true;
}
bool heltecV4CapTouchPopSwipe(int8_t* x, int8_t* y) {
  if (!s_swipePending) return false; s_swipePending = false;
  if (x) *x = s_swipeX;
  if (y) *y = s_swipeY;
  return true;
}
bool heltecV4CapTouchStartBackgroundPoll(uint32_t periodMs) {
  if (s_async || !s_ready) return false;
  s_periodMs = periodMs < 4 ? 4 : (periodMs > 100 ? 100 : periodMs);
  if (xTaskCreatePinnedToCore(pollTask, "indicator_touch", 3072, nullptr, 2, &s_task, 0) != pdPASS) return false;
  s_async = true; return true;
}
bool heltecV4CapTouchIsAsyncPolling() { return s_async; }
bool heltecV4CapTouchIsSwiping() { return s_swiping; }
void heltecV4CapTouchSetRotation(uint8_t) {}
void heltecV4CapTouchSetPointRotation(uint8_t) {}
void heltecV4CapTouchSetSlowPoll(bool slow) { s_periodMs = slow ? 50 : 8; }
const char* heltecV4CapTouchDebug() { return s_debug; }
void heltecV4CapTouchGetRaw(uint16_t* x, uint16_t* y) { if (x) *x = s_rawX; if (y) *y = s_rawY; }

#endif
