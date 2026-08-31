// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>

#include "ui-touch/SenderExtField.h"

// The on-disk shape: base is the frozen v6 field (24 chars + NUL), ext is the appended
// tail, and together they hold MAX_SENDER_NAME == 31 chars.
static const size_t kBaseCap = 25;
static const size_t kExtCap = 7;
static const size_t kOutCap = 32;

int main() {
  char base[kBaseCap], ext[kExtCap], out[kOutCap];

  // Short name: fits the base field, ext stays empty.
  SenderExtField::store("nick", base, sizeof base, ext, sizeof ext);
  assert(strcmp(base, "nick") == 0);
  for (size_t i = 0; i < kExtCap; ++i) assert(ext[i] == '\0');
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(strcmp(out, "nick") == 0);

  // Exactly 24: fills the base field with no spill. The tail must stay empty, or the
  // "base is full" test below would resurrect garbage.
  const char* exactly24 = "123456789012345678901234";
  assert(strlen(exactly24) == 24);
  SenderExtField::store(exactly24, base, sizeof base, ext, sizeof ext);
  assert(strcmp(base, exactly24) == 0);
  for (size_t i = 0; i < kExtCap; ++i) assert(ext[i] == '\0');
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(strcmp(out, exactly24) == 0);

  // A full 31-char name — the case the whole split exists for. Round-trips intact.
  const char* full = "Ouderkerk Repeater Node Twelve";
  assert(strlen(full) == 30);
  SenderExtField::store(full, base, sizeof base, ext, sizeof ext);
  assert(strncmp(base, full, 24) == 0 && base[24] == '\0');
  assert(memcmp(ext, full + 24, 6) == 0);
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(strcmp(out, full) == 0);

  // The longest possible name fills ext completely, leaving it unterminated. load() is
  // bounded by the field size, so it must not read past.
  const char* longest = "1234567890123456789012345678901";
  assert(strlen(longest) == 31);
  SenderExtField::store(longest, base, sizeof base, ext, sizeof ext);
  assert(memcmp(ext, longest + 24, kExtCap) == 0);   // no NUL anywhere in ext
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(strcmp(out, longest) == 0);

  // An OLD record: base written by a pre-widening build, ext zero-filled by the loader's
  // prefix read. Must yield exactly the stored short name.
  memset(ext, 0, sizeof ext);
  memset(base, 0, sizeof base);
  strcpy(base, "olde name");
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(strcmp(out, "olde name") == 0);

  // Garbage in ext behind a NON-full base must be ignored — that is the corrupt-record
  // case, and appending it would splice junk onto a valid short name.
  memset(ext, 'X', sizeof ext);
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(strcmp(out, "olde name") == 0);

  // An unterminated base field (corrupt record) must not read into the message body that
  // follows it on disk: the copy stops at the field width.
  char raw_base[kBaseCap];
  memset(raw_base, 'A', sizeof raw_base);   // no NUL at all
  memset(ext, 0, sizeof ext);
  SenderExtField::load(raw_base, sizeof raw_base, ext, sizeof ext, out, sizeof out);
  assert(strlen(out) == 24);

  // A multi-byte character straddling the boundary: split anywhere, rejoin exact. The
  // halves are never used apart, so the byte-level split is deliberately not aligned.
  const char* utf8 = "Ouderkerkkkkkkkkkkkkkkk\xE2\x98\x80";   // 23 ASCII + 3-byte U+2600
  assert(strlen(utf8) == 26);
  SenderExtField::store(utf8, base, sizeof base, ext, sizeof ext);
  assert((unsigned char)base[23] == 0xE2);   // sequence is cut mid-character on disk
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(strcmp(out, utf8) == 0);            // and comes back whole

  // Degenerate inputs.
  SenderExtField::store(nullptr, base, sizeof base, ext, sizeof ext);
  assert(base[0] == '\0');
  SenderExtField::load(base, sizeof base, ext, sizeof ext, out, sizeof out);
  assert(out[0] == '\0');
  return 0;
}
