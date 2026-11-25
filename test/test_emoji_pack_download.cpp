// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for EmojiPackDownload.h (#831).
// Build: g++ -std=c++17 -I src -o /tmp/t test/test_emoji_pack_download.cpp
#include "ui-touch/EmojiPackDownload.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

struct FakeFs {
  std::map<std::string, std::string> files;
  bool fail_temp_rename = false;

  bool exists(const char* path) { return files.count(path) != 0; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (fail_temp_rename && std::string(from) == EmojiPackDownload::kTempPath) return false;
    if (!exists(from) || exists(to)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};

static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

// A header for a pack of `glyphs` glyphs and `idxn` index entries, laid out the
// way gen-emoji-pack.py writes it; returns the file size it implies.
static uint32_t makeHeader(uint8_t* h, uint32_t glyphs, uint32_t idxn) {
  const uint32_t ioff = 32 + idxn * 12, ilen = 2 + 3 * idxn;
  const uint32_t boff = ioff + ilen, blen = glyphs * 768;
  put32(h, 0x50454D57u); put16(h + 4, 1); put16(h + 6, 16);
  put32(h + 8, glyphs); put32(h + 12, idxn);
  put32(h + 16, ioff); put32(h + 20, ilen); put32(h + 24, boff); put32(h + 28, blen);
  return boff + blen;
}

int main() {
  using namespace EmojiPackDownload;

  // headerOk
  uint8_t h[32];
  const uint32_t size = makeHeader(h, 100, 120);
  assert(headerOk(h, size));
  assert(!headerOk(h, size - 1));          // truncated download
  assert(!headerOk(h, 31));
  assert(!headerOk(nullptr, size));
  assert(!headerOk(h, kMaxFile + 1));
  { uint8_t b[32]; memcpy(b, h, 32); b[0] = 'X'; assert(!headerOk(b, size)); }       // not a pack (an HTML error page)
  { uint8_t b[32]; memcpy(b, h, 32); put16(b + 4, 2); assert(!headerOk(b, size)); } // other version
  { uint8_t b[32]; memcpy(b, h, 32); put16(b + 6, 24); assert(!headerOk(b, size)); }
  { uint8_t b[32]; memcpy(b, h, 32); put32(b + 8, 0); assert(!headerOk(b, size)); }
  { uint8_t b[32]; memcpy(b, h, 32); put32(b + 28, 99 * 768); assert(!headerOk(b, size)); }
  { uint8_t b[32]; memcpy(b, h, 32); put32(b + 16, 8); assert(!headerOk(b, size)); }
  { uint8_t b[32]; memcpy(b, h, 32); put32(b + 24, 0xFFFFFFF0u); assert(!headerOk(b, size)); } // no wrap

  // locations / exists
  FakeFs card, data;
  assert(!exists(&card, &data));
  assert(!exists<FakeFs>(nullptr, nullptr));
  card.files[kRootPath] = "x";
  assert(locations(&card, &data) == 2);
  card.files.clear();
  data.files[kPath] = "x";
  assert(locations(&card, &data) == 4);
  assert(locations(&card, &card) == 0);   // the data partition is the card: counted once
  data.files.clear();

  // install: nothing downloaded
  assert(!install(card));
  // fresh install
  card.files[kTempPath] = "v1";
  assert(install(card));
  assert(card.files[kPath] == "v1" && !card.exists(kTempPath) && !card.exists(kBackupPath));
  // replace
  card.files[kTempPath] = "v2";
  assert(install(card));
  assert(card.files[kPath] == "v2" && !card.exists(kBackupPath));
  // a failed move puts the old pack back
  card.files[kTempPath] = "v3";
  card.fail_temp_rename = true;
  assert(!install(card));
  assert(card.files[kPath] == "v2" && !card.exists(kBackupPath));
  card.fail_temp_rename = false;
  // a stale backup does not block an install
  card.files[kBackupPath] = "old";
  assert(install(card));
  assert(card.files[kPath] == "v3" && !card.exists(kBackupPath));

  // restoreBackup: only when the pack itself is missing
  card.files.clear();
  card.files[kBackupPath] = "v2";
  assert(restoreBackup(card) && card.files[kPath] == "v2" && !card.exists(kBackupPath));
  card.files[kBackupPath] = "older";
  assert(restoreBackup(card) && card.files[kPath] == "v2");

  printf("test_emoji_pack_download: all assertions passed\n");
  return 0;
}
