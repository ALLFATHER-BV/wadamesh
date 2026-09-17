#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "helpers/esp32/WebFileTransferProtocol.h"

#ifndef EXPECT_FILE_TRANSFER
#define EXPECT_FILE_TRANSFER 0
#endif

#ifndef EXPECT_FILE_TRANSFER_SDMMC
#define EXPECT_FILE_TRANSFER_SDMMC 0
#endif

#include "helpers/esp32/WebFileTransferConfig.h"

static_assert(WADA_WEB_FILE_TRANSFER == EXPECT_FILE_TRANSFER,
              "unexpected file-transfer capability");
static_assert(WADA_WEB_FILE_TRANSFER_SDMMC == EXPECT_FILE_TRANSFER_SDMMC,
              "unexpected file-transfer storage backend");

int main() {
  using namespace WebFileTransferProtocol;

  const uint8_t crc_input[] = "123456789";
  uint32_t crc = crc32Update(0xFFFFFFFFu, crc_input, strlen((const char*)crc_input));
  assert((crc ^ 0xFFFFFFFFu) == 0xCBF43926u);

  const uint8_t offset[] = {0x78, 0x56, 0x34, 0x12};
  assert(readLe32(offset) == 0x12345678u);

  uint32_t parsed = 0;
  assert(parseHex32("cBf43926", &parsed));
  assert(parsed == 0xCBF43926u);
  assert(!parseHex32("CBF4392", &parsed));
  assert(!parseHex32("CBF4392Z", &parsed));

  assert(fileNameValid("photo-01.png"));
  assert(fileNameValid("firmware_backup.bin"));
  assert(!fileNameValid(""));
  assert(!fileNameValid(".hidden"));
  assert(!fileNameValid("pending.part"));
  assert(!fileNameValid("nested/file.txt"));
  assert(!fileNameValid("space name.txt"));

  char long_name[66];
  memset(long_name, 'a', sizeof(long_name));
  long_name[65] = '\0';
  assert(!fileNameValid(long_name));

  MapTilePath tile = {};
  assert(mapTilePathValid("3/0/0.png", &tile));
  assert(tile.zoom == 3 && tile.x == 0 && tile.y == 0);
  assert(mapTilePathValid("15/17642/10765.png", &tile));
  assert(tile.zoom == 15 && tile.x == 17642 && tile.y == 10765);
  assert(mapTilePathValid("19/524287/524287.PNG", &tile));
  assert(!mapTilePathValid("2/0/0.png"));
  assert(!mapTilePathValid("20/0/0.png"));
  assert(!mapTilePathValid("3/8/0.png"));
  assert(!mapTilePathValid("3/0/8.png"));
  assert(!mapTilePathValid("15/x/10765.png"));
  assert(!mapTilePathValid("15/17642/-1.png"));
  assert(!mapTilePathValid("15/17642/10765.jpg"));
  assert(!mapTilePathValid("/15/17642/10765.png"));
  assert(!mapTilePathValid("15/17642/../10765.png"));
  assert(!mapTilePathValid("15/17642/10765.png/extra"));

  const uint8_t png_signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  const uint8_t jpeg_signature[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0 };
  assert(pngSignatureValid(png_signature, sizeof png_signature));
  assert(!pngSignatureValid(png_signature, sizeof png_signature - 1));
  assert(!pngSignatureValid(jpeg_signature, sizeof jpeg_signature));

  assert(readablePath("/screenshots/capture.png"));
  assert(readablePath("/transfer/archive.bin"));
  assert(readablePath("/wadamesh-crash.elf"));
  assert(readablePath("/wadamesh-crash.elf.txt"));
  assert(readablePath("/meshcore-backup.json"));
  assert(readablePath("/meshcore-20260907-120000.json"));
  assert(readablePath("/meshcore-backup-123.json"));
  assert(!readablePath("/transfer/.upload.part"));
  assert(!readablePath("/transfer/../secret.txt"));
  assert(!readablePath("/transfer/nested/file.txt"));
  assert(!readablePath("/contacts3"));
  assert(!readablePath("/other.json"));
  assert(!readablePath("/meshcore-.json"));
  assert(!readablePath("/meshcore-backup.json/extra"));
  assert(!readablePath("/other/archive.bin"));
  return 0;
}