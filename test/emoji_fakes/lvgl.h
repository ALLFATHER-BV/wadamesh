// Host stand-in for the two LVGL 8 image types emoji_data.c and
// variants/tanmatsu/emoji_deflated.cpp use (same fields as lv_img_buf.h).
#pragma once
#include <stdint.h>
enum { LV_IMG_CF_TRUE_COLOR = 4, LV_IMG_CF_TRUE_COLOR_ALPHA = 5 };
typedef struct {
  uint32_t cf : 5;
  uint32_t always_zero : 3;
  uint32_t reserved : 2;
  uint32_t w : 11;
  uint32_t h : 11;
} lv_img_header_t;
typedef struct {
  lv_img_header_t header;
  uint32_t data_size;
  const uint8_t* data;
} lv_img_dsc_t;
