// SPDX-License-Identifier: GPL-3.0-or-later
// Host configuration for test/constellation_render: the firmware's colour depth
// and fonts, the system allocator, no tick source (the renderer drives time).
#ifndef LV_CONF_H
#define LV_CONF_H
#include <stdint.h>
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc
#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_USE_LOG 0
#define LV_USE_USER_DATA 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_USE_FONT_COMPRESSED 1   /* the SemiBold faces are RLE-compressed, as on the device */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_USE_FONT_PLACEHOLDER 1
#endif
