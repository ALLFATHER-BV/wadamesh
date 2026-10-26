// Host test for the PMTiles v3 header, directory, and z/x/y primitives.
//
//   c++ -std=c++17 -Wall -Wextra -Isrc/ui-touch src/ui-touch/PmTiles.cpp test/test_pmtiles.cpp -o /tmp/pmtiles && /tmp/pmtiles

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "PmTiles.h"

static void appendVarint(std::vector<uint8_t>& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>(value) | 0x80);
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

static void putU64(uint8_t* bytes, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}

static void putI32(uint8_t* bytes, int32_t value) {
  const uint32_t u = static_cast<uint32_t>(value);
  for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(u >> (i * 8));
}

int main() {
  assert(PmTiles::zxyToTileId(0, 0, 0) == 0);
  assert(PmTiles::zxyToTileId(1, 0, 0) == 1);
  assert(PmTiles::zxyToTileId(1, 0, 1) == 2);
  assert(PmTiles::zxyToTileId(1, 1, 1) == 3);
  assert(PmTiles::zxyToTileId(1, 1, 0) == 4);
  assert(PmTiles::zxyToTileId(2, 0, 0) == 5);
  assert(PmTiles::zxyToTileId(2, 2, 0) == 19);
  assert(PmTiles::zxyToTileId(2, 0, 1) == 8);
  assert(PmTiles::zxyToTileId(2, 4, 0) == UINT64_MAX);

  uint8_t header[PmTiles::kHeaderBytes] = {};
  std::memcpy(header, "PMTiles", 7);
  header[7] = 3;
  putU64(header + 8, 127);
  putU64(header + 16, 20);
  header[97] = PmTiles::kCompressionGzip;
  header[98] = PmTiles::kCompressionNone;
  header[99] = PmTiles::kTileTypePng;
  header[100] = 0;
  header[101] = 12;
  putI32(header + 102, -1800000000);
  putI32(header + 106, -900000000);
  putI32(header + 110, 1800000000);
  putI32(header + 114, 900000000);

  PmTiles::Header parsed{};
  assert(PmTiles::parseHeader(header, sizeof(header), parsed));
  assert(parsed.root_dir_offset == 127 && parsed.root_dir_bytes == 20);
  assert(parsed.tile_type == PmTiles::kTileTypePng && parsed.max_zoom == 12);
  header[7] = 2;
  assert(!PmTiles::parseHeader(header, sizeof(header), parsed));
  header[7] = 3;
  assert(!PmTiles::parseHeader(header, sizeof(header) - 1, parsed));

  // Two sorted entries: tile 1 is a single PNG at offset 0; tile 2-3 share a run.
  std::vector<uint8_t> directory;
  appendVarint(directory, 2);
  appendVarint(directory, 1);  // tile IDs: 1, 2
  appendVarint(directory, 1);
  appendVarint(directory, 1);  // run lengths: 1, 2
  appendVarint(directory, 2);
  appendVarint(directory, 2);  // lengths: 2, 3
  appendVarint(directory, 3);
  appendVarint(directory, 1);  // first offset = 0
  appendVarint(directory, 3);  // second offset = 2

  PmTiles::Directory entries;
  assert(PmTiles::parseDirectory(directory.data(), directory.size(), entries));
  PmTiles::Entry found{};
  assert(PmTiles::findTile(entries, 1, found) && found.offset == 0 && found.length == 2);
  assert(PmTiles::findTile(entries, 2, found) && found.offset == 2 && found.length == 3);
  assert(PmTiles::findTile(entries, 3, found) && found.offset == 2 && found.length == 3);
  assert(!PmTiles::findTile(entries, 4, found));
  directory.pop_back();
  assert(!PmTiles::parseDirectory(directory.data(), directory.size(), entries));

  // A zero run-length entry points at a leaf directory and covers the gap
  // before the next entry as well as its exact tile ID.
  std::vector<uint8_t> leaf_directory;
  appendVarint(leaf_directory, 2);
  appendVarint(leaf_directory, 1);  // IDs: 1, 4
  appendVarint(leaf_directory, 3);
  appendVarint(leaf_directory, 0);  // run lengths: leaf pointer, single tile
  appendVarint(leaf_directory, 1);
  appendVarint(leaf_directory, 10); // lengths
  appendVarint(leaf_directory, 2);
  appendVarint(leaf_directory, 1);  // first offset = 1, second = 2
  appendVarint(leaf_directory, 2);
  assert(PmTiles::parseDirectory(leaf_directory.data(), leaf_directory.size(), entries));
  assert(PmTiles::findTile(entries, 3, found) && found.run_length == 0 && found.offset == 0);
  assert(PmTiles::findTile(entries, 4, found) && found.run_length == 1 && found.offset == 1);

  std::puts("test_pmtiles: all assertions passed");
  return 0;
}
