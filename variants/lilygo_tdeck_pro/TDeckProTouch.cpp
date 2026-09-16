// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_TDECK_PRO) && defined(ESP32)

#include <Arduino.h>
#include <helpers/input/HeltecV4CapTouch.h>
#include <helpers/ui/MomentaryButton.h>

#include "TDeckProDisplay.h"

extern TDeckProDisplay display;

namespace {
bool ready = false;
bool down = false;
bool live = false;
bool tap_pending = false;
bool swipe_pending = false;
bool swiping = false;
uint16_t x = 0, y = 0, start_x = 0, start_y = 0, tap_x = 0, tap_y = 0;
int8_t swipe_x = 0, swipe_y = 0;
uint32_t down_at = 0;

void poll() {
  uint16_t next_x = 0, next_y = 0;
  if (display.getTouchPoint(next_x, next_y)) {
    x = next_x;
    y = next_y;
    live = true;
    if (!down) {
      down = true;
      start_x = x;
      start_y = y;
      down_at = millis();
      swiping = false;
    }
    const int dx = (int)x - start_x;
    const int dy = (int)y - start_y;
    if (!swiping && abs(dx) >= 40 && abs(dx) > abs(dy)) swiping = true;
    return;
  }

  live = false;
  if (!down) return;
  down = false;
  const int dx = (int)x - start_x;
  const int dy = (int)y - start_y;
  swiping = false;
  if (abs(dx) >= 40 && abs(dx) > abs(dy) + 8) {
    swipe_x = dx < 0 ? -1 : 1;
    swipe_y = 0;
    swipe_pending = true;
  } else if (abs(dy) >= 40 && abs(dy) > abs(dx) + 8) {
    swipe_x = 0;
    swipe_y = dy < 0 ? -1 : 1;
    swipe_pending = true;
  } else if (millis() - down_at >= 12 && abs(dx) <= 16 && abs(dy) <= 16) {
    tap_x = x;
    tap_y = y;
    tap_pending = true;
  }
}
}

bool heltecV4CapTouchBegin() { ready = true; return true; }
int heltecV4CapTouchCheck() { if (ready) poll(); return BUTTON_EVENT_NONE; }
bool heltecV4CapTouchPopTap(uint16_t* out_x, uint16_t* out_y) {
  if (!tap_pending) return false;
  tap_pending = false;
  if (out_x) *out_x = tap_x;
  if (out_y) *out_y = tap_y;
  return true;
}
bool heltecV4CapTouchGetLive(uint16_t* out_x, uint16_t* out_y) {
  if (!live) return false;
  if (out_x) *out_x = x;
  if (out_y) *out_y = y;
  return true;
}
bool heltecV4CapTouchPopSwipe(int8_t* out_x, int8_t* out_y) {
  if (!swipe_pending) return false;
  swipe_pending = false;
  if (out_x) *out_x = swipe_x;
  if (out_y) *out_y = swipe_y;
  return true;
}
bool heltecV4CapTouchStartBackgroundPoll(uint32_t) { return false; }
bool heltecV4CapTouchIsAsyncPolling() { return false; }
bool heltecV4CapTouchIsSwiping() { return swiping; }
void heltecV4CapTouchSetRotation(uint8_t) {}
void heltecV4CapTouchSetPointRotation(uint8_t) {}
void heltecV4CapTouchSetSlowPoll(bool) {}
const char* heltecV4CapTouchDebug() { return "T-Deck Pro CST328/CST3530"; }
void heltecV4CapTouchGetRaw(uint16_t* out_x, uint16_t* out_y) {
  if (out_x) *out_x = x;
  if (out_y) *out_y = y;
}

#endif