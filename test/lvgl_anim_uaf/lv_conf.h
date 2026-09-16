// SPDX-License-Identifier: GPL-3.0-or-later
// Host configuration for test/lvgl_anim_uaf. LVGL allocates through the system
// allocator so AddressSanitizer can see every animation descriptor.
#ifndef LV_CONF_H
#define LV_CONF_H
#include <stdint.h>
#define LV_COLOR_DEPTH 16
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc
#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_USE_LOG 0
#define LV_USE_USER_DATA 1
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
