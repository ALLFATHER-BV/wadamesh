// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

// A PMTiles archive on the microSD card, read through FatFs itself.
//
// Archive files run past 4 GiB, which only exFAT can hold. On the card's own
// FatFs (exFAT enabled, 64-bit file sizes) they are fine, but the POSIX/VFS
// layer and Arduino's File stop at 2 GiB: off_t is a 32-bit long here and
// File::seek takes a uint32_t. So archives are opened by FatFs path, with
// the drive in front: "0:/maps/osm/osm.pmtiles".
//
// The handle is opaque so FatFs' BYTE/WORD typedefs stay out of UITask.cpp.
namespace PmTilesCard {

struct Archive;

Archive* open(const char* fatfs_path);   // nullptr when missing, a directory, or no memory
void close(Archive* archive);            // accepts nullptr
uint64_t size(const Archive* archive);
// Exactly `length` bytes at `offset`; false on a read error or past the end.
bool read(Archive* archive, uint64_t offset, uint8_t* dst, size_t length);

}  // namespace PmTilesCard
