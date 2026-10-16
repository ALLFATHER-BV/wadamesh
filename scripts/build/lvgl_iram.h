// Force-included into the LVGL sources named by custom_lvgl_iram (scripts/build/perf_lvgl.py):
// the functions LVGL marks LV_ATTRIBUTE_FAST_MEM there are placed in IRAM, like IRAM_ATTR.
// A header rather than a -D, so the attribute's brackets and quotes never meet a shell.
#pragma once
#define LV_ATTRIBUTE_FAST_MEM __attribute__((section(".iram1.lvgl")))
