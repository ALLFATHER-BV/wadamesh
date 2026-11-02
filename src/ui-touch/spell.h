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
//   F      = frequency, 0 (rarest) .. 255 (most common), log-scaled.
// Every kBlockWords-th entry starts a block and stores its word in full
// (prefix 0), and blocks[] holds each block's byte offset, so a lookup is a
// binary search over blocks and a short scan inside one -- straight from
// flash, nothing unpacked into RAM.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace spell {

constexpr int kBlockWords = 32;
constexpr int kMaxWord = 24;      // longest word checked, in bytes
constexpr int kMaxSuggest = 3;

struct Dict {
    const uint8_t *data;
    uint32_t size;          // bytes in data
    const uint32_t *blocks; // byte offset of each block's first entry
    uint32_t blockCount;
    uint32_t wordCount;
};

// The compiled-in English list (spell_en.cpp).
const Dict &english();

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
