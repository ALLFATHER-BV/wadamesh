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

// Owning entry array in PSRAM (internal RAM fallback). No std::vector: the IDF
// builds run without C++ exceptions, so an allocator could not report OOM.
class Directory {
 public:
  Directory() = default;
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  ~Directory() { clear(); }

  bool resize(size_t count) {
    clear();
    if (count == 0) return true;
    if (count > SIZE_MAX / sizeof(Entry)) return false;
#if defined(ESP32)
    void* memory = heap_caps_malloc(count * sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) memory = heap_caps_malloc(count * sizeof(Entry), MALLOC_CAP_8BIT);
#else
    void* memory = std::malloc(count * sizeof(Entry));
#endif
    if (!memory) return false;
    entries_ = static_cast<Entry*>(memory);
    size_ = count;
    return true;
  }

  void clear() {
#if defined(ESP32)
    heap_caps_free(entries_);
#else
    std::free(entries_);
#endif
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

}  // namespace PmTiles
