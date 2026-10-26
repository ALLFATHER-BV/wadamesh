#include "PmTiles.h"

#include <cstring>
#include <limits>

namespace PmTiles {
namespace {

uint64_t readU64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
  return value;
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

}  // namespace

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
  if (!readVarint(cursor, end, count) || count > length / 4 ||
      count > std::numeric_limits<size_t>::max())
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

}  // namespace PmTiles
