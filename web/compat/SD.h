#pragma once
// microSD on the web board: there is no card (CAP_SD 0). SD exists so shared code that
// names it links; begin() fails and every file operation finds nothing.
#include "arduino/FS.h"
typedef enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC, CARD_UNKNOWN } sdcard_type_t;
class SPIClass;
namespace fs {
class SDFS : public FS {
public:
  SDFS();
  bool begin(uint8_t ssPin = 0, SPIClass* spi = nullptr, uint32_t frequency = 4000000,
             const char* mountpoint = "/sd", uint8_t max_files = 5, bool format_if_empty = false) {
    (void)ssPin; (void)spi; (void)frequency; (void)mountpoint; (void)max_files; (void)format_if_empty;
    return false;
  }
  bool begin(uint8_t ssPin, SPIClass& spi, uint32_t frequency = 4000000,
             const char* mountpoint = "/sd", uint8_t max_files = 5, bool format_if_empty = false) {
    return begin(ssPin, &spi, frequency, mountpoint, max_files, format_if_empty);   // arduino-esp32's signature
  }
  void end() {}
  sdcard_type_t cardType() { return CARD_NONE; }
  uint64_t cardSize() { return 0; }
  size_t numSectors() { return 0; }
  size_t sectorSize() { return 512; }
  uint64_t totalBytes() { return 0; }
  uint64_t usedBytes() { return 0; }
  bool readRAW(uint8_t*, uint32_t) { return false; }
  bool writeRAW(uint8_t*, uint32_t) { return false; }
protected:
  uint8_t _pdrv = 0xFF;   // as arduino-esp32's SDFS (the UI reads it for f_mkfs/f_getfree)
};
}
extern fs::SDFS SD;
