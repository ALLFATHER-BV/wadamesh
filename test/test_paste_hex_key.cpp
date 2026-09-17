// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the pasted-key extraction (#526, reported by Steve8677: a
// channel secret copied out of a message would not fit the field).
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_paste_hex_key.cpp -o /tmp/paste && /tmp/paste

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/PasteHexKey.h"

static const char* K32 = "0123456789abcdef0123456789abcdef";                          // channel secret
static const char* K64 = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";  // public key

int main() {
  char buf[65];
  char msg[300];

  // What people actually copy: the key with a sentence around it.
  snprintf(msg, sizeof msg, "Join my channel! secret %s see you there", K32);
  assert(strcmp(PasteHexKey::extract(msg, 32, buf, sizeof buf), K32) == 0);
  snprintf(msg, sizeof msg, "add me: %s (thanks)", K64);
  assert(strcmp(PasteHexKey::extract(msg, 64, buf, sizeof buf), K64) == 0);

  // The key on its own is returned unchanged in substance.
  assert(strcmp(PasteHexKey::extract(K32, 32, buf, sizeof buf), K32) == 0);

  // Either end of the string, and short hex words either side of the key: the
  // exact-run pass has to survive "cafe" and "babe" reading as hex.
  snprintf(msg, sizeof msg, "%s at start", K32);
  assert(strcmp(PasteHexKey::extract(msg, 32, buf, sizeof buf), K32) == 0);
  snprintf(msg, sizeof msg, "at end %s", K32);
  assert(strcmp(PasteHexKey::extract(msg, 32, buf, sizeof buf), K32) == 0);
  snprintf(msg, sizeof msg, "cafe %s babe", K32);
  assert(strcmp(PasteHexKey::extract(msg, 32, buf, sizeof buf), K32) == 0);

  // A key copied in groups of four.
  assert(strcmp(PasteHexKey::extract("0123 4567 89ab cdef 0123 4567 89ab cdef",
                                     32, buf, sizeof buf), K32) == 0);

  // Exactly the right length or nothing. A public key must not answer a request
  // for a channel secret with its first half, one digit too many is not a key,
  // and two keys glued together are not one key either.
  snprintf(msg, sizeof msg, "secret %s trailing", K64);
  assert(strcmp(PasteHexKey::extract(msg, 32, buf, sizeof buf), msg) == 0);
  snprintf(msg, sizeof msg, "key %sa end", K32);
  assert(strcmp(PasteHexKey::extract(msg, 32, buf, sizeof buf), msg) == 0);
  snprintf(msg, sizeof msg, "%s%s", K32, K32);
  assert(strcmp(PasteHexKey::extract(msg, 32, buf, sizeof buf), msg) == 0);

  // Nothing to find, and the ordinary fields that ask for no key at all: the
  // clipboard has to come back untouched, pointer and all.
  assert(strcmp(PasteHexKey::extract("nothing here", 32, buf, sizeof buf), "nothing here") == 0);
  assert(strcmp(PasteHexKey::extract("", 32, buf, sizeof buf), "") == 0);
  const char* plain = "whatever";
  assert(PasteHexKey::extract(plain, 0, buf, sizeof buf) == plain);

  // A destination that cannot hold the key plus its terminator is left alone.
  char tight[32];
  assert(strcmp(PasteHexKey::extract(K32, 32, tight, sizeof tight), K32) == 0);

  // Defensive: no clipboard at all.
  assert(PasteHexKey::extract(nullptr, 32, buf, sizeof buf) == nullptr);
  assert(!PasteHexKey::find(nullptr, 32, false, buf));

  printf("test_paste_hex_key: all assertions passed\n");
  return 0;
}
