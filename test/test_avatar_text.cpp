// SPDX-License-Identifier: GPL-3.0-or-later
// Avatar text (#674): the initials and the emoji icon taken from a name.
// Build: g++ -std=c++17 -I src -o /tmp/test_avatar_text test/test_avatar_text.cpp
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <set>

#include "ui-touch/AvatarText.h"

// A font like the device's: ASCII and Latin-1, no flags or emoji.
static bool latinFont(uint32_t cp, void*) { return cp < 0x100; }
// A narrower one: ASCII only, so an accented letter needs its plain letter.
static bool asciiFont(uint32_t cp, void*) { return cp < 0x80; }
// The firmware's chained faces, which also carry Greek and Cyrillic.
static bool wideFont(uint32_t cp, void*) { return cp < 0x530; }

// The baked emoji set as the firmware has it: the rows that matter here. The joiner
// and the variation selectors are in the table as an empty 2 x 2 image (so text never
// shows them); the visible check is what the UI passes, true for a real picture only.
static const std::set<uint32_t> kTable = {0x200D, 0xFE0E, 0xFE0F, 0x1F1F8, 0x1F600, 0x1F44D};
static const std::set<uint32_t> kInvisible = {0x200D, 0xFE0E, 0xFE0F, 0x1F1F8};
static bool anyRow(uint32_t cp, void*) { return kTable.count(cp) != 0; }   // the old check
static bool visibleRow(uint32_t cp, void*) { return kTable.count(cp) && !kInvisible.count(cp); }

static std::string ini(const char* name, AvatarText::HasGlyph has = latinFont) {
  char out[12];
  AvatarText::initials(name, out, sizeof out, has, nullptr);
  return out;
}

// The names from the report: a flag (U+1F1F5 U+1F1F9) and an island with its emoji
// variation selector (U+1F3DD U+FE0F), and a man (U+1F468) after the name.
static const char kTristaoEdc[] = "\xF0\x9F\x87\xB5\xF0\x9F\x87\xB9\xF0\x9F\x8F\x9D\xEF\xB8\x8F Trist\xC3\xA3o EDC";
static const char kTristaoMobile[] = "Trist\xC3\xA3o Mobile \xF0\x9F\x91\xA8";
static const char kTristaoZwj[] = "Trist\xC3\xA3o \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB";   // man + ZWJ + laptop

int main() {
  char eb[20];

  // The bug, reproduced with the old rule (any table row counts): the island's
  // variation selector is the "first emoji", an empty image, so the tile is blank.
  assert(AvatarText::firstEmoji(kTristaoEdc, eb, sizeof eb, anyRow, nullptr));
  assert(!strcmp(eb, "\xEF\xB8\x8F"));
  assert(AvatarText::firstEmoji(kTristaoZwj, eb, sizeof eb, anyRow, nullptr));
  assert(!strcmp(eb, "\xE2\x80\x8D"));
  // Fixed: only a visible picture is an icon; these names keep their dot pattern.
  assert(!AvatarText::firstEmoji(kTristaoEdc, eb, sizeof eb, visibleRow, nullptr));
  assert(!AvatarText::firstEmoji(kTristaoMobile, eb, sizeof eb, visibleRow, nullptr));
  assert(!AvatarText::firstEmoji(kTristaoZwj, eb, sizeof eb, visibleRow, nullptr));
  assert(!AvatarText::firstEmoji("\xF0\x9F\x87\xB8\xF0\x9F\x87\xAA Sweden", eb, sizeof eb, visibleRow, nullptr));
  // A real emoji still becomes the icon (#640), after a joiner too.
  assert(AvatarText::firstEmoji("Base \xEF\xB8\x8F\xF0\x9F\x98\x80 camp", eb, sizeof eb, visibleRow, nullptr));
  assert(!strcmp(eb, "\xF0\x9F\x98\x80"));
  assert(!AvatarText::firstEmoji("\xC2\xA9 corp", eb, sizeof eb, visibleRow, nullptr));
  assert(!AvatarText::firstEmoji("Plain name", eb, sizeof eb, visibleRow, nullptr));

  // Initials: whole code points, in capitals, emoji and symbols skipped.
  assert(ini("\xC3\xA3na") == "\xC3\x83N");                 // "ãna" -> "ÃN"
  assert(ini("Jo\xC3\xA3o") == "JO");
  assert(ini("Jo""a\xCC\x83o") == "JO");                    // a + combining tilde
  assert(ini("\xC3\x89mile") == "\xC3\x89M");               // "Émile" -> "ÉM"
  assert(ini("Zo\xC3\xAB") == "ZO");
  assert(ini(kTristaoEdc) == "TR");                         // was the two flag halves
  assert(ini(kTristaoMobile) == "TR");
  assert(ini("#hiking") == "HI");
  assert(ini("  [base] camp") == "BA");
  assert(ini("42 Node") == "42");
  assert(ini("\xD0\xB4\xD0\xB8\xD0\xBC\xD0\xB0", wideFont) == "\xD0\x94\xD0\x98");   // Cyrillic "дима" -> "ДИ"
  // A font without the accented capital: its plain letter.
  assert(ini("\xC3\xA3na", asciiFont) == "AN");
  // Nothing to draw: "?", never blank.
  assert(ini("\xF0\x9F\x87\xB5\xF0\x9F\x87\xB9") == "?");
  assert(ini("") == "?");
  assert(ini(nullptr) == "?");
  assert(ini("\xFF\xFE") == "?");                          // malformed bytes
  // Two letters only, and never past the buffer.
  char small[4];
  AvatarText::initials("\xC3\xA3\xC3\xA9", small, sizeof small, latinFont, nullptr);
  assert(!strcmp(small, "\xC3\x83"));                      // the second does not fit

  // The uppercase table the section labels use, moved here unchanged.
  assert(AvatarText::upper('a') == 'A' && AvatarText::upper(0xE3) == 0xC3 && AvatarText::upper(0xF7) == 0xF7);
  assert(AvatarText::upper(0x101) == 0x100 && AvatarText::upper(0x3C2) == 0x3A3 && AvatarText::upper(0x451) == 0x401);

  puts("avatar text: emoji icon skips joiners, initials are whole letters, never blank PASS");
}
