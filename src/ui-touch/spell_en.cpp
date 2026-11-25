// SPDX-License-Identifier: GPL-3.0-or-later
// The compiled-in English word list. Kept out of spell.cpp so the host tests
// can link the engine without the generated header. Only built where
// CAP_SPELLCHECK is on (device_caps.h): elsewhere nothing references it, and
// the list stays out of the image.
//
// The list is stored deflated (~66 KB of flash) and inflated into PSRAM (~112 KB
// plus a 3 KB block index) the first time a word is checked, so a board that
// never types English never pays the RAM.
#include "device_caps.h"

#if CAP_SPELLCHECK
#include "spell.h"
#include "spell_dict_en.h"

#include <stdlib.h>

// The ROM's tinfl on the ESP32 targets (no flash cost); the web build uses
// upstream miniz, which has the same API (as PmTiles.cpp does).
#if __has_include("rom/miniz.h")
  #include "rom/miniz.h"
  #define SPELL_HAVE_TINFL 1
#elif __has_include("miniz.h")
  #include "miniz.h"
  #define SPELL_HAVE_TINFL 1
#else
  #define SPELL_HAVE_TINFL 0
#endif

#if defined(ESP_PLATFORM) || defined(ESP32)
  #include "esp_heap_caps.h"
  static void *spellAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  static void spellFree(void *p) { heap_caps_free(p); }
#else
  static void *spellAlloc(size_t n) { return malloc(n); }
  static void spellFree(void *p) { free(p); }
#endif

namespace spell {

const Dict &english() {
    static Dict d = {nullptr, 0, nullptr, 0, 0};
    static bool tried = false;
    if (tried) return d;
    tried = true;   // one attempt: a failure would only fail again on every word
#if SPELL_HAVE_TINFL
    uint8_t *data = (uint8_t *)spellAlloc(kSpellEnSize);
    uint32_t *blocks = (uint32_t *)spellAlloc(kSpellEnBlockCount * sizeof(uint32_t));
    // ~11 KB of state: never on the UI task's stack.
    tinfl_decompressor *t = (tinfl_decompressor *)spellAlloc(sizeof(tinfl_decompressor));
    bool ok = false;
    if (data && blocks && t) {
        tinfl_init(t);
        size_t in = kSpellEnZSize, out = kSpellEnSize;
        ok = tinfl_decompress(t, kSpellEnZ, &in, data, data, &out,
                              TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) == TINFL_STATUS_DONE &&
             out == kSpellEnSize &&
             buildBlocks(data, kSpellEnSize, kSpellEnWords, blocks, kSpellEnBlockCount) ==
                 kSpellEnBlockCount;
    }
    if (t) spellFree(t);
    if (!ok) {
        if (data) spellFree(data);
        if (blocks) spellFree(blocks);
        return d;
    }
    d = {data, kSpellEnSize, blocks, kSpellEnBlockCount, kSpellEnWords};
#endif
    return d;
}

}  // namespace spell
#endif  // CAP_SPELLCHECK
