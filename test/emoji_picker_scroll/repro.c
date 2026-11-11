#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static lv_color_t draw_pixels[320 * 40];

static void flush_cb(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_disp_flush_ready(driver);
}

int main(int argc, char **argv)
{
    const int broken = argc > 1 && strcmp(argv[1], "null") == 0;
    lv_init();
    lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels, NULL, 320 * 40);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 320;
    driver.ver_res = 240;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = flush_cb;
    lv_disp_drv_register(&driver);

    static const lv_style_prop_t no_transition_props[] = { (lv_style_prop_t)0 };
    static lv_style_transition_dsc_t no_transition;
    lv_style_transition_dsc_init(&no_transition, no_transition_props, NULL, 0, 0, NULL);
    lv_obj_t *grid = lv_obj_create(lv_scr_act());
    lv_obj_set_size(grid, 300, 180);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    for(int index = 0; index < 60; ++index) {
        lv_obj_t *cell = lv_btn_create(grid);
        lv_obj_set_size(cell, 38, 38);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x303030), LV_PART_MAIN);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x505050), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_transform_width(cell, 0, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_transform_height(cell, 0, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_transition(cell, broken ? NULL : &no_transition, LV_PART_MAIN);
    }
    lv_obj_update_layout(grid);
    for(int index = 0; index < 60; ++index) {
        lv_obj_t *cell = lv_obj_get_child(grid, index);
        lv_obj_add_state(cell, LV_STATE_PRESSED);
        lv_obj_scroll_by(grid, 0, -4, LV_ANIM_OFF);
        lv_obj_clear_state(cell, LV_STATE_PRESSED);
        lv_tick_inc(20);
        lv_timer_handler();
        if(lv_obj_get_width(cell) != 38 || lv_obj_get_height(cell) != 38 ||
           lv_obj_get_style_transform_width(cell, LV_PART_MAIN) != 0 ||
           lv_obj_get_style_transform_height(cell, LV_PART_MAIN) != 0) return 1;
    }
    puts("ok: 60 cell press/scroll/release cycles, unchanged cell dimensions");
    return 0;
}