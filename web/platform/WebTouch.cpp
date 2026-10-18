// SPDX-License-Identifier: GPL-3.0-or-later
// The web board's touch driver: the touch-UI input API (src/helpers/input/
// HeltecV4CapTouch.h) over the SDL mouse / finger. Same gesture rules as the
// CHSC6x driver (src/helpers/input/HeltecV4CapTouch.cpp): a horizontal move past
// SWIPE_MIN is a swipe (cancels the press), short press = tap, 1 s still = long.
#include "helpers/input/HeltecV4CapTouch.h"
#include "WebInput.h"
#include <Arduino.h>
#include <helpers/ui/MomentaryButton.h>

namespace {
constexpr int kSwipeMin = 36;          // HELTEC_V4_TOUCH_SWIPE_MIN
constexpr unsigned long kLongMs = 1000;
constexpr int kLongMoveMax = 18;
bool s_ptr_down = false;
int s_ptr_x = 0, s_ptr_y = 0;
bool s_tap_pending = false; uint16_t s_tap_x = 0, s_tap_y = 0;
bool s_swipe_pending = false; int8_t s_swipe_x = 0, s_swipe_y = 0;
bool s_live = false; uint16_t s_live_x = 0, s_live_y = 0;
bool s_swiping_now = false;
}  // namespace

void webPointer(bool down, int x, int y) { s_ptr_down = down; s_ptr_x = x; s_ptr_y = y; }

bool heltecV4CapTouchBegin() { return true; }
int heltecV4CapTouchCheck() {
  static bool down = false, long_dispatched = false;
  static unsigned long down_at = 0;
  static int start_x = 0, start_y = 0, last_x = 0, last_y = 0;
  const bool now = s_ptr_down;
  if (now) { last_x = s_ptr_x; last_y = s_ptr_y; }
  if (now && !down) {
    down = true; down_at = millis(); long_dispatched = false;
    start_x = last_x; start_y = last_y;
    s_swiping_now = false;
    s_live = true; s_live_x = (uint16_t)last_x; s_live_y = (uint16_t)last_y;
  } else if (now && down) {
    s_live = true; s_live_x = (uint16_t)last_x; s_live_y = (uint16_t)last_y;
    if (!s_swiping_now) {
      const int adx = abs(last_x - start_x), ady = abs(last_y - start_y);
      if (adx >= kSwipeMin && adx > ady) s_swiping_now = true;
    }
  } else if (!now && down) {
    s_live = false; down = false; s_swiping_now = false;
    const unsigned long dur = millis() - down_at;
    const int dx = last_x - start_x, dy = last_y - start_y;
    const int adx = abs(dx), ady = abs(dy);
    if (!long_dispatched && adx >= kSwipeMin && adx > ady + 8) {
      s_swipe_x = dx < 0 ? -1 : 1; s_swipe_y = 0; s_swipe_pending = true;
      return dx < 0 ? BUTTON_EVENT_DOUBLE_CLICK : BUTTON_EVENT_TRIPLE_CLICK;
    }
    if (!long_dispatched && ady >= kSwipeMin && ady > adx + 8) {
      s_swipe_x = 0; s_swipe_y = dy < 0 ? -1 : 1; s_swipe_pending = true;
      return BUTTON_EVENT_NONE;
    }
    if (!long_dispatched && dur < kLongMs) {
      s_tap_x = (uint16_t)last_x; s_tap_y = (uint16_t)last_y; s_tap_pending = true;
      return BUTTON_EVENT_CLICK;
    }
    long_dispatched = false;
  } else if (down && !long_dispatched && millis() - down_at >= kLongMs) {
    if (abs(last_x - start_x) > kLongMoveMax || abs(last_y - start_y) > kLongMoveMax) return BUTTON_EVENT_NONE;
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
  return true;
}
// No background poll task on the host: the UI then polls inline from the LVGL indev
// read, on the UI thread, which is where the SDL pointer state lives.
bool heltecV4CapTouchStartBackgroundPoll(uint32_t) { return false; }
bool heltecV4CapTouchIsAsyncPolling() { return false; }
bool heltecV4CapTouchIsSwiping() { return s_swiping_now; }
void heltecV4CapTouchSetRotation(uint8_t) {}
void heltecV4CapTouchSetSlowPoll(bool) {}
void heltecV4CapTouchSetPointRotation(uint8_t) {}
const char* heltecV4CapTouchDebug() { return "web: SDL pointer"; }
void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) { if (rx) *rx = (uint16_t)s_ptr_x; if (ry) *ry = (uint16_t)s_ptr_y; }
