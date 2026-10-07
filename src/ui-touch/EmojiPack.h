#pragma once
#include <FS.h>
#include "lvgl.h"

// Optional SD-card emoji pack: extra colour glyphs plus the picker entries that
// expose them, read from /emoji/emoji.pack on the card root. Built by
// scripts/build/gen-emoji-pack.py. The pack is untrusted input, so the loader
// validates every offset before use and falls back to the baked-in set (see
// emoji_data.h) whenever the card, the file, or its contents are unusable.
//
// A full pack is over a megabyte and the card runs at 4 MHz, so the read is
// pumped from the UI loop in budgeted slices rather than blocking boot. The
// table is published in one step at the end, so a reader on the other core sees
// either no pack at all or a complete one.

// Picker category of a pack item. The values are part of the on-disk format.
// 0-7 are the groups k_emoji_items has always had; 8+ exist only in packs.
// HEARTS stays because the built-in list groups them separately, but the picker
// files them under Smileys, where Unicode puts them.
enum : uint8_t
{
    EMOJI_CAT_FACES = 0,
    EMOJI_CAT_GESTURES = 1,
    EMOJI_CAT_HEARTS = 2,
    EMOJI_CAT_SYMBOLS = 3,
    EMOJI_CAT_OBJECTS = 4,
    EMOJI_CAT_ANIMALS = 5,
    EMOJI_CAT_ACTIVITY = 6,
    EMOJI_CAT_SPECIAL = 7,
    EMOJI_CAT_FOOD = 8,
    EMOJI_CAT_PLACES = 9,
    EMOJI_CAT_FLAGS = 10,
    EMOJI_CAT_EXTRA = 255, // no home category — appended after every other group
};

// Takes over `f`. False means there is nothing to load and no pump is needed.
bool emojiPackBegin(File f);
// Reads up to `budget` bytes. True once the load has finished, either way.
bool emojiPackPump(size_t budget);
bool emojiPackLoading();
bool emojiPackLoaded();

// Baked image for `cp`, or NULL when the pack has no glyph for it. Callers
// consult emojiGlyphLookup() first so an SD pack can never shadow shipped art.
const lv_img_dsc_t *emojiPackLookup(uint32_t cp);

int emojiPackItemCount();
const char *emojiPackItem(int i); // NUL-terminated UTF-8, may be a multi-codepoint sequence
uint8_t emojiPackItemCat(int i);
