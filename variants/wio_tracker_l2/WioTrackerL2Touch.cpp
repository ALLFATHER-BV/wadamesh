// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_WIO_TRACKER_L2) && defined(ESP32)

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/input/HeltecV4CapTouch.h>
#include <helpers/ui/MomentaryButton.h>

#include "WioTrackerL2Display.h"

extern WioTrackerL2Display display;

namespace {
bool s_ready = false;
bool s_down = false;
bool s_live = false;
bool s_tapPending = false;
bool s_swipePending = false;
bool s_swiping = false;
uint16_t s_x = 0, s_y = 0, s_startX = 0, s_startY = 0, s_tapX = 0, s_tapY = 0;
int8_t s_swipeX = 0, s_swipeY = 0;
uint32_t s_downAt = 0;
TaskHandle_t s_task = nullptr;
bool s_async = false;
uint32_t s_periodMs = 8;

void poll() {
  uint16_t x = 0, y = 0;
  const bool touched = display.getTouchPoint(x, y);
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
}

bool heltecV4CapTouchBegin() { s_ready = true; return true; }
int heltecV4CapTouchCheck() { if (!s_ready) return BUTTON_EVENT_NONE; poll(); return BUTTON_EVENT_NONE; }
bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  if (!s_tapPending) return false; s_tapPending = false;
  if (x) *x = s_tapX; if (y) *y = s_tapY; return true;
}
bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (!s_live) return false; if (x) *x = s_x; if (y) *y = s_y; return true;
}
bool heltecV4CapTouchPopSwipe(int8_t* x, int8_t* y) {
  if (!s_swipePending) return false; s_swipePending = false;
  if (x) *x = s_swipeX; if (y) *y = s_swipeY; return true;
}
bool heltecV4CapTouchStartBackgroundPoll(uint32_t periodMs) {
  if (s_async || !s_ready) return false;
  s_periodMs = periodMs < 4 ? 4 : (periodMs > 100 ? 100 : periodMs);
  if (xTaskCreatePinnedToCore(pollTask, "wio_l2_touch", 3072, nullptr, 2, &s_task, 0) != pdPASS) return false;
  s_async = true; return true;
}
bool heltecV4CapTouchIsAsyncPolling() { return s_async; }
bool heltecV4CapTouchIsSwiping() { return s_swiping; }
void heltecV4CapTouchSetRotation(uint8_t) {}
void heltecV4CapTouchSetPointRotation(uint8_t) {}
void heltecV4CapTouchSetSlowPoll(bool slow) { s_periodMs = slow ? 50 : 8; }
const char* heltecV4CapTouchDebug() { return "Wio Tracker L2 GT911"; }
void heltecV4CapTouchGetRaw(uint16_t* x, uint16_t* y) { if (x) *x = s_x; if (y) *y = s_y; }

#endif