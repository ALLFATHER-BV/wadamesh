// SPDX-License-Identifier: GPL-3.0-or-later
// Host-directory SPIFFS / LittleFS (see web/compat/SPIFFS.h).
#include "SPIFFS.h"
#include "LittleFS.h"
#include "arduino/vfs_api.h"
#include "wada_host.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <atomic>

#ifndef WADA_DEFAULT_DATA_DIR
#define WADA_DEFAULT_DATA_DIR "wadamesh-data"
#endif

namespace { std::atomic<unsigned> g_fs_gen{0}; }
void wadaHostFsTouched() { g_fs_gen.fetch_add(1, std::memory_order_relaxed); }
unsigned wadaHostFsGeneration() { return g_fs_gen.load(std::memory_order_relaxed); }

const std::string& wadaHostDataDir() {
  static std::string dir = [] {
    const char* e = getenv("WADA_DATA_DIR");
    std::string d = (e && *e) ? e : WADA_DEFAULT_DATA_DIR;
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
// A file opened for writing: the real VFS file, plus "the flash changed" on every write
// and on close (wadaHostFsTouched: the browser persists its IDBFS mount after writes).
class TouchingFileImpl : public FileImpl {
public:
  explicit TouchingFileImpl(FileImplPtr f) : _f(std::move(f)) {}
  ~TouchingFileImpl() override { _f.reset(); wadaHostFsTouched(); }
  size_t write(const uint8_t* buf, size_t size) override { size_t n = _f->write(buf, size); wadaHostFsTouched(); return n; }
  size_t read(uint8_t* buf, size_t size) override { return _f->read(buf, size); }
  void flush() override { _f->flush(); wadaHostFsTouched(); }
  bool seek(uint32_t pos, SeekMode mode) override { return _f->seek(pos, mode); }
  size_t position() const override { return _f->position(); }
  size_t size() const override { return _f->size(); }
  bool setBufferSize(size_t size) override { return _f->setBufferSize(size); }
  void close() override { _f->close(); wadaHostFsTouched(); }
  time_t getLastWrite() override { return _f->getLastWrite(); }
  const char* path() const override { return _f->path(); }
  const char* name() const override { return _f->name(); }
  boolean isDirectory(void) override { return _f->isDirectory(); }
  FileImplPtr openNextFile(const char* mode) override { return _f->openNextFile(mode); }
  boolean seekDir(long position) override { return _f->seekDir(position); }
  String getNextFileName(void) override { return _f->getNextFileName(); }
  String getNextFileName(bool* isDir) override { return _f->getNextFileName(isDir); }
  void rewindDirectory(void) override { _f->rewindDirectory(); }
  operator bool() override { return (bool)*_f; }
private:
  FileImplPtr _f;
};

// A directory read the SPIFFS way. SPIFFS has no folders: listing it hands back every
// file, with "/lock/a.jpg" as one name, and the firmware's file manager builds its
// virtual folders from those names. The host keeps such files in real folders (see
// SPIFFS.h), and a plain readdir() handed the folder itself back as an entry, which
// the file manager showed as a 0 B file. This walks the tree and returns the files.
class SpiffsDirImpl : public FileImpl {
public:
  SpiffsDirImpl(VFSImpl* fs, FileImplPtr dir) : _fs(fs), _dir(std::move(dir)) { rewindDirectory(); }
  ~SpiffsDirImpl() override { closeAll(); }
  size_t write(const uint8_t*, size_t) override { return 0; }
  size_t read(uint8_t*, size_t) override { return 0; }
  void flush() override {}
  bool seek(uint32_t, SeekMode) override { return false; }
  size_t position() const override { return 0; }
  size_t size() const override { return 0; }
  bool setBufferSize(size_t) override { return false; }
  void close() override { closeAll(); _dir->close(); }
  time_t getLastWrite() override { return _dir->getLastWrite(); }
  const char* path() const override { return _dir->path(); }
  const char* name() const override { return _dir->name(); }
  boolean isDirectory(void) override { return true; }
  boolean seekDir(long) override { return false; }
  FileImplPtr openNextFile(const char* mode) override {
    std::string rel;
    if (!next(rel)) return FileImplPtr();
    return _fs->open(rel.c_str(), mode ? mode : "r", false);
  }
  String getNextFileName(void) override { std::string rel; return next(rel) ? String(rel.c_str()) : String(""); }
  String getNextFileName(bool* isDir) override { if (isDir) *isDir = false; return getNextFileName(); }
  void rewindDirectory(void) override {
    closeAll();
    const char* p = _dir->path();
    push(p && *p ? p : "/");
  }
  operator bool() override { return (bool)*_dir; }
private:
  struct Level { std::string rel; DIR* d; };
  void push(const std::string& rel) {
    const char* mp = _fs->mountpoint();
    if (!mp) return;
    DIR* d = opendir((std::string(mp) + rel).c_str());
    if (d) _stack.push_back({ rel, d });
  }
  void closeAll() { for (auto& l : _stack) closedir(l.d); _stack.clear(); }
  bool next(std::string& out) {
    while (!_stack.empty()) {
      Level& top = _stack.back();
      dirent* e = readdir(top.d);
      if (!e) { closedir(top.d); _stack.pop_back(); continue; }
      if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
      std::string rel = top.rel + (top.rel.empty() || top.rel.back() != '/' ? "/" : "") + e->d_name;
      struct stat st;
      if (stat((std::string(_fs->mountpoint()) + rel).c_str(), &st) != 0) continue;
      if (S_ISDIR(st.st_mode)) { push(rel); continue; }
      if (!S_ISREG(st.st_mode)) continue;
      out = rel;
      return true;
    }
    return false;
  }
  VFSImpl* _fs;
  FileImplPtr _dir;
  std::vector<Level> _stack;
};

// SPIFFS/LittleFS semantics on top of real folders: a write never fails for want of a folder.
class FlatVFSImpl : public VFSImpl {
public:
  explicit FlatVFSImpl(bool spiffs_listing = false) : _spiffs_listing(spiffs_listing) {}
  FileImplPtr open(const char* path, const char* mode, const bool create) override {
    const bool writes = mode && mode[0] != 'r';
    FileImplPtr f = VFSImpl::open(path, mode, create || writes);
    if (!writes && f && _spiffs_listing && f->isDirectory())
      return std::make_shared<SpiffsDirImpl>(this, std::move(f));
    if (!writes || !f) return f;
    wadaHostFsTouched();
    return std::make_shared<TouchingFileImpl>(std::move(f));
  }
  bool rename(const char* from, const char* to) override { bool ok = VFSImpl::rename(from, to); wadaHostFsTouched(); return ok; }
  // A ".." segment never names anything on a device partition; here it would reach
  // the host's files outside the data dir, so a destructive call refuses it.
  static bool escapes(const char* p) {
    for (const char* s = p; s && (s = strstr(s, "..")) != nullptr; s += 2)
      if ((s == p || s[-1] == '/') && (s[2] == '/' || s[2] == '\0')) return true;
    return false;
  }
  bool remove(const char* path) override {
    if (escapes(path)) return false;
    bool ok = VFSImpl::remove(path); wadaHostFsTouched(); return ok;
  }
  bool mkdir(const char* path) override { bool ok = VFSImpl::mkdir(path); wadaHostFsTouched(); return ok; }
  bool rmdir(const char* path) override {
    if (escapes(path)) return false;
    bool ok = VFSImpl::rmdir(path); wadaHostFsTouched(); return ok;
  }
private:
  bool _spiffs_listing;
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
// Constructed on first use: an FS object in another translation unit (UITask.cpp's own
// LittleFSFS for the tiles partition) is built during static initialisation, possibly
// before this file's globals, and would copy an empty pointer (every open then fails).
std::shared_ptr<FlatVFSImpl>& spiffsImpl() { static auto p = std::make_shared<FlatVFSImpl>(true); return p; }
std::shared_ptr<FlatVFSImpl>& lfsImpl() { static auto p = std::make_shared<FlatVFSImpl>(); return p; }
HostVolume g_spiffs_vol, g_lfs_vol;
}  // namespace

// The SPIFFS partition of the touch boards' partitions_tft_touch.csv (0x170000 bytes):
// what the About page and the storage-full guards are written against.
static const size_t k_spiffs_total = 0x170000;
static const size_t k_lfs_total = 0x4C0000;

fs::SPIFFSFS::SPIFFSFS() : FS(spiffsImpl()) {}
bool fs::SPIFFSFS::begin(bool, const char*, uint8_t, const char*) { return mountVolume(*this, *spiffsImpl(), g_spiffs_vol, "spiffs"); }
bool fs::SPIFFSFS::format() { if (!g_spiffs_vol.mounted) begin(); rmTree(g_spiffs_vol.root, true); wadaHostFsTouched(); return true; }
size_t fs::SPIFFSFS::totalBytes() { return k_spiffs_total; }
size_t fs::SPIFFSFS::usedBytes() { return g_spiffs_vol.mounted ? duBytes(g_spiffs_vol.root) : 0; }
void fs::SPIFFSFS::end() { spiffsImpl()->mountpoint(NULL); g_spiffs_vol.mounted = false; }
fs::SPIFFSFS SPIFFS;

fs::LittleFSFS::LittleFSFS() : FS(lfsImpl()) {}
bool fs::LittleFSFS::begin(bool, const char*, uint8_t, const char*) { return mountVolume(*this, *lfsImpl(), g_lfs_vol, "littlefs"); }
bool fs::LittleFSFS::format() { if (!g_lfs_vol.mounted) begin(); rmTree(g_lfs_vol.root, true); wadaHostFsTouched(); return true; }
size_t fs::LittleFSFS::totalBytes() { return k_lfs_total; }
size_t fs::LittleFSFS::usedBytes() { return g_lfs_vol.mounted ? duBytes(g_lfs_vol.root) : 0; }
void fs::LittleFSFS::end() { lfsImpl()->mountpoint(NULL); g_lfs_vol.mounted = false; }
fs::LittleFSFS LittleFS;

// ---- SD: no card (compat/SD.h) ---------------------------------------------------
#include "SD.h"
namespace { std::shared_ptr<VFSImpl> g_sd_impl = std::make_shared<VFSImpl>(); }   // never mounted
fs::SDFS::SDFS() : FS(g_sd_impl) {}
fs::SDFS SD;
