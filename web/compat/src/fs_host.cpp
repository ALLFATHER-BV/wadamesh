// SPDX-License-Identifier: GPL-3.0-or-later
// Host-directory SPIFFS / LittleFS (see web/compat/SPIFFS.h).
#include "SPIFFS.h"
#include "LittleFS.h"
#include "arduino/vfs_api.h"
#include "wada_host.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <cstdlib>
#include <cstring>

const std::string& wadaHostDataDir() {
  static std::string dir = [] {
    const char* e = getenv("WADA_DATA_DIR");
    std::string d = (e && *e) ? e : "wadamesh-data";
    wadaHostMkdirs(d);
    return d;
  }();
  return dir;
}
bool wadaHostMkdirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty()) mkdir(cur.c_str(), 0755);
    }
    if (i < path.size()) cur += path[i];
  }
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

namespace {
// SPIFFS/LittleFS semantics on top of real folders: a write never fails for want of a folder.
class FlatVFSImpl : public VFSImpl {
public:
  FileImplPtr open(const char* path, const char* mode, const bool create) override {
    const bool writes = mode && mode[0] != 'r';
    return VFSImpl::open(path, mode, create || writes);
  }
};
size_t duBytes(const std::string& dir) {
  size_t total = 0;
  DIR* d = opendir(dir.c_str());
  if (!d) return 0;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    std::string p = dir + "/" + e->d_name;
    struct stat st;
    if (stat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) total += duBytes(p); else total += (size_t)st.st_size;
  }
  closedir(d);
  return total;
}
void rmTree(const std::string& dir, bool keep_root) {
  DIR* d = opendir(dir.c_str());
  if (d) {
    while (dirent* e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      std::string p = dir + "/" + e->d_name;
      struct stat st;
      if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) rmTree(p, false); else unlink(p.c_str());
    }
    closedir(d);
  }
  if (!keep_root) rmdir(dir.c_str());
}
struct HostVolume {
  std::string root;
  bool mounted = false;
  std::string mountpoint;
};
bool mountVolume(FS& fs, VFSImpl& impl, HostVolume& v, const char* sub) {
  if (v.mounted) return true;
  v.root = wadaHostDataDir() + "/" + sub;
  if (!wadaHostMkdirs(v.root)) return false;
  impl.mountpoint(v.root.c_str());
  v.mounted = true;
  (void)fs;
  return true;
}
std::shared_ptr<FlatVFSImpl> g_spiffs_impl = std::make_shared<FlatVFSImpl>();
std::shared_ptr<FlatVFSImpl> g_lfs_impl = std::make_shared<FlatVFSImpl>();
HostVolume g_spiffs_vol, g_lfs_vol;
}  // namespace

// The SPIFFS partition of the touch boards' partitions_tft_touch.csv (0x170000 bytes):
// what the About page and the storage-full guards are written against.
static const size_t k_spiffs_total = 0x170000;
static const size_t k_lfs_total = 0x4C0000;

fs::SPIFFSFS::SPIFFSFS() : FS(g_spiffs_impl) {}
bool fs::SPIFFSFS::begin(bool, const char*, uint8_t, const char*) { return mountVolume(*this, *g_spiffs_impl, g_spiffs_vol, "spiffs"); }
bool fs::SPIFFSFS::format() { if (!g_spiffs_vol.mounted) begin(); rmTree(g_spiffs_vol.root, true); return true; }
size_t fs::SPIFFSFS::totalBytes() { return k_spiffs_total; }
size_t fs::SPIFFSFS::usedBytes() { return g_spiffs_vol.mounted ? duBytes(g_spiffs_vol.root) : 0; }
void fs::SPIFFSFS::end() { g_spiffs_impl->mountpoint(NULL); g_spiffs_vol.mounted = false; }
fs::SPIFFSFS SPIFFS;

fs::LittleFSFS::LittleFSFS() : FS(g_lfs_impl) {}
bool fs::LittleFSFS::begin(bool, const char*, uint8_t, const char*) { return mountVolume(*this, *g_lfs_impl, g_lfs_vol, "littlefs"); }
bool fs::LittleFSFS::format() { if (!g_lfs_vol.mounted) begin(); rmTree(g_lfs_vol.root, true); return true; }
size_t fs::LittleFSFS::totalBytes() { return k_lfs_total; }
size_t fs::LittleFSFS::usedBytes() { return g_lfs_vol.mounted ? duBytes(g_lfs_vol.root) : 0; }
void fs::LittleFSFS::end() { g_lfs_impl->mountpoint(NULL); g_lfs_vol.mounted = false; }
fs::LittleFSFS LittleFS;

// ---- SD: no card (compat/SD.h) ---------------------------------------------------
#include "SD.h"
namespace { std::shared_ptr<VFSImpl> g_sd_impl = std::make_shared<VFSImpl>(); }   // never mounted
fs::SDFS::SDFS() : FS(g_sd_impl) {}
fs::SDFS SD;
