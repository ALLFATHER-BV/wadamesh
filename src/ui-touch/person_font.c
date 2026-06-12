/*******************************************************************************
 * Size: 16 px
 * Bpp: 4
 * Opts: --no-compress --bpp 4 --size 16 --font /tmp/fa/fa-solid-900.ttf -r 0xF007 --format lvgl -o /tmp/person_font.c
 ******************************************************************************/

#include "lvgl.h"

#ifndef PERSON_FONT
#define PERSON_FONT 1
#endif

#if PERSON_FONT

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F007 "" */
    0x0, 0x0, 0x2a, 0xee, 0xa2, 0x0, 0x0, 0x0,
    0x2, 0xef, 0xff, 0xfe, 0x20, 0x0, 0x0, 0xa,
    0xff, 0xff, 0xff, 0xa0, 0x0, 0x0, 0xe, 0xff,
    0xff, 0xff, 0xe0, 0x0, 0x0, 0xe, 0xff, 0xff,
    0xff, 0xe0, 0x0, 0x0, 0xa, 0xff, 0xff, 0xff,
    0xa0, 0x0, 0x0, 0x2, 0xef, 0xff, 0xfe, 0x20,
    0x0, 0x0, 0x0, 0x2a, 0xee, 0xa2, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x1, 0x57, 0x77, 0x75, 0x10, 0x0, 0x0, 0x8f,
    0xff, 0xff, 0xff, 0xf8, 0x0, 0xa, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xa0, 0x5f, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xf5, 0xbf, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xfe, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfc
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 224, .box_w = 14, .box_h = 16, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61447, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t person_font = {
#else
lv_font_t person_font = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if PERSON_FONT*/

