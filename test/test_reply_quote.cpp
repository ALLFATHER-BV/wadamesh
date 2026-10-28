// SPDX-License-Identifier: GPL-3.0-or-later

#define LVGL_H  // prevent ReplyQuote.h from including the real lvgl.h

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

// Minimal LVGL shim for host testing
typedef int16_t lv_coord_t;
typedef struct { lv_coord_t x; lv_coord_t y; } lv_point_t;
typedef struct { int dummy; } lv_font_t;

#define LV_COORD_MAX 32767
#define LV_TEXT_FLAG_NONE 0

static lv_font_t g_font_12 = {0};
static lv_font_t g_font_semi_12 = {0};

static void lv_txt_get_size(lv_point_t* size_res_p, const char* text, const lv_font_t* font,
                            lv_coord_t letter_space, lv_coord_t line_space,
                            lv_coord_t max_width, uint32_t flag) {
  (void)letter_space; (void)line_space; (void)max_width; (void)flag;
  // Approximate: 6px per char in 12px font, 7px per char in semi_12
  int px_per_char = (font == &g_font_semi_12) ? 7 : 6;
  size_t len = strlen(text);
  size_res_p->x = (lv_coord_t)(len * px_per_char);
  size_res_p->y = (lv_coord_t)((font == &g_font_semi_12) ? 14 : 12);
}

static lv_coord_t lv_font_get_line_height(const lv_font_t* font) {
  return (lv_coord_t)((font == &g_font_semi_12) ? 14 : 12);
}

#include "ui-touch/ReplyQuote.h"

int main() {
  char sender[32];
  char text[84];

  // Parse: valid prefix
  {
    const char* body = ReplyQuote::parse("@[Alice] hello", sender, sizeof(sender));
    assert(body != nullptr);
    assert(strcmp(sender, "Alice") == 0);
    assert(strcmp(body, "hello") == 0);
  }

  // Parse: no prefix
  {
    const char* body = ReplyQuote::parse("hello world", sender, sizeof(sender));
    assert(body == nullptr);
  }

  // Parse: empty name
  {
    const char* body = ReplyQuote::parse("@[] hello", sender, sizeof(sender));
    assert(body == nullptr);
  }

  // Parse: missing bracket
  {
    const char* body = ReplyQuote::parse("@[Alice hello", sender, sizeof(sender));
    assert(body == nullptr);
  }

  // Parse: long name (31 chars max)
  {
    const char* body = ReplyQuote::parse("@[1234567890123456789012345678901] test", sender, sizeof(sender));
    assert(body != nullptr);
    assert(strlen(sender) == 31);
  }

  // Parse: name too long (32 chars)
  {
    const char* body = ReplyQuote::parse("@[12345678901234567890123456789012] test", sender, sizeof(sender));
    assert(body == nullptr);
  }

  // formatQuote: short sender and text
  {
    char out_sender[32], out_text[84];
    ReplyQuote::formatQuote("Alice", "hello", 200, &g_font_semi_12, &g_font_12,
                            out_sender, sizeof(out_sender), out_text, sizeof(out_text));
    assert(strcmp(out_sender, "Alice") == 0);
    assert(strcmp(out_text, "hello") == 0);
  }

  // formatQuote: long text that needs truncation
  {
    char out_sender[32], out_text[84];
    ReplyQuote::formatQuote("Bob", "this is a very long message that should be truncated", 50, &g_font_semi_12, &g_font_12,
                            out_sender, sizeof(out_sender), out_text, sizeof(out_text));
    assert(out_text[0] == '.');  // starts with ellipsis
    assert(strlen(out_text) <= 50);
  }

  // formatQuote: long sender name
  {
    char out_sender[32], out_text[84];
    ReplyQuote::formatQuote("VeryLongSenderNameThatExceedsBuffer", "hi", 200, &g_font_semi_12, &g_font_12,
                            out_sender, sizeof(out_sender), out_text, sizeof(out_text));
    assert(strlen(out_sender) <= 31);  // truncated to buffer size
  }

  printf("test_reply_quote: all assertions passed\n");
  return 0;
}