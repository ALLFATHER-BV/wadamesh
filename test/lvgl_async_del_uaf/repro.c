// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host reproduction of a pending lv_obj_del_async() outliving its object. Run it
// through run.sh, which builds LVGL with AddressSanitizer.
//
//   "parent"  a child's delete is queued, then its parent is deleted at once
//             (the chat divider crash noted at chatVirtReset in UITask.cpp);
//   "twice"   two closers queue the same object;
//   "plain"   one queued delete, the normal case - it must still work, and with
//             only the lv_obj_tree.c half of the patch it frees its info twice.

#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define W 320
#define H 240
static lv_color_t s_buf[W * 40];
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_drv;

static void flush_cb(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * px)
{
    (void)area;
    (void)px;
    lv_disp_flush_ready(drv);
}

int main(int argc, char ** argv)
{
    const char * mode = argc > 1 ? argv[1] : "plain";
    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf, NULL, W * 40);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = W;
    s_drv.ver_res = H;
    s_drv.flush_cb = flush_cb;
    s_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_drv);

    lv_obj_t * scr = lv_scr_act();
    lv_obj_t * sheet = lv_obj_create(scr);
    lv_obj_t * row = lv_obj_create(sheet);
    lv_tick_inc(20);
    lv_timer_handler();

    if(strcmp(mode, "parent") == 0) {
        lv_obj_del_async(row);
        lv_obj_del(sheet);
    }
    else if(strcmp(mode, "twice") == 0) {
        lv_obj_del_async(sheet);
        lv_obj_del_async(sheet);
    }
    else {
        lv_obj_del_async(sheet);
    }
    for(int i = 0; i < 5; i++) {
        lv_tick_inc(20);
        lv_timer_handler();
    }

    const int ok = lv_obj_get_child_cnt(scr) == 0;
    printf("%s: %s children_left=%u\n", ok ? "ok" : "FAIL", mode, (unsigned)lv_obj_get_child_cnt(scr));
    return ok ? 0 : 1;
}
