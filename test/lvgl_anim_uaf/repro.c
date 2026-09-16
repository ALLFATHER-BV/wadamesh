// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host reproduction of the chat-scroll panic in issues #428 and #475. Run it
// through run.sh, which builds LVGL with AddressSanitizer.
//
// The keypad page scroll (navScrollBy) animates the chat message list. Each
// animation step sends LV_EVENT_SCROLL, and chatVirtRemap1To1Scroll re-anchors
// the compressed scroll position with lv_obj_scroll_to_y(..., LV_ANIM_OFF),
// which deletes the running animation. When the UI loop stalled past the
// animation's duration, that step was also its last, and stock LVGL 8.4 then
// read and finished the freed animation.
//
// "plain" is the handler before the fix, "guard" skips the re-anchor while an
// animation drives the scroll (the fix in UITask.cpp).

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

// chatVirtRemap1To1Scroll with a 2:1 compressed scroll space.
static int s_guard, s_virt_top, s_anchor, s_scroll_end;

static void scroll_cb(lv_event_t * e)
{
    lv_obj_t * o = lv_event_get_target(e);
    lv_coord_t lv_y = lv_obj_get_scroll_y(o);
    lv_coord_t delta = lv_y - s_anchor;
    if(delta == 0) return;
    s_virt_top += delta;
    if(s_guard && lv_anim_get(o, NULL)) {
        s_anchor = lv_y;
        return;
    }
    lv_coord_t corrected = s_virt_top / 2;
    s_anchor = corrected;
    if(corrected != lv_y) lv_obj_scroll_to_y(o, corrected, LV_ANIM_OFF);
}

static void scroll_end_cb(lv_event_t * e)
{
    (void)e;
    s_scroll_end++;
}

int main(int argc, char ** argv)
{
    s_guard = argc > 1 && strcmp(argv[1], "guard") == 0;
    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf, NULL, W * 40);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = W;
    s_drv.ver_res = H;
    s_drv.flush_cb = flush_cb;
    s_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_drv);

    lv_obj_t * list = lv_obj_create(lv_scr_act());
    lv_obj_set_size(list, W, H);
    lv_obj_t * spacer = lv_obj_create(list);
    lv_obj_set_size(spacer, W - 40, 5000);
    lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    lv_tick_inc(20);
    lv_timer_handler();

    lv_obj_scroll_by(list, 0, -120, LV_ANIM_ON);   // navScrollBy
    lv_tick_inc(500);                               // the loop stalls past the animation
    lv_timer_handler();
    for(int i = 0; i < 20; i++) {
        lv_tick_inc(20);
        lv_timer_handler();
    }

    const int ok = s_scroll_end == 1 && lv_anim_count_running() == 0 &&
                   s_virt_top == 120 && lv_obj_get_scroll_y(list) == (s_guard ? 120 : 60);
    printf("%s: %s scroll_y=%d virt_top=%d scroll_end=%d\n", ok ? "ok" : "FAIL",
           s_guard ? "guard" : "plain", (int)lv_obj_get_scroll_y(list), s_virt_top, s_scroll_end);
    return ok ? 0 : 1;
}
