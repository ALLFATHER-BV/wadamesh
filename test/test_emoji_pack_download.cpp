#include "ui-touch/EmojiPackDownload.h"
#include <cassert>
#include <map>
#include <string>

struct FakeFs {
  std::map<std::string, std::string> files;
  bool add_destination_on_rename = false;
  bool fail_temp_rename = false;

  bool exists(const char* path) { return files.count(path) != 0; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (fail_temp_rename && std::string(from) == EmojiPackDownload::kTempPath) return false;
    if (add_destination_on_rename) files[to] = "custom";
    if (!exists(from) || exists(to)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};

int main() {
  using namespace EmojiPackDownload;
  FakeFs card, data;
  assert(!exists(&card, &data));
  assert(!exists<FakeFs>(nullptr, nullptr));
  for (const char* path : {kPath, kRootPath}) {
    card.files[path] = "custom";
    assert(exists(&card, &data));
    card.files.erase(path);
    data.files[path] = "custom";
    assert(exists(&card, &data));
    data.files.erase(path);
  }
  card.files[kTempPath] = "download";
  card.files[kPath] = "custom";
  assert(!install(card, &data, 0));
  assert(card.files[kPath] == "custom");
  assert(card.files[kTempPath] == "download");
  card.files.erase(kPath);
  data.files[kRootPath] = "custom";
  assert(!install(card, &data, 0));
  assert(data.files[kRootPath] == "custom");
  data.files.clear();
  card.add_destination_on_rename = true;
  assert(!install(card, &data, 0));
  assert(card.files[kPath] == "custom");
  card.files.erase(kPath);
  card.add_destination_on_rename = false;
  assert(install(card, &data, 0));
  assert(card.files[kPath] == "download");
  assert(!card.exists(kTempPath));
  card.files[kTempPath] = "new";
  assert(install(card, &data, 1));
  assert(card.files[kPath] == "new");
  assert(!card.exists(kBackupPath));
  card.files[kTempPath] = "newer";
  card.fail_temp_rename = true;
  assert(!install(card, &data, 1));
  assert(card.files[kPath] == "new");
  assert(!card.exists(kBackupPath));
  card.fail_temp_rename = false;
  card.files[kBackupPath] = "backup";
  assert(!install(card, &data, 1));
  assert(card.files[kPath] == "new");
  assert(card.files[kBackupPath] == "backup");
  card.files.erase(kBackupPath);
  data.files[kRootPath] = "custom";
  assert(!install(card, &data, 1));
  assert(card.files[kPath] == "new");
  assert(install(card, &data, 9));
  assert(data.files[kRootPath] == "custom");
}
