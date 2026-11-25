// SPDX-License-Identifier: GPL-3.0-or-later
// Spell suggestions for the compose box: is a word known, and if not, which
// known words is it most likely a typo of.
//
// Free of Arduino and LVGL so test/test_spell.cpp can check it on the host.
// The English word list is compiled in by scripts/build/gen_spell_dict.py and reached
// through spell::english(), which lives in spell_en.cpp -- a separate file so
// the tests can hand the engine a dictionary of their own.
//
// Dictionary format (what gen_spell_dict.py writes, what the engine reads).
// Words are lowercase ASCII letters plus the apostrophe, sorted by bytes, and
// front-coded: each entry stores only what differs from the word before it.
//   entry  = H [L] suffix... F
//   H      = (shared prefix length << 4) | suffix length, both 0..15. A suffix
//            of 15 or more writes 15 here and the real length in the byte L.
//   F      = frequency, 0 (rarest) .. kFreqMax (most common), log-scaled.
// Every kBlockWords-th entry starts a block and stores its word in full
// (prefix 0), and blocks[] holds each block's byte offset, so a lookup is a
// binary search over blocks and a short scan inside one.
//
// The English list ships deflated (66 KB of flash instead of 115 KB) and is
// inflated into PSRAM the first time a word is checked (spell_en.cpp), where
// buildBlocks() rebuilds the block index from it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace spell {

constexpr int kBlockWords = 32;
constexpr int kMaxWord = 24;      // longest word checked, in bytes
constexpr int kMaxSuggest = 3;
constexpr int kFreqMax = 15;      // F's top value (gen_spell_dict.py FREQ_MAX)

struct Dict {
    const uint8_t *data;
    uint32_t size;          // bytes in data
    const uint32_t *blocks; // byte offset of each block's first entry
    uint32_t blockCount;
    uint32_t wordCount;
};

// The compiled-in English list (spell_en.cpp), inflated on the first call. An
// empty Dict (data == nullptr) when that failed (no PSRAM): nothing is known and
// nothing is suggested then, and it is not tried again.
const Dict &english();

// Walks a list of `words` entries in `size` bytes and writes the byte offset of
// every kBlockWords-th entry to blocks[] (at most cap). Returns how many it wrote,
// or 0 when the list is malformed (an entry runs past the end, or a block start
// does not stand alone with prefix 0).
uint32_t buildBlocks(const uint8_t *data, uint32_t size, uint32_t words,
                     uint32_t *blocks, uint32_t cap);

// Whether a token is a word worth checking at all. Not: shorter than 2 or
// longer than kMaxWord - 2 letters, anything with a digit or a non-ASCII byte,
// all capitals (LORA, SOS -- acronyms), capitals after the first letter
// (LongFast, iPhone), or an apostrophe at either end.
bool shouldCheck(const char *w, size_t n);

// Whether the word is in the list, ignoring case. "Mike's" is known when
// "mike" is.
bool known(const Dict &d, const char *w, size_t n);

// Up to maxOut known words within two edits (insert, delete, substitute, swap
// two neighbours) of w, closest first and most common first within a
// distance. Two-edit matches only for words of four letters or more: shorter
// ones have too many to be a useful guess. Written with the case of w applied
// (see applyCase). Returns how many were written.
int suggest(const Dict &d, const char *w, size_t n,
            char out[][kMaxWord + 1], int maxOut);

// Give a lowercase suggestion the shape of what was typed: "Teh" -> "The",
// "TEH" -> "THE". Also "i'm" -> "I'm" however it was typed.
void applyCase(const char *typed, size_t n, char *sugg);

}  // namespace spell
