#pragma once

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// NOTE: LV_USE_LARGE_COORD is intentionally OFF — enabling it caused a boot-time
// LoadProhibited in LVGL on the T-Deck. Chat scroll heights >8191 px are handled
// in UITask via int32 layout offsets + compressed LVGL scroll coords.

// Route LVGL's general allocator through PSRAM-preferred wrappers (defined in
// LvglPsramAlloc.cpp). The board has 8 MB of PSRAM and the ~60 KB of widget
// state LVGL grows during buildUiTree would otherwise come out of the 320 KB
// internal DRAM that WiFi DMA also needs — without this redirect, DMA-capable
// free hovers around 5 KB after boot and association can't complete.
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "LvglPsramAlloc.h"
#define LV_MEM_CUSTOM_ALLOC   lvglPsramAlloc
#define LV_MEM_CUSTOM_FREE    lvglPsramFree
#define LV_MEM_CUSTOM_REALLOC lvglPsramRealloc

// LVGL reads elapsed time straight from the ESP32 hardware timer instead of
// relying on manual lv_tick_inc() calls in the main loop. Means animations
// stay correctly-timed even when the main loop bursts (mesh packet handling,
// long renders) — no more "skipped frames" or "fast-forward catch-up" after
// a stall.
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000))

// 16 ms ≈ 60 Hz refresh. We have CPU headroom (160-200 MHz) and a
// double-buffered internal-DRAM draw setup, so driving the display at
// 60 fps gives noticeably smoother animations / pan / scroll. Higher
// values were chosen earlier when CPU was 80 MHz; on 160+ they're
// leaving frame budget unused.
#define LV_DISP_DEF_REFR_PERIOD 16
// 15 ms (was 30 ms): the touch driver runs async at 125 Hz so the cached state
// is always fresh. Polling LVGL at 67 Hz catches brief taps that the previous
// 30 ms cadence missed (felt as needing to click twice).
#define LV_INDEV_DEF_READ_PERIOD 15

// Scroll threshold (px the finger must travel before a press becomes a scroll).
// Default 10 was too twitchy on the cap-touch panel: a tap on a row inside a
// scrollable container (e.g. the Settings tab, whose section buttons are direct
// children of a vertical-scroll view) would jitter past 10 px and turn into a
// scroll, cancelling the click — the button showed its press style but the
// CLICKED handler never fired ("nothing happens"). 24 px keeps taps reliable
// while still allowing a deliberate drag to scroll.
#define LV_INDEV_DEF_SCROLL_LIMIT 24

#define LV_USE_LOG 0

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
/* Larger Montserrat sizes for the Tanmatsu's crisp "UI size" (Large/Huge) — render the UI bigger
 * at native resolution instead of upscaling a low-res frame. Gated to the Tanmatsu so the
 * flash-tighter S3 touch builds don't pay for fonts they never use. */
#if defined(HAS_TANMATSU)
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#endif
/* 28 px Montserrat for the boot splash title — keeps the rest of the UI on
 * the smaller fonts so the .data cost stays modest. */
#define LV_FONT_MONTSERRAT_28 1
/* Pixelated UNSCII bitmap font for the boot splash. Matches the retro/
 * pixelated wordmark the bootloader paints, so the splash reads as a
 * direct continuation of the boot sequence rather than a different style
 * of "designed" screen. Both sizes get linked since we use 16 for the
 * title and 8 for the subtitle. */
#define LV_FONT_UNSCII_8  1
#define LV_FONT_UNSCII_16 1

/*
 * Unicode / emoji: bundled Montserrat only includes a small subset (see each lv_font_montserrat_*.c header).
 * More letters or emoji need extra glyph data — e.g. run LVGL’s lv_font_conv on Montserrat/Noto TTF with
 * the ranges you want, add the generated .c here, and wire it in UITask (same hardware, flash tradeoff).
 */

#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_LIST 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_TABVIEW 1
#define LV_USE_ROLLER 1
#define LV_USE_BAR 1
#define LV_USE_ARC 1
#define LV_USE_SWITCH 1
#define LV_USE_CANVAS 1
/* QR-code widget for the "share my contact" popup in the Chats tab. Needs
 * LV_USE_CANVAS since lv_qrcode renders into a canvas. */
#define LV_USE_QRCODE 1
/* Split-JPEG decoder for the Map tab's slippy tiles. SJPG decodes JPEG
 * incrementally in stripes so a 256×256 tile only needs ~10-20 KB of
 * working RAM rather than the whole frame at once. */
#define LV_USE_SJPG 1
/* PNG decoder via lodepng. OSM tile.openstreetmap.org serves PNG; the
 * in-firmware Wi-Fi tile fetcher (Phase 4.1c) writes them straight into
 * the tiles LittleFS partition as .png. Tile loader tries .jpg first
 * (offline packs) and falls back to .png (fetched). */
#define LV_USE_PNG 1
/* Decoded-image cache. We pre-decode map tiles to RGB565 ourselves and
 * present them as CF_TRUE_COLOR, so the cache mostly tracks open-decoder
 * metadata rather than holding decoded pixel buffers. 8 entries is plenty
 * for the chat / status icons that DO go through the lazy decoder; the
 * map's 9 visible tiles are managed by our own per-slot LRU. Smaller
 * cache also means less state to keep coherent across slot reuse. */
#define LV_IMG_CACHE_DEF_SIZE 8

/* Image-as-font (lv_imgfont): maps Unicode codepoints to colour images so we can
 * render full-colour emoji inline with text. The emoji set is pre-baked to
 * RGB565+alpha lv_img_dsc_t C-arrays (emoji_data.c) and wired as the tail of the
 * font fallback chain in UITask (montserrat -> extras -> emoji). */
#define LV_USE_FONT_COMPRESSED 1   /* decode RLE-compressed extras/accent fonts (Tanmatsu size budget) */
#define LV_USE_IMGFONT 1

/* Bi-directional text + Arabic/Persian contextual shaping. Lets Arabic (and
 * other RTL scripts) render right-to-left with correctly joined letter forms.
 * No-op for LTR text (Latin/Cyrillic/Greek auto-detect as LTR). The fallback
 * fonts carry the Arabic presentation forms (0xFE70-0xFEFF) that the shaper
 * substitutes in. */
#define LV_USE_BIDI 1
#define LV_BIDI_BASE_DIR_DEF LV_BASE_DIR_AUTO
#define LV_USE_ARABIC_PERSIAN_CHARS 1

