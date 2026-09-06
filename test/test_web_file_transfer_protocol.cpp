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

  assert(readablePath("/screenshots/capture.png"));
  assert(readablePath("/transfer/archive.bin"));
  assert(!readablePath("/transfer/.upload.part"));
  assert(!readablePath("/transfer/../secret.txt"));
  assert(!readablePath("/transfer/nested/file.txt"));
  assert(!readablePath("/other/archive.bin"));
  return 0;
}