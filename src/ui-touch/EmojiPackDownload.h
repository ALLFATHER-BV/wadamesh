// SPDX-License-Identifier: GPL-3.0-or-later
// Emoji pack download over Wi-Fi (#831): where the pack lives, the checks a
// downloaded file passes before it is installed, and the install itself.
//
// The download lands in kTempPath on the SD card and is checked there, never by
// loading it: emojiPackBegin() frees the live pack's tables, and avatars and
// labels drawn with a pack emoji still point into them. The new pack is moved in
// place on the card and is read at the next start.
//
// Header-only and free of Arduino/LVGL so the host test can include it.
// Build the test: g++ -std=c++17 -I src -o /tmp/t test/test_emoji_pack_download.cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace EmojiPackDownload {

static constexpr char kPath[] = "/emoji/emoji.pack";
static constexpr char kRootPath[] = "/emoji.pack";
static constexpr char kTempPath[] = "/emoji/emoji.pack.tmp";
static constexpr char kBackupPath[] = "/emoji/emoji.pack.bak";

// Plain http on purpose: on-device TLS needs ~30 KB of internal heap for the
// handshake, and the pack is checked below before it is used.
static constexpr char kUrl[] = "http://wadamesh.com/emoji/emoji.pack";

// EmojiPack.cpp's format: a 32-byte header, then the tables it points at.
static constexpr uint32_t kHeaderSize = 32;
static constexpr uint32_t kMaxFile = 2u * 1024u * 1024u;   // EmojiPack.cpp's kMaxFile

// Whether a file of `size` bytes starting with header `h` is a pack the loader
// can take: magic "WMEP", version 1, 16 px glyphs, a blob section that matches
// the glyph count, and every section inside the file. The loader checks every
// entry again at boot; this keeps a truncated or wrong download off the card.
inline bool headerOk(const uint8_t* h, uint32_t size) {
  auto rd16 = [&](int o) { return (uint32_t)h[o] | ((uint32_t)h[o + 1] << 8); };
  auto rd32 = [&](int o) { return rd16(o) | (rd16(o + 2) << 16); };
  if (!h || size < kHeaderSize || size > kMaxFile) return false;
  if (rd32(0) != 0x50454D57u || rd16(4) != 1 || rd16(6) != 16) return false;
  const uint32_t glyphs = rd32(8), idxn = rd32(12);
  const uint32_t ioff = rd32(16), ilen = rd32(20), boff = rd32(24), blen = rd32(28);
  if (glyphs == 0 || idxn == 0) return false;
  if (blen % (16u * 16u * 3u) || blen / (16u * 16u * 3u) != glyphs) return false;
  if (kHeaderSize + (uint64_t)idxn * 12u > size) return false;
  if (ioff < kHeaderSize || (uint64_t)ioff + ilen > size) return false;
  if (boff < kHeaderSize || (uint64_t)boff + blen > size) return false;
  return true;
}

// Which pack files exist: bit 0 /emoji/emoji.pack and bit 1 /emoji.pack on the
// card, bits 2 and 3 the same on the data partition when that is not the card.
template <typename Fs>
unsigned locations(Fs* card, Fs* data) {
  unsigned found = 0;
  if (card && card->exists(kPath)) found |= 1;
  if (card && card->exists(kRootPath)) found |= 2;
  if (data && data != card && data->exists(kPath)) found |= 4;
  if (data && data != card && data->exists(kRootPath)) found |= 8;
  return found;
}

template <typename Fs>
bool exists(Fs* card, Fs* data) { return locations(card, data) != 0; }

// Moves the checked download (kTempPath) to kPath on the card. The pack it
// replaces is kept as kBackupPath until the new one is in place, and put back if
// that fails; a backup left by an install that was cut off (power loss) is
// restored at boot when kPath is missing (restoreBackup), and is stale otherwise.
template <typename Fs>
bool install(Fs& card) {
  if (!card.exists(kTempPath)) return false;
  if (card.exists(kBackupPath)) card.remove(kBackupPath);
  const bool replace = card.exists(kPath);
  if (replace && !card.rename(kPath, kBackupPath)) return false;
  if (!card.rename(kTempPath, kPath)) {
    if (replace) card.rename(kBackupPath, kPath);
    return false;
  }
  if (replace) card.remove(kBackupPath);
  return true;
}

// At boot: an install cut off between its two renames leaves only the backup.
template <typename Fs>
bool restoreBackup(Fs& card) {
  if (!card.exists(kBackupPath) || card.exists(kPath)) return true;
  return card.rename(kBackupPath, kPath);
}

} // namespace EmojiPackDownload
