#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if defined(ESP32)
  #include <esp_heap_caps.h>
#endif

namespace PmTiles {

constexpr size_t kHeaderBytes = 127;
constexpr uint8_t kCompressionUnknown = 0;
constexpr uint8_t kCompressionNone = 1;
constexpr uint8_t kCompressionGzip = 2;
constexpr uint8_t kTileTypeUnknown = 0;
constexpr uint8_t kTileTypePng = 2;

// Bounds on what a file may make us allocate. Real archives stay far below
// them (go-pmtiles and planetiler keep header + root under 16 KB and start
// leaves at 4096 entries); a corrupt or hostile file fails cleanly instead.
constexpr size_t kMaxDirectoryBytes = 1024u * 1024u;   // compressed or inflated
constexpr size_t kMaxDirectoryEntries = 32768;         // 768 KB decoded
constexpr int kMaxDepth = 4;                           // root + 3 leaf levels

struct Header {
  uint64_t root_dir_offset;
  uint64_t root_dir_bytes;
  uint64_t leaf_dirs_offset;
  uint64_t leaf_dirs_bytes;
  uint64_t tile_data_offset;
  uint64_t tile_data_bytes;
  uint8_t internal_compression;
  uint8_t tile_compression;
  uint8_t tile_type;
  uint8_t min_zoom;
  uint8_t max_zoom;
};

struct Entry {
  uint64_t tile_id;
  uint64_t offset;
  uint32_t length;
  uint32_t run_length;
};

// PSRAM first. Internal RAM only for small blocks: a directory must never be
// what starves the internal heap that Wi-Fi and the radio live on.
void* allocBuffer(size_t bytes);
void freeBuffer(void* p);

// Owning entry array (allocBuffer). No std::vector: the IDF builds run without
// C++ exceptions, so an allocator could not report OOM.
class Directory {
 public:
  Directory() = default;
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  ~Directory() { clear(); }

  bool resize(size_t count) {
    clear();
    if (count == 0) return true;
    if (count > kMaxDirectoryEntries) return false;
    void* memory = allocBuffer(count * sizeof(Entry));
    if (!memory) return false;
    entries_ = static_cast<Entry*>(memory);
    size_ = count;
    return true;
  }

  void clear() {
    freeBuffer(entries_);
    entries_ = nullptr;
    size_ = 0;
  }

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  Entry& operator[](size_t i) { return entries_[i]; }
  const Entry& operator[](size_t i) const { return entries_[i]; }

 private:
  Entry* entries_ = nullptr;
  size_t size_ = 0;
};

bool parseHeader(const uint8_t* bytes, size_t length, Header& header);
bool parseDirectory(const uint8_t* bytes, size_t length, Directory& entries);
bool findTile(const Directory& entries, uint64_t tile_id, Entry& entry);
uint64_t zxyToTileId(uint8_t zoom, uint32_t x, uint32_t y);

// Whether an archive can be drawn: the first kHeaderBytes of the file and its
// size. Usable = PMTiles v3, PNG (or untyped) uncompressed tiles, gzip or plain
// directories, every section inside the file.
enum Check : uint8_t { kCheckOk, kCheckNotV3, kCheckNotPng, kCheckTruncated };
Check checkArchive(const uint8_t* raw, size_t raw_len, uint64_t file_size, Header& header);

// gzip member -> inflated bytes (allocBuffer, release with freeBuffer); the
// CRC32 and size trailer are checked. nullptr when anything is off.
uint8_t* gunzip(const uint8_t* input, size_t length, size_t& output_length);

// Reads exactly `length` bytes at `offset` of the archive.
typedef bool (*ReadFn)(void* ctx, uint64_t offset, uint8_t* dst, size_t length);

// The decoded directory last used at each depth, so a pan inside one leaf
// reads no directory at all. One archive at a time: clear() on a switch.
class DirCache {
 public:
  DirCache() { clear(); }
  void clear() {
    for (int i = 0; i < kMaxDepth; ++i) {
      offset_[i] = UINT64_MAX;
      dirs_[i].clear();
    }
  }
  uint64_t offset_[kMaxDepth];
  Directory dirs_[kMaxDepth];
};

enum Lookup : uint8_t {
  kFound,       // tile_offset/tile_length are absolute in the file
  kAbsent,      // the archive has no such tile (or outside its zoom range)
  kReadError,   // the card failed to deliver bytes; maybe transient
  kCorrupt,     // a directory does not decode or points outside its section
};
Lookup locateTile(const Header& header, ReadFn read, void* ctx, DirCache& cache,
                  uint8_t z, uint32_t x, uint32_t y,
                  uint64_t& tile_offset, uint32_t& tile_length);

}  // namespace PmTiles
