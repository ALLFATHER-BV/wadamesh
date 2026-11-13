// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host reproduction of the beta_102 T-Deck coredump (loopTask, LoadProhibited in
// get_prop_core under _lv_indev_scroll_handler). Run it through run.sh, which
// builds LVGL with AddressSanitizer.
//
// A page is dragged until it scrolls, and code running inside that gesture calls
// lv_indev_wait_release(), as popupClose() does. The release that ends the wait
// clears act_obj and last_obj but keeps scroll_obj for the throw. The page is then
// deleted while it still has momentum, and stock LVGL 8.4 keeps scroll_obj
// pointing at it:
//   "press"   the next drag walks the freed page in _lv_indev_scroll_handler
//             (the beta_102 coredump);
//   "release" the next throw step reads it in _lv_indev_scroll_throw_handler
//             (the beta_18 coredump noted at popupClose()).

#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define W 320
#define H 240
static lv_color_t s_buf[W * 40];
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_drv;
static lv_indev_drv_t s_indev_drv;
static lv_indev_t * s_indev;

static lv_indev_state_t s_state = LV_INDEV_STATE_RELEASED;
static lv_point_t s_point = { 100, 200 };

static void flush_cb(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * px)
{
    (void)area;
    (void)px;
    lv_disp_flush_ready(drv);
}

static void read_cb(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    (void)drv;
    data->state = s_state;
    data->point = s_point;
}

// One indev read period.
static void step(void)
{
    lv_tick_inc(LV_INDEV_DEF_READ_PERIOD);
    lv_timer_handler();
}

static void touch(lv_indev_state_t state, lv_coord_t y)
{
    s_state = state;
    s_point.y = y;
    step();
}

// What popupClose() does first when it runs inside a gesture. Once: the throw
// sends SCROLL_BEGIN again, and the sheet closes only once.
static int s_closing;

static void scroll_begin_cb(lv_event_t * e)
{
    (void)e;
    if(s_closing) return;
    s_closing = 1;
    lv_indev_wait_release(lv_indev_get_act());
}

int main(int argc, char ** argv)
{
    const int press = argc > 1 && strcmp(argv[1], "press") == 0;
    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf, NULL, W * 40);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = W;
    s_drv.ver_res = H;
    s_drv.flush_cb = flush_cb;
    s_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_drv);
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = read_cb;
    s_indev = lv_indev_drv_register(&s_indev_drv);

    lv_obj_t * page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page, W, H);
    lv_obj_t * content = lv_obj_create(page);
    lv_obj_set_size(content, W - 40, 2000);
    lv_obj_add_event_cb(page, scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    step();

    // Fast upward drag: the page starts scrolling and the handler asks LVGL to
    // ignore the finger until it lifts.
    touch(LV_INDEV_STATE_PRESSED, 200);
    for(lv_coord_t y = 170; y >= 50; y -= 30) touch(LV_INDEV_STATE_PRESSED, y);
    touch(LV_INDEV_STATE_RELEASED, 50);
    const int scrolling = lv_indev_get_scroll_obj(s_indev) == page;

    // The sheet closes (popupClose's lv_obj_del_async has run) while the page
    // still has momentum.
    lv_obj_del(page);

    if(press) {
        touch(LV_INDEV_STATE_PRESSED, 200);
        touch(LV_INDEV_STATE_PRESSED, 170);
        touch(LV_INDEV_STATE_PRESSED, 140);
        touch(LV_INDEV_STATE_RELEASED, 140);
    }
    for(int i = 0; i < 5; i++) step();

    const int ok = scrolling && lv_indev_get_scroll_obj(s_indev) == NULL;
    printf("%s: %s scrolling_before_delete=%d scroll_obj_after=%p\n", ok ? "ok" : "FAIL",
           press ? "press" : "release", scrolling, (void *)lv_indev_get_scroll_obj(s_indev));
    return ok ? 0 : 1;
}
