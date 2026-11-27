// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal LVGL v8 declarations so the host can compile KeyboardLayouts.cpp and
// test its map remapping without the firmware. The values mirror
// lvgl/src/widgets/lv_btnmatrix.h and lv_keyboard.h exactly; the LV_SYMBOL_*
// strings only need to be distinct and stable within this translation unit.
#pragma once
#include <stdint.h>
typedef uint16_t lv_btnmatrix_ctrl_t;
typedef struct _lv_obj_t { int dummy; } lv_obj_t;
enum {
    LV_BTNMATRIX_CTRL_HIDDEN     = 0x0010,
    LV_BTNMATRIX_CTRL_NO_REPEAT  = 0x0020,
    LV_BTNMATRIX_CTRL_DISABLED   = 0x0040,
    LV_BTNMATRIX_CTRL_CHECKABLE  = 0x0080,
    LV_BTNMATRIX_CTRL_CHECKED    = 0x0100,
    LV_BTNMATRIX_CTRL_CLICK_TRIG = 0x0200,
    LV_BTNMATRIX_CTRL_POPOVER    = 0x0400,
};
#define LV_KEYBOARD_CTRL_BTN_FLAGS (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED)
#define LV_SYMBOL_OK         "\x01sOK"
#define LV_SYMBOL_LEFT       "\x01sL"
#define LV_SYMBOL_RIGHT      "\x01sR"
#define LV_SYMBOL_UP         "\x01sU"
#define LV_SYMBOL_KEYBOARD   "\x01sK"
#define LV_SYMBOL_BACKSPACE  "\x01sB"
#define LV_SYMBOL_NEW_LINE   "\x01sN"
#define LV_SYMBOL_REFRESH    "\x01sRf"
#define LV_SYMBOL_PASTE      "\x01sP"
enum { LV_KEYBOARD_MODE_TEXT_LOWER = 1, LV_KEYBOARD_MODE_TEXT_UPPER = 2,
       LV_KEYBOARD_MODE_SPECIAL = 3, LV_KEYBOARD_MODE_NUMBER = 4, LV_KEYBOARD_MODE_USER_1 = 5 };
void lv_keyboard_set_map(struct _lv_obj_t* keyboard, uint16_t mode,
                         const char* const map[], const lv_btnmatrix_ctrl_t ctrl_map[]);
