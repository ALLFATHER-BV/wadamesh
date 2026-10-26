// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The text an avatar draws from a name: the Initials look's letters and the chat
// icon taken from the first emoji in a name (#640). Pure, so the host tests can run
// it (test/test_avatar_text.cpp); UITask.cpp passes in what the font and the baked
// emoji set can draw.
//
// #674: a name that opens with emoji, such as a flag and an island, came out blank.
// The emoji pick took the first code point with ANY row in the emoji table, and the
// table maps the invisible joiners (U+200D, the U+FE0E/FE0F variation selectors) to
// a 2 x 2 empty image so text never shows them; so the icon was that empty image.
// The initials took the first two code points whatever they were, so the same name
// showed flag halves the font does not have.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Utf8Text.h"

namespace AvatarText {

// The next code point at text[i], advancing i. A malformed byte is skipped and
// returns 0, so a broken name still walks to its end.
inline uint32_t next(const char* text, size_t len, size_t& i) {
  if (i >= len) return 0;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(text) + i;
  const size_t n = Utf8Text::sequenceLength(b, len - i);
  if (!n) { ++i; return 0; }
  i += n;
  if (n == 1) return b[0];
  if (n == 2) return ((uint32_t)(b[0] & 0x1F) << 6) | (b[1] & 0x3F);
  if (n == 3) return ((uint32_t)(b[0] & 0x0F) << 12) | ((uint32_t)(b[1] & 0x3F) << 6) | (b[2] & 0x3F);
  return ((uint32_t)(b[0] & 0x07) << 18) | ((uint32_t)(b[1] & 0x3F) << 12) |
         ((uint32_t)(b[2] & 0x3F) << 6) | (b[3] & 0x3F);
}

inline size_t encode(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// Uppercase in ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic, which covers
// every language the firmware ships. Anything else (Arabic has no case) is kept.
inline uint32_t upper(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 0x20;
  if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 0x20;
  if (c >= 0x100 && c <= 0x17F) {
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
    return (c & 1) ? c - 1 : c;
  }
  if (c >= 0x3B1 && c <= 0x3C9) return c == 0x3C2 ? 0x3A3 : c - 0x20;
  if (c >= 0x430 && c <= 0x44F) return c - 0x20;
  if (c >= 0x450 && c <= 0x45F) return c - 0x50;
  return c;
}

// A letter or digit a person would read as part of the name. Not punctuation,
// symbols, combining accents, joiners, variation selectors or emoji.
inline bool isLetter(uint32_t c) {
  if (c < 0x80) return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
  if (c < 0xC0 || c == 0xD7 || c == 0xF7) return false;   // Latin-1 signs: (c), degree, x, ...
  if (c >= 0x300 && c <= 0x36F) return false;              // combining accents
  if (c < 0x2000) return true;                              // Latin, Greek, Cyrillic, Arabic, ...
  return (c >= 0x3040 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3);   // kana, CJK, Hangul
}

// The plain letter under a Latin-1 accented one ("A" for A-tilde), 0 when none.
inline uint32_t baseLetter(uint32_t c) {
  static const char kBase[] = "AAAAAAACEEEEIIIIDNOOOOO\0OUUUUYTsaaaaaaaceeeeiiiidnooooo\0ouuuuyty";
  if (c < 0xC0 || c > 0xFF) return 0;
  return (uint8_t)kBase[c - 0xC0];
}

typedef bool (*HasGlyph)(uint32_t cp, void* ctx);

// The Initials look's letters: the first two letters or digits in the name, in
// capitals. One the font cannot draw becomes its plain letter (A for A-tilde) when
// the font has that, else is left out for the next. Writes "?" when the name has
// none at all, so the ball is never blank. Returns the bytes written.
inline size_t initials(const char* name, char* out, size_t cap, HasGlyph has, void* ctx) {
  if (!out || cap < 2) return 0;
  size_t o = 0;
  int got = 0;
  const size_t len = name ? strlen(name) : 0;
  for (size_t i = 0; i < len && got < 2;) {
    const uint32_t c = next(name, len, i);
    if (!isLetter(c)) continue;
    uint32_t pick = upper(c);
    if (has && !has(pick, ctx)) {
      if (has(c, ctx)) pick = c;
      else {
        const uint32_t b = baseLetter(pick);
        if (!b || !has(b, ctx)) continue;
        pick = b;
      }
    }
    char enc[4];
    const size_t n = encode(pick, enc);
    if (o + n + 1 > cap) break;
    memcpy(out + o, enc, n);
    o += n;
    ++got;
  }
  if (!o) out[o++] = '?';
  out[o] = 0;
  return o;
}

// The first emoji in a name the device can draw, copied into out (#640). Only code
// points from U+2000 up count (a (c) sign is not an icon), and only ones `visible`
// accepts: the caller rejects the invisible joiner rows. False when there is none.
inline bool firstEmoji(const char* name, char* out, size_t cap, HasGlyph visible, void* ctx) {
  if (!name || !out || cap < 5) return false;
  const size_t len = strlen(name);
  for (size_t i = 0; i < len;) {
    const size_t at = i;
    const uint32_t c = next(name, len, i);
    if (c < 0x2000 || !visible(c, ctx)) continue;
    const size_t n = i - at;
    if (n >= cap) return false;
    memcpy(out, name + at, n);
    out[n] = 0;
    return true;
  }
  return false;
}

}  // namespace AvatarText
