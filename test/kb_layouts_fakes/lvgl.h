// SPDX-License-Identifier: GPL-3.0-or-later
// The LVGL names src/ui-touch/KeyboardLayouts.cpp uses, for a host test of its
// physical-key maps (test/test_keyboard_layouts_hw.cpp). The on-screen maps compile
// against these and are never drawn.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint16_t lv_btnmatrix_ctrl_t;
enum {
  LV_BTNMATRIX_CTRL_HIDDEN = 0x0008, LV_BTNMATRIX_CTRL_NO_REPEAT = 0x0010,
  LV_BTNMATRIX_CTRL_DISABLED = 0x0020, LV_BTNMATRIX_CTRL_CHECKABLE = 0x0040,
  LV_BTNMATRIX_CTRL_CHECKED = 0x0080, LV_BTNMATRIX_CTRL_CLICK_TRIG = 0x0100,
  LV_BTNMATRIX_CTRL_POPOVER = 0x0200, LV_BTNMATRIX_CTRL_RECOLOR = 0x1000,
};
#define LV_KEYBOARD_CTRL_BTN_FLAGS (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED)
typedef enum { LV_KEYBOARD_MODE_TEXT_LOWER, LV_KEYBOARD_MODE_TEXT_UPPER, LV_KEYBOARD_MODE_SPECIAL,
               LV_KEYBOARD_MODE_NUMBER, LV_KEYBOARD_MODE_USER_1 } lv_keyboard_mode_t;
struct _lv_obj_t { int unused; };
typedef struct _lv_obj_t lv_obj_t;
inline void lv_keyboard_set_map(lv_obj_t*, lv_keyboard_mode_t, const char* const*, const lv_btnmatrix_ctrl_t*) {}
#define LV_SYMBOL_BACKSPACE "\xEF\x95\x9A"
#define LV_SYMBOL_NEW_LINE "\xEF\xA2\xA2"
#define LV_SYMBOL_OK "\xEF\x80\x8C"
#define LV_SYMBOL_CLOSE "\xEF\x80\x8D"
#define LV_SYMBOL_LEFT "\xEF\x81\x93"
#define LV_SYMBOL_RIGHT "\xEF\x81\x94"
#define LV_SYMBOL_KEYBOARD "\xEF\x84\x9C"
#define LV_SYMBOL_UP "\xEF\x81\xB7"
#define LV_KB_BTN(width) (LV_BTNMATRIX_CTRL_POPOVER | (width))
