// SPDX-License-Identifier: GPL-3.0-or-later
#include "PmTilesCard.h"

#include "device_caps.h"

#if CAP_MAP_PMTILES

#include <new>

#include "ff.h"

namespace PmTilesCard {

struct Archive {
  FIL file;
};

// ff_memalloc: the board's FatFs allocator, so the FIL sector buffer lands
// where that board's sdkconfig wants FatFs buffers (some keep them out of PSRAM).
Archive* open(const char* fatfs_path) {
  if (!fatfs_path) return nullptr;
  void* memory = ff_memalloc(sizeof(Archive));
  if (!memory) return nullptr;
  Archive* archive = new (memory) Archive;
  if (f_open(&archive->file, fatfs_path, FA_READ) != FR_OK) {
    ff_memfree(memory);
    return nullptr;
  }
  return archive;
}

void close(Archive* archive) {
  if (!archive) return;
  f_close(&archive->file);
  ff_memfree(archive);
}

uint64_t size(const Archive* archive) {
  return archive ? static_cast<uint64_t>(f_size(&archive->file)) : 0;
}

bool read(Archive* archive, uint64_t offset, uint8_t* dst, size_t length) {
  if (!archive || !dst) return false;
  const uint64_t file_size = f_size(&archive->file);
  if (offset > file_size || length > file_size - offset) return false;
  if (f_lseek(&archive->file, static_cast<FSIZE_t>(offset)) != FR_OK) return false;
  while (length > 0) {
    UINT got = 0;
    if (f_read(&archive->file, dst, static_cast<UINT>(length), &got) != FR_OK || got == 0)
      return false;
    dst += got;
    length -= got;
  }
  return true;
}

}  // namespace PmTilesCard

#endif  // CAP_MAP_PMTILES
