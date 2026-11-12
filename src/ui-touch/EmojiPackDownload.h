#pragma once

namespace EmojiPackDownload {

static constexpr char kPath[] = "/emoji/emoji.pack";
static constexpr char kRootPath[] = "/emoji.pack";
static constexpr char kTempPath[] = "/emoji/emoji.pack.tmp";
static constexpr char kBackupPath[] = "/emoji/emoji.pack.bak";

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

template <typename Fs>
bool install(Fs& card, Fs* data, unsigned approved) {
  if (locations(&card, data) != approved || card.exists(kBackupPath)) return false;
  const bool replace = (approved & 1) != 0;
  if (replace && !card.rename(kPath, kBackupPath)) return false;
  if (!card.rename(kTempPath, kPath)) {
    if (replace) card.rename(kBackupPath, kPath);
    return false;
  }
  if (replace) card.remove(kBackupPath);
  return true;
}

} // namespace EmojiPackDownload
