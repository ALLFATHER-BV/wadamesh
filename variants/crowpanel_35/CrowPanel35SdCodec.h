// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Michael A. Cojocari and contributors
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace CrowPanel35SdCodec {

inline uint8_t commandCrc(const uint8_t* frame, size_t length) {
  uint8_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    uint8_t byte = frame[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (uint8_t)(crc << 1);
      if ((byte ^ crc) & 0x80) crc ^= 0x09;
      byte = (uint8_t)(byte << 1);
    }
  }
  return (uint8_t)((crc << 1) | 1);
}

inline uint16_t dataCrc(uint16_t crc, uint8_t byte) {
  crc ^= (uint16_t)byte << 8;
  for (int bit = 0; bit < 8; ++bit)
    crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
  return crc;
}

inline uint32_t sectorsFromCsd(const uint8_t (&csd)[16]) {
  uint64_t sectors;
  const uint8_t version = csd[0] >> 6;
  if (version == 1) {
    const uint32_t size = ((uint32_t)(csd[7] & 0x3F) << 16)
                       | ((uint32_t)csd[8] << 8) | csd[9];
    sectors = ((uint64_t)size + 1) * 1024;
  } else if (version == 0) {
    const uint32_t blockLength = csd[5] & 0x0F;
    const uint32_t size = ((uint32_t)(csd[6] & 0x03) << 10)
                       | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
    const uint32_t multiplier = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
    sectors = (((uint64_t)size + 1) << (multiplier + 2 + blockLength)) / 512;
  } else {
    return 0;
  }
  return sectors <= UINT32_MAX ? (uint32_t)sectors : 0;
}

inline bool validRange(uint32_t sectors, uint32_t sector, unsigned count) {
  return count && sector < sectors && count <= sectors - sector;
}

}  // namespace CrowPanel35SdCodec
