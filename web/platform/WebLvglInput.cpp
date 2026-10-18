// SPDX-License-Identifier: GPL-3.0-or-later
// LVGL-side input helpers of the web board (WebInput.h): text fields for the phone's
// keyboard (below), and mouse wheel / trackpad scrolling (webWheel). A panel has no
// wheel, so the UI never asks for one: this scrolls the LVGL object under the pointer the way a finger drag would, without a press (nothing gets clicked or
// long-pressed). It finds the topmost object under the pointer (system layer, top
// layer, then the screen, as LVGL's own pointer search does) and walks up to the first
// ancestor that can still scroll that way, so a list inside a page scrolls before the
// page does. Vertical only: the UI's horizontal scrollers are tab strips that snap.
// Runs on the UI thread between LVGL timer passes (the front ends' event pumps).
#include "WebInput.h"
#include <Arduino.h>
#include <lvgl.h>

namespace {
lv_obj_t* s_scrolled = nullptr;   // the object the last wheel step moved
uint32_t s_last_ms = 0;

lv_obj_t* hit(lv_point_t p) {
  if (lv_obj_t* o = lv_indev_search_obj(lv_layer_sys(), &p)) return o;
  if (lv_obj_t* o = lv_indev_search_obj(lv_layer_top(), &p)) return o;
  return lv_indev_search_obj(lv_scr_act(), &p);
}
}  // namespace

void webWheel(int x, int y, int dy) {
  if (dy == 0 || !lv_scr_act()) return;
  for (lv_obj_t* o = hit(lv_point_t{(lv_coord_t)x, (lv_coord_t)y}); o; o = lv_obj_get_parent(o)) {
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) || !(lv_obj_get_scroll_dir(o) & LV_DIR_VER)) continue;
    // dy > 0: the wheel turned down / two fingers moved up: show what is further down
    const lv_coord_t room = dy > 0 ? lv_obj_get_scroll_bottom(o) : lv_obj_get_scroll_top(o);
    if (room <= 0) continue;   // this one is at its end: chain to the parent
    const lv_coord_t step = (lv_coord_t)(dy > 0 ? LV_MIN(dy, room) : -LV_MIN(-dy, room));
    if (s_scrolled && s_scrolled != o && lv_obj_is_valid(s_scrolled)) lv_event_send(s_scrolled, LV_EVENT_SCROLL_END, nullptr);
    if (s_scrolled != o) lv_event_send(o, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_scroll_by(o, 0, -step, LV_ANIM_OFF);
    s_scrolled = o;
    s_last_ms = millis();
    return;
  }
}

// From the board loop: a wheel that has stopped ends the scroll (lists that load more
// at their end, or tidy up, listen for SCROLL_END as they do after a drag).
void webWheelTick() {
  if (!s_scrolled || millis() - s_last_ms < 150) return;
  if (lv_obj_is_valid(s_scrolled)) lv_event_send(s_scrolled, LV_EVENT_SCROLL_END, nullptr);
  s_scrolled = nullptr;
}

// ---- text fields, for the phone's own keyboard (web/shell/app.js) -----------------------
bool webTextareaAt(int x, int y) {
  if (!lv_scr_act()) return false;
  for (lv_obj_t* o = hit(lv_point_t{(lv_coord_t)x, (lv_coord_t)y}); o; o = lv_obj_get_parent(o))
    if (lv_obj_check_type(o, &lv_textarea_class)) return true;
  return false;
}
namespace {
lv_obj_t* findKeyboard(lv_obj_t* o, int depth) {
  if (!o || depth > 12) return nullptr;
  if (lv_obj_check_type(o, &lv_keyboard_class)) return o;
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; ++i)
    if (lv_obj_t* k = findKeyboard(lv_obj_get_child(o, i), depth + 1)) return k;
  return nullptr;
}
lv_obj_t* s_kb = nullptr;
uint32_t s_kb_look_ms = 0;
}  // namespace
// The UI keeps one keyboard object and binds it to the field being edited (showKb), also
// while an external keyboard types and the keys stay hidden; unbinding ends the edit.
bool webEditing() {
  if (s_kb && !lv_obj_is_valid(s_kb)) s_kb = nullptr;
  if (!s_kb && millis() - s_kb_look_ms > 1000) {
    s_kb_look_ms = millis();
    s_kb = findKeyboard(lv_layer_top(), 0);
    if (!s_kb) s_kb = findKeyboard(lv_layer_sys(), 0);
    if (!s_kb) s_kb = findKeyboard(lv_scr_act(), 0);
  }
  return s_kb && lv_keyboard_get_textarea(s_kb) != nullptr;
}
bool webOskShown() {
  webEditing();   // finds the keyboard object
  return s_kb && !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}
