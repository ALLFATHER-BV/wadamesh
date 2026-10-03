// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <string.h>
#include "CrowPanel35SdCodec.h"

int main() {
  using namespace CrowPanel35SdCodec;
  const uint8_t cmd0[] = {0x40, 0, 0, 0, 0};
  const uint8_t cmd8[] = {0x48, 0, 0, 0x01, 0xAA};
  assert(commandCrc(cmd0, sizeof cmd0) == 0x95);
  assert(commandCrc(cmd8, sizeof cmd8) == 0x87);
  uint16_t crc = 0;
  for (const char* byte = "123456789"; *byte; ++byte) crc = dataCrc(crc, (uint8_t)*byte);
  assert(crc == 0x31C3);

  uint8_t csd[16] = {};
  csd[0] = 0x40;
  csd[8] = 0xFF;
  csd[9] = 0xFF;
  assert(sectorsFromCsd(csd) == 67108864);  // 32 GiB, block-addressed.
  csd[7] = 0x3F;
  assert(sectorsFromCsd(csd) == 0);         // 2 TiB overflows 32-bit sector count.
  csd[0] = 0x80;
  assert(sectorsFromCsd(csd) == 0);         // Unsupported CSD structure.

  memset(csd, 0, sizeof csd);
  csd[5] = 9;
  csd[6] = 3;
  csd[7] = 0xFF;
  csd[8] = 0xC0;
  csd[9] = 3;
  csd[10] = 0x80;
  assert(sectorsFromCsd(csd) == 2097152);   // 1 GiB, byte-addressed.
  assert(validRange(100, 0, 100));
  assert(validRange(100, 99, 1));
  assert(!validRange(100, 100, 1));
  assert(!validRange(100, 0, 0));
  assert(!validRange(100, 99, 2));
  assert(!validRange(UINT32_MAX, UINT32_MAX - 1, UINT32_MAX));
}
