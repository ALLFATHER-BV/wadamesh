// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for PmTilesCard (src/ui-touch/PmTilesCard.cpp): PMTiles archives on
// the card are read through FatFs with 64-bit offsets, so a map archive larger
// than 4 GiB (only exFAT holds one) reads correctly at every offset. The
// Arduino File / VFS path it replaced stops at 2 GiB (32-bit off_t).
//
// Runs the firmware's own FatFs (arch/esp32/fatfs_exfat, exFAT on) over a
// 16 GiB card kept sparse in memory:
//   g++ -std=c++17 -I src -I test/fatfs_fakes -I arch/esp32/fatfs_exfat/src -I src/ui-touch \
//       -DCAP_MAP_PMTILES=1 -x c arch/esp32/fatfs_exfat/src/ff.c \
//       arch/esp32/fatfs_exfat/src/ffunicode.c -x none src/ui-touch/PmTilesCard.cpp \
//       test/test_pmtiles_card.cpp -o /tmp/pmtiles_card && /tmp/pmtiles_card

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "ff.h"
#include "diskio.h"
#include "PmTilesCard.h"

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

// ------------------------------------------------- a sparse in-memory card

static constexpr DWORD kSectors = 16u * 1024u * 1024u * 2u;   // 16 GiB of 512-byte sectors
static std::map<DWORD, std::array<BYTE, 512>> g_card;           // never-written sectors read as zero

DSTATUS ff_disk_initialize(BYTE) { return 0; }
DSTATUS ff_disk_status(BYTE) { return 0; }
DRESULT ff_disk_read(BYTE, BYTE* buff, DWORD sector, UINT count) {
  for (UINT i = 0; i < count; ++i, buff += 512) {
    const auto it = g_card.find(sector + i);
    if (it == g_card.end()) std::memset(buff, 0, 512);
    else std::memcpy(buff, it->second.data(), 512);
  }
  return RES_OK;
}
DRESULT ff_disk_write(BYTE, const BYTE* buff, DWORD sector, UINT count) {
  for (UINT i = 0; i < count; ++i, buff += 512) std::memcpy(g_card[sector + i].data(), buff, 512);
  return RES_OK;
}
DRESULT ff_disk_ioctl(BYTE, BYTE cmd, void* buff) {
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *static_cast<DWORD*>(buff) = kSectors; return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK;
    default: return RES_PARERR;
  }
}

// What ESP-IDF's diskio.c and port/freertos/ffsystem.c provide on the device.
PARTITION VolToPart[FF_VOLUMES] = {{0, 0}, {1, 0}};
DWORD get_fattime(void) { return (DWORD)(2026 - 1980) << 25 | 1u << 21 | 1u << 16; }
void* ff_memalloc(UINT size) { return std::malloc(size); }
void ff_memfree(void* block) { std::free(block); }
int ff_cre_syncobj(BYTE, FF_SYNC_t* sync) { *sync = &g_card; return 1; }
int ff_req_grant(FF_SYNC_t) { return 1; }
void ff_rel_grant(FF_SYNC_t) {}
int ff_del_syncobj(FF_SYNC_t) { return 1; }

// --------------------------------------------------------------- the tests

static constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
static const char kHead[] = "PMTiles header stand-in";
static const char kFar[] = "tile beyond 4 GiB";
static constexpr uint64_t kFarOffset = 5 * kGiB + 12345;   // past the 32-bit and the 2 GiB limits

static bool writeAt(FIL& file, uint64_t offset, const char* text) {
  UINT written = 0;
  return f_lseek(&file, offset) == FR_OK &&
         f_write(&file, text, (UINT)std::strlen(text), &written) == FR_OK &&
         written == std::strlen(text);
}

int main() {
  static FATFS volume;
  static BYTE work[4096];
  CHECK(f_mkfs("0:", FM_EXFAT, 0, work, sizeof work) == FR_OK);
  CHECK(f_mount(&volume, "0:", 1) == FR_OK);
  CHECK(volume.fs_type == FS_EXFAT);

  // A 5 GiB archive: its first bytes and a "tile" past 5 GiB. Seeking past the
  // end in write mode allocates the clusters in between without writing them.
  CHECK(f_mkdir("0:/maps") == FR_OK);
  CHECK(f_mkdir("0:/maps/osm") == FR_OK);
  FIL file;
  CHECK(f_open(&file, "0:/maps/osm/osm.pmtiles", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
  CHECK(writeAt(file, 0, kHead));
  CHECK(writeAt(file, kFarOffset, kFar));
  CHECK(f_close(&file) == FR_OK);
  const uint64_t expected_size = kFarOffset + std::strlen(kFar);

  PmTilesCard::Archive* archive = PmTilesCard::open("0:/maps/osm/osm.pmtiles");
  CHECK(archive != nullptr);
  if (archive) {
    CHECK(PmTilesCard::size(archive) == expected_size);

    char got[64] = {};
    CHECK(PmTilesCard::read(archive, 0, (uint8_t*)got, std::strlen(kHead)));
    CHECK(std::memcmp(got, kHead, std::strlen(kHead)) == 0);

    std::memset(got, 0, sizeof got);
    CHECK(PmTilesCard::read(archive, kFarOffset, (uint8_t*)got, std::strlen(kFar)));
    CHECK(std::memcmp(got, kFar, std::strlen(kFar)) == 0);

    // Back to the start after the far read: the seek is absolute every time.
    std::memset(got, 0, sizeof got);
    CHECK(PmTilesCard::read(archive, 0, (uint8_t*)got, std::strlen(kHead)));
    CHECK(std::memcmp(got, kHead, std::strlen(kHead)) == 0);

    // The last byte is readable; one byte more, or a range that runs past the
    // end, is refused instead of returning a short read.
    CHECK(PmTilesCard::read(archive, expected_size - 1, (uint8_t*)got, 1));
    CHECK(got[0] == kFar[std::strlen(kFar) - 1]);
    CHECK(!PmTilesCard::read(archive, expected_size, (uint8_t*)got, 1));
    CHECK(!PmTilesCard::read(archive, expected_size - 2, (uint8_t*)got, 3));
    CHECK(!PmTilesCard::read(archive, UINT64_MAX, (uint8_t*)got, 1));
    PmTilesCard::close(archive);
  }

  // What the archive scan treats as "no archive here".
  CHECK(PmTilesCard::open("0:/maps/topo/topo.pmtiles") == nullptr);
  CHECK(PmTilesCard::open("0:/maps/osm") == nullptr);   // a directory
  PmTilesCard::close(nullptr);

  CHECK(f_mount(nullptr, "0:", 0) == FR_OK);
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("pmtiles card: ok");
  return 0;
}
