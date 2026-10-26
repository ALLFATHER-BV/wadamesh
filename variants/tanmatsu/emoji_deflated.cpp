// Tanmatsu replacement for src/ui-touch/emoji_data.c: the same colour emoji, kept
// deflated in flash and inflated into PSRAM on the first lookup.
//
// Why: the launcher maps at most 50 x 64 KB of an AppFS app (appfsBlMmap), so
// application.bin must stay <= 3,276,800 B. The raw emoji images are ~209 KB of
// that; deflated they are ~105 KB. The Tanmatsu has PSRAM to spare, so holding
// the inflated images there (one ~209 KB block, once) costs nothing visible.
//
// emoji_deflated.h is generated at build time from emoji_data.c by
// scripts/build/gen-emoji-deflated.py (tanmatsu/components/wadamesh_app/
// CMakeLists.txt), which also verifies the round trip. The IDF build leaves
// emoji_data.c out and defines WADA_EMOJI_DEFLATED; nothing else compiles this.
#if defined(WADA_EMOJI_DEFLATED) && WADA_EMOJI_DEFLATED
#include "emoji_data.h"
#include "emoji_deflated.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "miniz.h"   // the ROM's tinfl: inflating costs no flash

static lv_img_dsc_t* s_emoji_dsc = nullptr;   // EMOJI_Z_IMG_COUNT descriptors, PSRAM
static bool s_emoji_broken = false;           // the blob failed to inflate: never retry

// Inflate every image once. Called from the LVGL thread (every emojiGlyphLookup
// caller is UI code). An allocation failure leaves the table empty and is retried
// on a later lookup; until then an emoji simply has no glyph, as an unknown one.
static bool emojiInflate() {
  if (s_emoji_dsc) return true;
  if (s_emoji_broken) return false;
  uint8_t* pix = (uint8_t*)heap_caps_malloc(EMOJI_Z_RAW_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  lv_img_dsc_t* dsc = (lv_img_dsc_t*)heap_caps_calloc(EMOJI_Z_IMG_COUNT, sizeof(lv_img_dsc_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  tinfl_decompressor* d = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor),
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bool ok = false, tried = false;
  if (pix && dsc && d) {
    tried = true;
    tinfl_init(d);   // ~11 KB of state: never on a task stack
    size_t in = sizeof(kEmojiZ), out = EMOJI_Z_RAW_LEN;
    ok = tinfl_decompress(d, kEmojiZ, &in, pix, pix, &out,
                          TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) == TINFL_STATUS_DONE &&
         out == EMOJI_Z_RAW_LEN;
  }
  if (d) heap_caps_free(d);
  if (!ok) {
    if (pix) heap_caps_free(pix);
    if (dsc) heap_caps_free(dsc);
    if (tried) s_emoji_broken = true;   // same input, same result: do not inflate per draw
    return false;
  }
  for (int i = 0; i < EMOJI_Z_IMG_COUNT; ++i) {
    const EmojiZImg& m = kEmojiZImgs[i];
    dsc[i].header.cf = m.cf;
    dsc[i].header.always_zero = 0;
    dsc[i].header.reserved = 0;
    dsc[i].header.w = m.w;
    dsc[i].header.h = m.h;
    dsc[i].data_size = m.len;
    dsc[i].data = pix + m.off;
  }
  s_emoji_dsc = dsc;   // pix lives for the rest of the run: every descriptor points into it
  return true;
}

const uint16_t kEmojiGlyphCount = EMOJI_Z_GLYPH_COUNT;

const lv_img_dsc_t* emojiGlyphLookup(uint32_t cp) {
  int lo = 0, hi = (int)(sizeof(kEmojiZGlyphs) / sizeof(kEmojiZGlyphs[0])) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    const uint32_t v = kEmojiZGlyphs[mid].cp;
    if (cp == v) return emojiInflate() ? &s_emoji_dsc[kEmojiZGlyphs[mid].img] : nullptr;
    if (cp < v) hi = mid - 1; else lo = mid + 1;
  }
  return nullptr;
}
#endif
