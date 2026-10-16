#include <lvgl.h>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
#include "ui-touch/ChatHashtag.h"

static uint32_t COLOR_CHAT_LINK = 0x4EA1FF;
static size_t layout_calls = 0;
static uint16_t trackedVisualPos(const char* text, char** visual, uint16_t length,
                                lv_base_dir_t dir, uint32_t logical, bool* rtl) {
    ++layout_calls;
    return _lv_bidi_get_visual_pos(text, visual, length, dir, logical, rtl);
}
#define _lv_bidi_get_visual_pos trackedVisualPos
#include "implementation.inc"
#undef _lv_bidi_get_visual_pos

struct DrawnGlyph {
    lv_point_t pos;
    uint32_t letter;
};
static std::vector<DrawnGlyph> drawn;
static void captureLetter(lv_draw_ctx_t*, const lv_draw_label_dsc_t*, const lv_point_t* pos, uint32_t letter) {
    drawn.push_back({*pos, letter});
}
static void flushDisplay(lv_disp_drv_t* driver, const lv_area_t*, lv_color_t*) {
    lv_disp_flush_ready(driver);
}

static void check(const char* text, lv_base_dir_t dir, lv_text_align_t align,
                  lv_coord_t width, lv_coord_t letter_space, const char* expected_links) {
    lv_obj_t* parent = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 320, 800);
    lv_obj_t* plain = lv_label_create(parent);
    lv_obj_set_style_text_font(plain, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_base_dir(plain, dir, LV_PART_MAIN);
    lv_obj_set_style_text_align(plain, align, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(plain, letter_space, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(plain, 3, LV_PART_MAIN);
    lv_obj_set_width(plain, width);
    lv_label_set_text(plain, text);
    lv_obj_t* overlay = chatCreateLinkLabel(parent, plain, text, &lv_font_montserrat_14,
                                           lv_color_white(), width, 0, 0);
    assert(overlay);
    lv_obj_update_layout(parent);
    assert(lv_obj_get_height(overlay) == lv_obj_get_height(plain));
    assert(lv_obj_get_style_text_opa(plain, LV_PART_MAIN) == LV_OPA_COVER);

    lv_area_t area;
    lv_obj_get_content_coords(plain, &area);
    lv_area_t clip = {0, 0, 319, 799};
    lv_draw_ctx_t ctx{};
    ctx.clip_area = &clip;
    ctx.draw_letter = captureLetter;
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = &lv_font_montserrat_14;
    dsc.bidi_dir = dir;
    dsc.align = align;
    dsc.letter_space = letter_space;
    dsc.line_space = 3;
    drawn.clear();
    lv_draw_label(&ctx, &dsc, &area, lv_label_get_text(plain), nullptr);
    const auto normal = drawn;
    drawn.clear();
    lv_event_send(overlay, LV_EVENT_DRAW_MAIN_END, &ctx);
    assert(!drawn.empty());
    size_t expected_count = _lv_txt_encoded_get_char_id(expected_links, strlen(expected_links));
    assert(drawn.size() == expected_count);
    for (const auto& glyph : drawn) {
        bool matched = false;
        for (const auto& original : normal) {
            if (glyph.letter == original.letter && glyph.pos.x == original.pos.x && glyph.pos.y == original.pos.y)
                matched = true;
        }
        if (!matched) std::fprintf(stderr, "glyph U+%04X at %d,%d not in LVGL output: %s\n",
                                   glyph.letter, glyph.pos.x, glyph.pos.y, text);
        assert(matched);
    }
    uint32_t byte = 0;
    for (const auto& glyph : drawn)
        assert(glyph.letter == _lv_txt_encoded_next(expected_links, &byte));
    const size_t calls_after_layout = layout_calls;
    for (int frame = 0; frame < 10; ++frame) {
        lv_obj_set_y(parent, frame);
        lv_obj_update_layout(parent);
        drawn.clear();
        lv_event_send(overlay, LV_EVENT_DRAW_MAIN_END, &ctx);
        assert(layout_calls == calls_after_layout);
        assert(drawn.size() == expected_count);
    }
    lv_obj_set_style_text_letter_space(plain, letter_space + 1, LV_PART_MAIN);
    lv_obj_update_layout(parent);
    lv_event_send(overlay, LV_EVENT_DRAW_MAIN_END, &ctx);
    assert(layout_calls > calls_after_layout);
    lv_obj_del(parent);
}

int main() {
    lv_init();
    static lv_color_t buffer[320 * 20];
    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, buffer, nullptr, 320 * 20);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 320;
    driver.ver_res = 800;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = flushDisplay;
    lv_disp_drv_register(&driver);
    check("plain # test #mesh tail", LV_BASE_DIR_LTR, LV_TEXT_ALIGN_LEFT, 240, 0, "#mesh");
    check("plain https://example.com/path#section tail #mesh", LV_BASE_DIR_LTR,
          LV_TEXT_ALIGN_LEFT, 95, 0, "https://example.com/path#section#mesh");
    check("plain #mesh and #radio tail", LV_BASE_DIR_LTR, LV_TEXT_ALIGN_CENTER, 110, 2, "#mesh#radio");
    check("plain 32.123, 34.456 tail", LV_BASE_DIR_LTR, LV_TEXT_ALIGN_RIGHT, 240, 0, "32.123, 34.456");
    const char* rtl = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D #mesh \xD7\xA2\xD7\x95\xD7\x93";
    check(rtl, LV_BASE_DIR_AUTO, LV_TEXT_ALIGN_AUTO, 240, 0, "#mesh");
    check(rtl, LV_BASE_DIR_RTL, LV_TEXT_ALIGN_RIGHT, 90, 2, "#mesh");
    check("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D https://example.com/#section tail #mesh",
          LV_BASE_DIR_AUTO, LV_TEXT_ALIGN_AUTO, 110, 0, "https://example.com/#section#mesh");
    check("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 #mesh tail", LV_BASE_DIR_AUTO,
          LV_TEXT_ALIGN_AUTO, 240, 0, "#mesh");
    check("plain #mesh tail", LV_BASE_DIR_RTL, LV_TEXT_ALIGN_CENTER, 100, 1, "#mesh");
    std::puts("PASS: link-only glyphs, hashes, LTR/RTL, shaping, wrapping, alignment and scroll cache");
}