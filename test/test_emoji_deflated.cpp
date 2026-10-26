// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Tanmatsu ships its colour emoji deflated (variants/tanmatsu/emoji_deflated.cpp,
// header generated from src/ui-touch/emoji_data.c by scripts/build/gen-emoji-deflated.py).
// This checks the deflated path against the raw one for EVERY codepoint up to
// U+10FFFF: same hits and misses, same header, same size, same pixels. Then the
// failure paths: out of memory (no glyph, retried later) and a blob that will not
// inflate (no glyph, never retried).
//
// Build (scripts/run-host-tests.sh does this): generate the header into a directory
// on the include path, then
//   g++ -std=c++17 -I test/emoji_fakes -I <gen dir> -I src -I src/ui-touch \
//       test/test_emoji_deflated.cpp -lz

#include <assert.h>
#include <stdio.h>
#include <string.h>

// The raw table, renamed so both implementations link into one binary.
#define emojiGlyphLookup rawEmojiGlyphLookup
#define kEmojiGlyphCount rawEmojiGlyphCount
#include "ui-touch/emoji_data.c"
#undef emojiGlyphLookup
#undef kEmojiGlyphCount

#define WADA_EMOJI_DEFLATED 1
#include "../variants/tanmatsu/emoji_deflated.cpp"

static bool sameImage(const lv_img_dsc_t* a, const lv_img_dsc_t* b) {
  return a->header.cf == b->header.cf && a->header.w == b->header.w && a->header.h == b->header.h &&
         a->header.always_zero == b->header.always_zero && a->data_size == b->data_size &&
         memcmp(a->data, b->data, a->data_size) == 0;
}

int main() {
  assert(kEmojiGlyphCount == rawEmojiGlyphCount);

  // A miss never inflates (or even allocates): presence checks stay free.
  assert(emojiGlyphLookup(0x41) == nullptr);
  assert(g_fake_allocs == 0 && s_emoji_dsc == nullptr);

  // Out of memory: a known emoji has no glyph, nothing is marked broken, and the
  // next lookup with memory available succeeds.
  g_fake_alloc_fail = 1;
  assert(emojiGlyphLookup(0x1F60A) == nullptr);
  assert(!s_emoji_broken && s_emoji_dsc == nullptr);
  g_fake_alloc_fail = 0;

  // Every codepoint: the deflated lookup matches the raw one exactly.
  int hits = 0;
  for (uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
    const lv_img_dsc_t* raw = rawEmojiGlyphLookup(cp);
    const lv_img_dsc_t* z = emojiGlyphLookup(cp);
    if (!raw) { assert(!z); continue; }
    assert(z);
    if (!sameImage(raw, z)) { printf("U+%X differs\n", (unsigned)cp); return 1; }
    ++hits;
  }
  printf("%d codepoints match\n", hits);
  assert(hits == (int)(sizeof(kGlyphs) / sizeof(kGlyphs[0])));

  // Codepoints that share one image in the source still share one image here.
  assert(emojiGlyphLookup(0xFE0F) == emojiGlyphLookup(0x200D));
  // Inflated exactly once: pixels, descriptors and the decompressor state.
  assert(g_fake_allocs == 3);

  // A blob that will not inflate: no glyph, and it is not inflated again per draw.
  heap_caps_free((void*)s_emoji_dsc[0].data);   // the pixel block (image 0 sits at offset 0)
  heap_caps_free(s_emoji_dsc);
  s_emoji_dsc = nullptr;
  g_fake_tinfl_corrupt = 1;
  const int before = g_fake_allocs;
  assert(emojiGlyphLookup(0x1F60A) == nullptr);
  assert(s_emoji_broken);
  assert(emojiGlyphLookup(0x1F60A) == nullptr);
  assert(g_fake_allocs == before + 3);   // one attempt only
  puts("ok");
  return 0;
}
