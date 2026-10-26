#include "PmTiles.h"

#include <cstring>
#include <limits>

// The ROM's tinfl on the S3 (no flash cost); the IDF 5.x P4 boards moved the
// header to esp_rom/include; the web build and the host test use upstream
// miniz, which has the same API.
#if __has_include("rom/miniz.h")
  #include "rom/miniz.h"
  #define PMTILES_HAVE_TINFL 1
#elif __has_include("miniz.h")
  #include "miniz.h"
  #define PMTILES_HAVE_TINFL 1
#else
  #define PMTILES_HAVE_TINFL 0
#endif

namespace PmTiles {
namespace {

uint64_t readU64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
  return value;
}

uint32_t readU32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

bool readVarint(const uint8_t*& cursor, const uint8_t* end, uint64_t& value) {
  value = 0;
  for (unsigned shift = 0; shift < 70; shift += 7) {
    if (cursor == end) return false;
    const uint8_t byte = *cursor++;
    if (shift == 63 && (byte & 0xFE) != 0) return false;
    value |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

void rotate(uint32_t n, uint32_t& x, uint32_t& y, uint32_t rx, uint32_t ry) {
  if (ry == 0) {
    if (rx != 0) {
      x = n - 1 - x;
      y = n - 1 - y;
    }
    const uint32_t swap = x;
    x = y;
    y = swap;
  }
}

// [offset, offset + length) lies inside [0, limit).
bool inside(uint64_t offset, uint64_t length, uint64_t limit) {
  return offset <= limit && length <= limit - offset;
}

}  // namespace

void* allocBuffer(size_t bytes) {
  if (bytes == 0) return nullptr;
#if defined(ESP32)
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p && bytes <= 16u * 1024u) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return p;
#else
  return std::malloc(bytes);
#endif
}

void freeBuffer(void* p) {
  if (!p) return;
#if defined(ESP32)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}

bool parseHeader(const uint8_t* bytes, size_t length, Header& header) {
  if (!bytes || length < kHeaderBytes || std::memcmp(bytes, "PMTiles", 7) != 0 || bytes[7] != 3)
    return false;

  header.root_dir_offset = readU64(bytes + 8);
  header.root_dir_bytes = readU64(bytes + 16);
  header.leaf_dirs_offset = readU64(bytes + 40);
  header.leaf_dirs_bytes = readU64(bytes + 48);
  header.tile_data_offset = readU64(bytes + 56);
  header.tile_data_bytes = readU64(bytes + 64);
  header.internal_compression = bytes[97];
  header.tile_compression = bytes[98];
  header.tile_type = bytes[99];
  header.min_zoom = bytes[100];
  header.max_zoom = bytes[101];

  return header.root_dir_bytes != 0 && header.min_zoom <= header.max_zoom;
}

bool parseDirectory(const uint8_t* bytes, size_t length, Directory& entries) {
  entries.clear();
  if (!bytes || length == 0) return false;
  const uint8_t* cursor = bytes;
  const uint8_t* const end = bytes + length;
  uint64_t count = 0;
  // Every entry takes at least one byte in each of its four columns.
  if (!readVarint(cursor, end, count) || count > length / 4 || count > kMaxDirectoryEntries)
    return false;

  if (!entries.resize(static_cast<size_t>(count))) return false;
  uint64_t previous_id = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    uint64_t delta;
    if (!readVarint(cursor, end, delta) || delta > UINT64_MAX - previous_id) return false;
    entries[i].tile_id = previous_id += delta;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    uint64_t run_length;
    if (!readVarint(cursor, end, run_length) || run_length > UINT32_MAX) return false;
    entries[i].run_length = static_cast<uint32_t>(run_length);
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    uint64_t tile_length;
    if (!readVarint(cursor, end, tile_length) || tile_length > UINT32_MAX) return false;
    entries[i].length = static_cast<uint32_t>(tile_length);
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    uint64_t encoded_offset;
    if (!readVarint(cursor, end, encoded_offset)) return false;
    if (i > 0 && encoded_offset == 0) {
      if (entries[i - 1].offset > UINT64_MAX - entries[i - 1].length) return false;
      entries[i].offset = entries[i - 1].offset + entries[i - 1].length;
    } else {
      if (encoded_offset == 0) return false;
      entries[i].offset = encoded_offset - 1;
    }
  }
  return cursor == end;
}

bool findTile(const Directory& entries, uint64_t tile_id, Entry& entry) {
  size_t first = 0;
  size_t last = entries.size();
  while (first < last) {
    const size_t mid = first + (last - first) / 2;
    if (entries[mid].tile_id < tile_id) first = mid + 1;
    else last = mid;
  }
  if (first < entries.size() && entries[first].tile_id == tile_id) {
    entry = entries[first];
    return entry.length != 0;
  }
  if (first == 0) return false;
  entry = entries[first - 1];
  return entry.length != 0 &&
         (entry.run_length == 0 || tile_id - entry.tile_id < entry.run_length);
}

uint64_t zxyToTileId(uint8_t zoom, uint32_t x, uint32_t y) {
  if (zoom > 31 || x >= (uint64_t{1} << zoom) || y >= (uint64_t{1} << zoom)) return UINT64_MAX;
  uint64_t tile_id = ((uint64_t{1} << (zoom * 2U)) - 1) / 3;
  if (zoom == 0) return tile_id;

  uint32_t tx = x;
  uint32_t ty = y;
  int bit = zoom - 1;
  for (uint32_t scale = uint32_t{1} << bit; scale > 0; scale >>= 1) {
    const uint32_t rx = scale & tx;
    const uint32_t ry = scale & ty;
    rotate(scale, tx, ty, rx, ry);
    tile_id += (static_cast<uint64_t>((3 * rx) ^ ry)) << bit;
    --bit;
  }
  return tile_id;
}

Check checkArchive(const uint8_t* raw, size_t raw_len, uint64_t file_size, Header& header) {
  if (!parseHeader(raw, raw_len, header)) return kCheckNotV3;
  if ((header.tile_type != kTileTypePng && header.tile_type != kTileTypeUnknown) ||
      (header.tile_compression != kCompressionNone &&
       header.tile_compression != kCompressionUnknown) ||
      (header.internal_compression != kCompressionGzip &&
       header.internal_compression != kCompressionNone))
    return kCheckNotPng;
  if (!inside(header.root_dir_offset, header.root_dir_bytes, file_size) ||
      !inside(header.leaf_dirs_offset, header.leaf_dirs_bytes, file_size) ||
      !inside(header.tile_data_offset, header.tile_data_bytes, file_size))
    return kCheckTruncated;
  return kCheckOk;
}

uint8_t* gunzip(const uint8_t* input, size_t length, size_t& output_length) {
  output_length = 0;
#if PMTILES_HAVE_TINFL
  if (!input || length < 18 || input[0] != 0x1F || input[1] != 0x8B ||
      input[2] != 8 || (input[3] & 0xE0) != 0)
    return nullptr;

  const size_t trailer = length - 8;
  size_t data_offset = 10;
  const uint8_t flags = input[3];
  if (flags & 0x04) {   // FEXTRA
    if (data_offset + 2 > trailer) return nullptr;
    const size_t extra_length = static_cast<size_t>(input[data_offset]) |
                                (static_cast<size_t>(input[data_offset + 1]) << 8);
    data_offset += 2;
    if (extra_length > trailer - data_offset) return nullptr;
    data_offset += extra_length;
  }
  static const uint8_t kStringFields[2] = {0x08, 0x10};   // FNAME, FCOMMENT
  for (const uint8_t flag : kStringFields) {
    if ((flags & flag) == 0) continue;
    while (data_offset < trailer && input[data_offset++] != 0) {}
    if (data_offset > trailer || input[data_offset - 1] != 0) return nullptr;
  }
  if (flags & 0x02) {   // FHCRC
    if (data_offset + 2 > trailer) return nullptr;
    data_offset += 2;
  }
  if (data_offset >= trailer) return nullptr;

  const size_t expected_length = readU32(input + length - 4);
  if (expected_length == 0 || expected_length > kMaxDirectoryBytes) return nullptr;

  uint8_t* output = static_cast<uint8_t*>(allocBuffer(expected_length));
  if (!output) return nullptr;
  // ~11 KB of state: never on a task stack.
  tinfl_decompressor* state = static_cast<tinfl_decompressor*>(allocBuffer(sizeof(tinfl_decompressor)));
  if (!state) {
    freeBuffer(output);
    return nullptr;
  }
  tinfl_init(state);
  size_t compressed_length = trailer - data_offset;
  size_t decompressed_length = expected_length;
  const tinfl_status status = tinfl_decompress(
      state, input + data_offset, &compressed_length, output, output, &decompressed_length,
      TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  freeBuffer(state);
  bool ok = status == TINFL_STATUS_DONE && decompressed_length == expected_length;
  if (ok) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < decompressed_length; ++i) {
      crc ^= output[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    ok = (crc ^ UINT32_MAX) == readU32(input + length - 8);
  }
  if (!ok) {
    freeBuffer(output);
    return nullptr;
  }
  output_length = decompressed_length;
  return output;
#else
  (void)input;
  (void)length;
  return nullptr;
#endif
}

namespace {

Lookup readDirectory(const Header& header, ReadFn read, void* ctx, uint64_t offset,
                     uint64_t length, Directory& out) {
  if (length == 0 || length > kMaxDirectoryBytes) return kCorrupt;
  uint8_t* raw = static_cast<uint8_t*>(allocBuffer(static_cast<size_t>(length)));
  if (!raw) return kReadError;
  if (!read(ctx, offset, raw, static_cast<size_t>(length))) {
    freeBuffer(raw);
    return kReadError;
  }
  const uint8_t* bytes = raw;
  size_t bytes_length = static_cast<size_t>(length);
  uint8_t* inflated = nullptr;
  if (header.internal_compression == kCompressionGzip) {
    inflated = gunzip(raw, bytes_length, bytes_length);
    freeBuffer(raw);
    raw = nullptr;
    if (!inflated) return kCorrupt;
    bytes = inflated;
  }
  const bool parsed = parseDirectory(bytes, bytes_length, out);
  freeBuffer(raw);
  freeBuffer(inflated);
  if (!parsed) out.clear();
  return parsed ? kFound : kCorrupt;
}

}  // namespace

Lookup locateTile(const Header& header, ReadFn read, void* ctx, DirCache& cache,
                  uint8_t z, uint32_t x, uint32_t y,
                  uint64_t& tile_offset, uint32_t& tile_length) {
  if (z < header.min_zoom || z > header.max_zoom) return kAbsent;
  const uint64_t tile_id = zxyToTileId(z, x, y);
  if (tile_id == UINT64_MAX) return kAbsent;

  // checkArchive put every section inside the file, so an entry inside its
  // section can't overflow the absolute offset.
  uint64_t dir_offset = header.root_dir_offset;
  uint64_t dir_length = header.root_dir_bytes;
  for (int depth = 0; depth < kMaxDepth; ++depth) {
    Directory& dir = cache.dirs_[depth];
    if (cache.offset_[depth] != dir_offset || dir.empty()) {
      cache.offset_[depth] = UINT64_MAX;
      const Lookup r = readDirectory(header, read, ctx, dir_offset, dir_length, dir);
      if (r != kFound) return r;
      cache.offset_[depth] = dir_offset;
    }
    Entry entry{};
    if (!findTile(dir, tile_id, entry)) return kAbsent;
    if (entry.run_length != 0) {
      if (!inside(entry.offset, entry.length, header.tile_data_bytes)) return kCorrupt;
      tile_offset = header.tile_data_offset + entry.offset;
      tile_length = entry.length;
      return kFound;
    }
    if (!inside(entry.offset, entry.length, header.leaf_dirs_bytes)) return kCorrupt;
    dir_offset = header.leaf_dirs_offset + entry.offset;
    dir_length = entry.length;
  }
  return kCorrupt;   // deeper than any writer nests leaves: a loop
}

}  // namespace PmTiles
