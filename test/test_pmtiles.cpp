// Host test for the PMTiles v3 reader (src/ui-touch/PmTiles.cpp): header,
// directory and z/x/y primitives, then the full lookup path (gzip and plain
// directories, leaf directories, run lengths, unclustered data) against
// archives written by test/pmtiles/make_fixtures.py, plus archives that must
// be refused (vector, JPEG, gzip tiles, brotli directories, v2, cut short) and
// corrupt ones that must fail cleanly (bad CRC, lying gzip sizes, hostile entry
// counts, out-of-section entries, a leaf that points at itself).
//
// Needs upstream miniz for tinfl (web/fetch-deps.sh puts it in web/third_party):
//   c++ -std=c++17 -Wall -Wextra -I src/ui-touch -I web/third_party/miniz \
//       src/ui-touch/PmTiles.cpp web/third_party/miniz/miniz_tinfl.c \
//       test/test_pmtiles.cpp -o /tmp/pmtiles && /tmp/pmtiles [fixture_dir]
// Without a fixture_dir it runs make_fixtures.py into $TMPDIR itself (from the
// repo root, as scripts/run-host-tests.sh does).

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "PmTiles.h"

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

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

static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- primitives

static void testPrimitives() {
  CHECK(PmTiles::zxyToTileId(0, 0, 0) == 0);
  CHECK(PmTiles::zxyToTileId(1, 0, 0) == 1);
  CHECK(PmTiles::zxyToTileId(1, 0, 1) == 2);
  CHECK(PmTiles::zxyToTileId(1, 1, 1) == 3);
  CHECK(PmTiles::zxyToTileId(1, 1, 0) == 4);
  CHECK(PmTiles::zxyToTileId(2, 0, 0) == 5);
  CHECK(PmTiles::zxyToTileId(2, 2, 0) == 19);
  CHECK(PmTiles::zxyToTileId(2, 0, 1) == 8);
  CHECK(PmTiles::zxyToTileId(12, 3423, 1763) == 19078479);   // the spec's own example
  CHECK(PmTiles::zxyToTileId(2, 4, 0) == UINT64_MAX);
  CHECK(PmTiles::zxyToTileId(32, 0, 0) == UINT64_MAX);

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
  CHECK(PmTiles::parseHeader(header, sizeof(header), parsed));
  CHECK(parsed.root_dir_offset == 127 && parsed.root_dir_bytes == 20);
  CHECK(parsed.tile_type == PmTiles::kTileTypePng && parsed.max_zoom == 12);
  header[7] = 2;
  CHECK(!PmTiles::parseHeader(header, sizeof(header), parsed));
  header[7] = 3;
  CHECK(!PmTiles::parseHeader(header, sizeof(header) - 1, parsed));

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
  CHECK(PmTiles::parseDirectory(directory.data(), directory.size(), entries));
  PmTiles::Entry found{};
  CHECK(PmTiles::findTile(entries, 1, found) && found.offset == 0 && found.length == 2);
  CHECK(PmTiles::findTile(entries, 2, found) && found.offset == 2 && found.length == 3);
  CHECK(PmTiles::findTile(entries, 3, found) && found.offset == 2 && found.length == 3);
  CHECK(!PmTiles::findTile(entries, 4, found));
  directory.pop_back();
  CHECK(!PmTiles::parseDirectory(directory.data(), directory.size(), entries));

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
  CHECK(PmTiles::parseDirectory(leaf_directory.data(), leaf_directory.size(), entries));
  CHECK(PmTiles::findTile(entries, 3, found) && found.run_length == 0 && found.offset == 0);
  CHECK(PmTiles::findTile(entries, 4, found) && found.run_length == 1 && found.offset == 1);

  // An entry count beyond the cap is refused before anything is allocated.
  std::vector<uint8_t> big;
  appendVarint(big, PmTiles::kMaxDirectoryEntries + 1);
  big.resize(4 * (PmTiles::kMaxDirectoryEntries + 2), 0);
  CHECK(!PmTiles::parseDirectory(big.data(), big.size(), entries));
}

// ---------------------------------------------------------------- archives

struct Archive {
  std::vector<uint8_t> bytes;
  int reads = 0;          // directory reads, to see the cache work
  bool fail_reads = false;
};

static bool readArchive(void* ctx, uint64_t offset, uint8_t* dst, size_t length) {
  Archive* a = static_cast<Archive*>(ctx);
  ++a->reads;
  if (a->fail_reads || offset > a->bytes.size() || length > a->bytes.size() - offset) return false;
  std::memcpy(dst, a->bytes.data() + offset, length);
  return true;
}

static bool loadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

struct Expect { unsigned z, x, y, len; uint32_t crc; };

static std::vector<Expect> loadExpect(const std::string& path) {
  std::vector<Expect> v;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return v;
  Expect e{};
  while (std::fscanf(f, "%u %u %u %u %u", &e.z, &e.x, &e.y, &e.len, &e.crc) == 5) v.push_back(e);
  std::fclose(f);
  return v;
}

static PmTiles::Check check(Archive& a, PmTiles::Header& h) {
  uint8_t raw[PmTiles::kHeaderBytes] = {};
  if (a.bytes.size() < sizeof raw) return PmTiles::kCheckNotV3;   // the device's short read
  std::memcpy(raw, a.bytes.data(), sizeof raw);
  return PmTiles::checkArchive(raw, sizeof raw, a.bytes.size(), h);
}

// Every addressed tile comes back byte-exact; every other tile of the
// archive's levels is absent, and nothing outside them is found.
static void testGoodArchive(const std::string& dir, const char* name, bool full_levels) {
  Archive a;
  CHECK(loadFile(dir + "/" + name + ".pmtiles", a.bytes));
  const std::vector<Expect> expect = loadExpect(dir + "/" + name + ".expect");
  CHECK(!expect.empty());
  PmTiles::Header h{};
  CHECK(check(a, h) == PmTiles::kCheckOk);
  PmTiles::DirCache cache;
  int found = 0;
  std::vector<std::vector<uint8_t>> present(32);
  for (const Expect& e : expect) {
    uint64_t off = 0;
    uint32_t len = 0;
    const PmTiles::Lookup r = PmTiles::locateTile(h, readArchive, &a, cache, (uint8_t)e.z, e.x, e.y, off, len);
    CHECK(r == PmTiles::kFound);
    if (r != PmTiles::kFound) {
      std::fprintf(stderr, "  %s: z%u/%u/%u lookup %d\n", name, e.z, e.x, e.y, (int)r);
      continue;
    }
    CHECK(len == e.len && off + len <= a.bytes.size());
    if (len == e.len && off + len <= a.bytes.size()) {
      CHECK(crc32(a.bytes.data() + off, len) == e.crc);
      CHECK(std::memcmp(a.bytes.data() + off, "\x89PNG", 4) == 0);
    }
    ++found;
  }
  CHECK(found == (int)expect.size());

  // The cache: the same tile again reads no directory.
  {
    const Expect& e = expect.back();
    uint64_t off; uint32_t len;
    PmTiles::locateTile(h, readArchive, &a, cache, (uint8_t)e.z, e.x, e.y, off, len);
    const int before = a.reads;
    CHECK(PmTiles::locateTile(h, readArchive, &a, cache, (uint8_t)e.z, e.x, e.y, off, len) == PmTiles::kFound);
    CHECK(a.reads == before);
  }

  // Absent tiles: everything at the archive's levels that isn't addressed.
  if (full_levels) {
    for (unsigned z = h.min_zoom; z <= h.max_zoom && z <= 6; ++z) {
      const uint32_t n = 1u << z;
      for (uint32_t x = 0; x < n; ++x)
        for (uint32_t y = 0; y < n; ++y) {
          bool addressed = false;
          for (const Expect& e : expect)
            if (e.z == z && e.x == x && e.y == y) { addressed = true; break; }
          if (addressed) continue;
          uint64_t off; uint32_t len;
          CHECK(PmTiles::locateTile(h, readArchive, &a, cache, (uint8_t)z, x, y, off, len) == PmTiles::kAbsent);
        }
    }
  } else {
    // Sparse region archive: the far corners of each level are not in it.
    for (unsigned z = h.min_zoom; z <= h.max_zoom; ++z) {
      const uint32_t n = 1u << z;
      uint64_t off; uint32_t len;
      CHECK(PmTiles::locateTile(h, readArchive, &a, cache, (uint8_t)z, 0, 0, off, len) == PmTiles::kAbsent);
      CHECK(PmTiles::locateTile(h, readArchive, &a, cache, (uint8_t)z, n - 1, n - 1, off, len) == PmTiles::kAbsent);
    }
  }
  // Outside the zoom range, or off the grid: absent without a read.
  {
    uint64_t off; uint32_t len;
    const int before = a.reads;
    if (h.min_zoom > 0)
      CHECK(PmTiles::locateTile(h, readArchive, &a, cache, h.min_zoom - 1, 0, 0, off, len) == PmTiles::kAbsent);
    CHECK(PmTiles::locateTile(h, readArchive, &a, cache, h.max_zoom + 1, 0, 0, off, len) == PmTiles::kAbsent);
    CHECK(PmTiles::locateTile(h, readArchive, &a, cache, h.max_zoom, 1u << h.max_zoom, 0, off, len) == PmTiles::kAbsent);
    CHECK(a.reads == before);
  }
  // A card that stops answering: a read error, not a crash or a false tile.
  {
    PmTiles::DirCache cold;
    a.fail_reads = true;
    uint64_t off; uint32_t len;
    const Expect& e = expect.front();
    CHECK(PmTiles::locateTile(h, readArchive, &a, cold, (uint8_t)e.z, e.x, e.y, off, len) == PmTiles::kReadError);
    a.fail_reads = false;
  }
  std::printf("  ok %-20s %zu tiles, z%u-%u, internal=%u, %d directory reads\n", name, expect.size(),
              h.min_zoom, h.max_zoom, h.internal_compression, a.reads);
}

static void testRefused(const std::string& dir, const char* name, PmTiles::Check want) {
  Archive a;
  CHECK(loadFile(dir + "/" + name + ".pmtiles", a.bytes));
  PmTiles::Header h{};
  const PmTiles::Check got = check(a, h);
  CHECK(got == want);
  if (got != want) std::fprintf(stderr, "  %s: check %d, want %d\n", name, (int)got, (int)want);
  else std::printf("  ok %-20s refused (%d)\n", name, (int)got);
}

// Passes the header check, but every lookup has to fail cleanly.
static void testCorrupt(const std::string& dir, const char* name) {
  Archive a;
  CHECK(loadFile(dir + "/" + name + ".pmtiles", a.bytes));
  PmTiles::Header h{};
  CHECK(check(a, h) == PmTiles::kCheckOk);
  PmTiles::DirCache cache;
  int corrupt = 0, lookups = 0;
  for (unsigned z = h.min_zoom; z <= h.max_zoom && z <= 3; ++z)
    for (uint32_t x = 0; x < (1u << z); ++x)
      for (uint32_t y = 0; y < (1u << z); ++y) {
        uint64_t off = 0; uint32_t len = 0;
        const PmTiles::Lookup r = PmTiles::locateTile(h, readArchive, &a, cache, (uint8_t)z, x, y, off, len);
        CHECK(r != PmTiles::kFound);
        CHECK(r != PmTiles::kReadError);
        if (r == PmTiles::kCorrupt) ++corrupt;
        ++lookups;
      }
  CHECK(corrupt > 0);
  std::printf("  ok %-20s %d/%d lookups corrupt, none found\n", name, corrupt, lookups);
}

int main(int argc, char** argv) {
  testPrimitives();

  std::string dir;
  if (argc > 1) {
    dir = argv[1];
  } else {
    const char* tmp = std::getenv("TMPDIR");
    dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/wada-pmtiles-fixtures";
    const std::string cmd = "python3 -I test/pmtiles/make_fixtures.py '" + dir + "' > /dev/null";
    if (std::system(cmd.c_str()) != 0) {
      std::fprintf(stderr, "cannot generate fixtures (%s); run from the repo root\n", cmd.c_str());
      return 1;
    }
  }

  testGoodArchive(dir, "raster_root", true);
  testGoodArchive(dir, "raster_leaf", true);
  testGoodArchive(dir, "raster_gzip_fields", true);
  testGoodArchive(dir, "raster_plain_dirs", true);
  testGoodArchive(dir, "raster_z10_12", false);

  testRefused(dir, "vector_mvt", PmTiles::kCheckNotPng);
  testRefused(dir, "raster_jpeg", PmTiles::kCheckNotPng);
  testRefused(dir, "raster_gzip_tiles", PmTiles::kCheckNotPng);
  testRefused(dir, "brotli_dirs", PmTiles::kCheckNotPng);
  testRefused(dir, "not_v3", PmTiles::kCheckNotV3);
  testRefused(dir, "short_header", PmTiles::kCheckNotV3);
  testRefused(dir, "zoom_inverted", PmTiles::kCheckNotV3);
  testRefused(dir, "truncated", PmTiles::kCheckTruncated);

  for (const char* name : {"bad_crc", "isize_short", "isize_huge", "deflate_damaged", "huge_count",
                           "short_dir", "id_overflow", "tile_oob", "leaf_oob", "leaf_loop",
                           "dir_trailing"})
    testCorrupt(dir, name);

  if (g_failures) {
    std::fprintf(stderr, "test_pmtiles: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("test_pmtiles: all checks passed");
  return 0;
}
