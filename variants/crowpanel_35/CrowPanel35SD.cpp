// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Michael A. Cojocari and contributors
// Adapted from Camillia-MT's sd_soft_spi.cpp for WadaMesh's SD filesystem API.
#include "CrowPanel35SD.h"
#include "CrowPanel35SdCodec.h"

#include <Arduino.h>
#include <vfs_api.h>
#include <freertos/semphr.h>
#include "diskio_impl.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "soc/gpio_reg.h"
#include "rom/ets_sys.h"

static_assert(PIN_SD_SCLK >= 0 && PIN_SD_SCLK < 32, "SD SCLK must be GPIO0-31");
static_assert(PIN_SD_MISO >= 0 && PIN_SD_MISO < 32, "SD MISO must be GPIO0-31");
static_assert(PIN_SD_MOSI >= 0 && PIN_SD_MOSI < 32, "SD MOSI must be GPIO0-31");
static_assert(PIN_SD_CS >= 0 && PIN_SD_CS < 32, "SD CS must be GPIO0-31");

namespace {
constexpr const char* kMountPoint = "/sd";
constexpr uint8_t kMaxOpenFiles = 8;
constexpr uint32_t kClockMask = 1UL << PIN_SD_SCLK;
constexpr uint32_t kMosiMask = 1UL << PIN_SD_MOSI;
constexpr uint32_t kCsMask = 1UL << PIN_SD_CS;
constexpr uint8_t kReady = 0x00, kIdle = 0x01, kIllegal = 0x04;
constexpr uint8_t kSingle = 0xFE, kMulti = 0xFC, kStop = 0xFD;
sdcard_type_t s_type = CARD_NONE;
uint32_t s_sectors = 0;
BYTE s_drive = FF_DRV_NOT_USED;
bool s_mounted = false, s_vfs_registered = false, s_slow = true;
SemaphoreHandle_t s_mutex = nullptr;
SemaphoreHandle_t s_lifecycle_mutex = nullptr;

// FatFs serializes filesystem operations, but the health probe also sends SD
// commands outside FatFs. Keep each command/data sequence indivisible.
class CardLock {
  SemaphoreHandle_t _mutex;
public:
  explicit CardLock(SemaphoreHandle_t mutex = s_mutex) : _mutex(mutex) {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
  }
  ~CardLock() { xSemaphoreGiveRecursive(_mutex); }
  CardLock(const CardLock&) = delete;
  CardLock& operator=(const CardLock&) = delete;
};

uint8_t transfer(uint8_t out) {
  uint8_t in = 0;
  for (int bit = 7; bit >= 0; --bit) {
    REG_WRITE((out >> bit) & 1 ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, kMosiMask);
    if (s_slow) ets_delay_us(2);
    REG_WRITE(GPIO_OUT_W1TS_REG, kClockMask);
    if (s_slow) ets_delay_us(2);
    in = (uint8_t)((in << 1) | ((REG_READ(GPIO_IN_REG) >> PIN_SD_MISO) & 1));
    REG_WRITE(GPIO_OUT_W1TC_REG, kClockMask);
  }
  return in;
}

uint8_t receive() { return transfer(0xFF); }
void select() { REG_WRITE(GPIO_OUT_W1TC_REG, kCsMask); }
void release() {
  REG_WRITE(GPIO_OUT_W1TS_REG, kCsMask);
  receive();  // Clock MISO free after deselecting.
}
bool waitReady(uint32_t timeout) {
  const uint32_t start = millis();
  do {
    if (receive() == 0xFF) return true;
    delay(1);
  } while ((uint32_t)(millis() - start) < timeout);
  return false;
}

uint8_t command(uint8_t cmd, uint32_t arg) {
  select();
  if (cmd != 0 && cmd != 12 && !waitReady(500)) return 0xFF;
  const uint8_t frame[] = {
    (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
    (uint8_t)(arg >> 8), (uint8_t)arg
  };
  for (uint8_t byte : frame) transfer(byte);
  transfer(CrowPanel35SdCodec::commandCrc(frame, sizeof frame));
  if (cmd == 12) receive();  // STOP_TRANSMISSION has a leading stuff byte.
  uint8_t response = 0xFF;
  for (int i = 0; i < 10; ++i) {
    response = receive();
    if (!(response & 0x80)) break;
  }
  return response;
}

uint8_t appCommand(uint8_t cmd, uint32_t arg) {
  const uint8_t response = command(55, 0);
  return response <= kIdle ? command(cmd, arg) : response;
}

bool readBlock(uint8_t* data, size_t length) {
  const uint32_t start = millis();
  uint8_t token;
  do {
    token = receive();
    if (token == 0xFF) delay(1);
  } while (token == 0xFF && (uint32_t)(millis() - start) < 300);
  if (token != kSingle) return false;
  uint16_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    data[i] = receive();
    crc = CrowPanel35SdCodec::dataCrc(crc, data[i]);
  }
  const uint16_t expected = (uint16_t)receive() << 8;
  return (expected | receive()) == crc;
}

bool writeBlock(const uint8_t* data, uint8_t token) {
  if (!waitReady(1000)) return false;
  transfer(token);
  uint16_t crc = 0;
  for (size_t i = 0; i < 512; ++i) {
    transfer(data[i]);
    crc = CrowPanel35SdCodec::dataCrc(crc, data[i]);
  }
  transfer((uint8_t)(crc >> 8));
  transfer((uint8_t)crc);
  const bool accepted = (receive() & 0x1F) == 0x05;
  const bool ready = waitReady(1000);
  return accepted && ready;
}

bool cardInit() {
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  pinMode(PIN_SD_SCLK, OUTPUT);
  pinMode(PIN_SD_MOSI, OUTPUT);
  pinMode(PIN_SD_MISO, INPUT_PULLUP);
  REG_WRITE(GPIO_OUT_W1TC_REG, kClockMask);
  REG_WRITE(GPIO_OUT_W1TS_REG, kMosiMask);
  s_slow = true;
  s_type = CARD_NONE;
  s_sectors = 0;
  for (int i = 0; i < 10; ++i) receive();  // At least 74 power-up clocks, CS high.

  uint8_t response = 0xFF;
  for (int i = 0; i < 10 && response != kIdle; ++i) {
    response = command(0, 0);
    release();
    if (response != kIdle) delay(10);
  }
  if (response != kIdle) {
    Serial.printf("[sd] soft SPI: CMD0 failed (R1=0x%02X)\n", response);
    return false;
  }
  response = command(59, 1);
  release();
  if (response == (kIdle | kIllegal)) {
    Serial.println("[sd] soft SPI: card does not support write CRC checks");
  } else if (response != kIdle) {
    Serial.printf("[sd] soft SPI: CRC enable failed (R1=0x%02X)\n", response);
    return false;
  }

  sdcard_type_t type = CARD_NONE;
  uint32_t start = millis();
  response = command(8, 0x1AA);
  if (response == kIdle) {
    uint8_t echo[4];
    for (auto& byte : echo) byte = receive();
    release();
    if (echo[2] != 0x01 || echo[3] != 0xAA) {
      Serial.println("[sd] soft SPI: CMD8 voltage/check pattern mismatch");
      return false;
    }
    do {
      response = appCommand(41, 1UL << 30);
      release();
      if (response == kIdle) delay(1);
    } while (response == kIdle && (uint32_t)(millis() - start) < 1000);
    if (response != kReady) {
      Serial.printf("[sd] soft SPI: ACMD41 failed (R1=0x%02X)\n", response);
      return false;
    }
    response = command(58, 0);
    if (response != kReady) {
      release();
      Serial.printf("[sd] soft SPI: CMD58 failed (R1=0x%02X)\n", response);
      return false;
    }
    uint8_t ocr[4];
    for (auto& byte : ocr) byte = receive();
    release();
    if (!(ocr[0] & 0x80) || !(ocr[1] & 0x10)) {
      Serial.println("[sd] soft SPI: card not ready for 3.3 V operation");
      return false;
    }
    type = (ocr[0] & 0x40) ? CARD_SDHC : CARD_SD;
  } else {
    release();
    if (response != (kIdle | kIllegal)) {
      Serial.printf("[sd] soft SPI: CMD8 failed (R1=0x%02X)\n", response);
      return false;
    }
    response = appCommand(41, 0);
    release();
    if (response <= kIdle) {
      type = CARD_SD;
      while (response == kIdle && (uint32_t)(millis() - start) < 1000) {
        delay(1);
        response = appCommand(41, 0);
        release();
      }
    } else {
      type = CARD_MMC;
      start = millis();
      do {
        response = command(1, 0);
        release();
        if (response == kIdle) delay(1);
      } while (response == kIdle && (uint32_t)(millis() - start) < 1000);
    }
    if (response != kReady) {
      Serial.printf("[sd] soft SPI: legacy init failed (R1=0x%02X)\n", response);
      return false;
    }
  }
  if (type != CARD_SDHC) {
    response = command(16, 512);
    release();
    if (response != kReady) {
      Serial.printf("[sd] soft SPI: CMD16 failed (R1=0x%02X)\n", response);
      return false;
    }
  }
  uint8_t csd[16];
  const bool csd_ok = command(9, 0) == kReady && readBlock(csd, sizeof csd);
  release();
  if (!csd_ok) {
    Serial.println("[sd] soft SPI: CSD read/CRC failed");
    return false;
  }
  s_sectors = CrowPanel35SdCodec::sectorsFromCsd(csd);
  if (!s_sectors || (type != CARD_SDHC && s_sectors > UINT32_MAX / 512UL + 1)) {
    Serial.println("[sd] soft SPI: invalid or unsupported capacity");
    s_sectors = 0;
    return false;
  }
  s_type = type;
  s_slow = false;
  return true;
}

bool cardReady() {
  if (s_type == CARD_NONE) return false;
  const uint8_t response = command(13, 0);
  const uint8_t status = response == kReady ? receive() : 0xFF;
  release();
  return response == kReady && status == 0;
}

bool readSectors(uint8_t* data, uint32_t sector, unsigned count) {
  const uint32_t address = s_type == CARD_SDHC ? sector : sector * 512;
  bool ok;
  if (count == 1) {
    ok = command(17, address) == kReady && readBlock(data, 512);
  } else {
    ok = command(18, address) == kReady;
    if (ok) {
      for (unsigned i = 0; ok && i < count; ++i) ok = readBlock(data + i * 512, 512);
      const bool stopped = command(12, 0) == kReady;
      const bool ready = waitReady(500);
      ok = ok && stopped && ready;
    }
  }
  release();
  return ok;
}

bool writeSectors(const uint8_t* data, uint32_t sector, unsigned count) {
  const uint32_t address = s_type == CARD_SDHC ? sector : sector * 512;
  bool ok;
  if (count == 1) {
    ok = command(24, address) == kReady && writeBlock(data, kSingle);
  } else {
    ok = command(25, address) == kReady;
    if (ok) {
      for (unsigned i = 0; ok && i < count; ++i) ok = writeBlock(data + i * 512, kMulti);
      if (waitReady(1000)) {
        transfer(kStop);  // Only an accepted CMD25 owes a stop token.
        receive();
        const bool ready = waitReady(1000);
        ok = ok && ready;
      } else {
        ok = false;
      }
    }
  }
  release();
  return ok;
}

DSTATUS diskStatus(unsigned char) {
  CardLock lock;
  return cardReady() ? 0 : STA_NOINIT;
}
DSTATUS diskInit(unsigned char drive) { return diskStatus(drive); }

DRESULT diskRead(unsigned char, unsigned char* data, uint32_t sector, unsigned count) {
  CardLock lock;
  if (s_type == CARD_NONE) return RES_NOTRDY;
  if (!data || !CrowPanel35SdCodec::validRange(s_sectors, sector, count)) {
    Serial.printf("[sd] soft SPI: invalid read sector=%lu count=%u\n", (unsigned long)sector, count);
    return RES_PARERR;
  }
  if (readSectors(data, sector, count) || readSectors(data, sector, count)) return RES_OK;
  Serial.printf("[sd] soft SPI: read failed sector=%lu count=%u\n", (unsigned long)sector, count);
  return RES_ERROR;
}
DRESULT diskWrite(unsigned char, const unsigned char* data, uint32_t sector, unsigned count) {
  CardLock lock;
  if (s_type == CARD_NONE) return RES_NOTRDY;
  if (!data || !CrowPanel35SdCodec::validRange(s_sectors, sector, count)) {
    Serial.printf("[sd] soft SPI: invalid write sector=%lu count=%u\n", (unsigned long)sector, count);
    return RES_PARERR;
  }
  if (writeSectors(data, sector, count) || writeSectors(data, sector, count)) return RES_OK;
  Serial.printf("[sd] soft SPI: write failed sector=%lu count=%u\n", (unsigned long)sector, count);
  return RES_ERROR;
}
DRESULT diskIoctl(unsigned char, unsigned char cmd, void* data) {
  CardLock lock;
  if (s_type == CARD_NONE) return RES_NOTRDY;
  if (cmd != CTRL_SYNC && !data) {
    Serial.printf("[sd] soft SPI: missing ioctl output for %u\n", (unsigned)cmd);
    return RES_PARERR;
  }
  switch (cmd) {
    case CTRL_SYNC: {
      select();
      const bool ready = waitReady(500);
      release();
      if (!ready) Serial.println("[sd] soft SPI: sync timed out");
      return ready ? RES_OK : RES_ERROR;
    }
    case GET_SECTOR_COUNT: *(DWORD*)data = s_sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)data = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)data = 1; return RES_OK;
    default:
      Serial.printf("[sd] soft SPI: unsupported ioctl %u\n", (unsigned)cmd);
      return RES_PARERR;
  }
}
const ff_diskio_impl_t kDiskImpl = { diskInit, diskStatus, diskRead, diskWrite, diskIoctl };

void driveName(char (&name)[8], uint8_t drive) { snprintf(name, sizeof name, "%u:", (unsigned)drive); }

bool space(uint64_t& total, uint64_t& used) {
  const uint8_t drive = SD.driveNumber();
  if (drive == FF_DRV_NOT_USED) return false;
  char name[8];
  driveName(name, drive);
  FATFS* fat = nullptr;
  DWORD free_clusters;
  // Do not hold CardLock while acquiring FatFs' volume lock: disk callbacks
  // acquire CardLock in the opposite order.
  const FRESULT result = f_getfree(name, &free_clusters, &fat);
  if (result != FR_OK || !fat || fat->n_fatent < 2 || free_clusters > fat->n_fatent - 2) {
    Serial.printf("[sd] soft SPI: free-space read failed (%u)\n", (unsigned)result);
    return false;
  }
  total = (uint64_t)(fat->n_fatent - 2) * fat->csize * 512ULL;
  used = total - (uint64_t)free_clusters * fat->csize * 512ULL;
  return true;
}
}  // namespace

CrowPanel35SD SD;
CrowPanel35SD::CrowPanel35SD() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}

bool CrowPanel35SD::begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
  if (!s_lifecycle_mutex) s_lifecycle_mutex = xSemaphoreCreateRecursiveMutex();
  if (!s_mutex || !s_lifecycle_mutex) {
    Serial.println("[sd] soft SPI: mutex allocation failed");
    return false;
  }
  CardLock lifecycle(s_lifecycle_mutex);
  {
    CardLock lock;
    if (s_mounted) return true;
  }
  {
    CardLock lock;
    if (!cardInit()) return false;
  }
  const esp_err_t drive_error = ff_diskio_get_drive(&s_drive);
  if (drive_error != ESP_OK || s_drive == FF_DRV_NOT_USED) {
    Serial.printf("[sd] soft SPI: no FatFs drive (%s)\n", esp_err_to_name(drive_error));
    end();
    return false;
  }
  ff_diskio_register(s_drive, &kDiskImpl);
  char name[8];
  driveName(name, s_drive);
  FATFS* fat = nullptr;
  const esp_err_t vfs_error = esp_vfs_fat_register(kMountPoint, name, kMaxOpenFiles, &fat);
  if (vfs_error != ESP_OK) {
    Serial.printf("[sd] soft SPI: VFS registration failed (%s)\n", esp_err_to_name(vfs_error));
    end();
    return false;
  }
  s_vfs_registered = true;
  const FRESULT mount_error = f_mount(fat, name, 1);
  if (mount_error != FR_OK) {
    Serial.printf("[sd] soft SPI: FAT mount failed (%u); card left unchanged\n", (unsigned)mount_error);
    end();
    return false;
  }
  _impl->mountpoint(kMountPoint);
  {
    CardLock lock;
    s_mounted = true;
  }
  Serial.printf("[sd] soft SPI mounted: %llu MB\n", (unsigned long long)(cardSize() / (1024ULL * 1024)));
  return true;
}

void CrowPanel35SD::end() {
  if (!s_lifecycle_mutex) return;  // No begin() has created a mount.
  CardLock lifecycle(s_lifecycle_mutex);
  _impl->mountpoint(nullptr);
  if (s_vfs_registered) {
    char name[8];
    driveName(name, s_drive);
    const FRESULT result = f_mount(nullptr, name, 0);
    if (result != FR_OK) Serial.printf("[sd] soft SPI: FAT unmount failed (%u)\n", (unsigned)result);
    const esp_err_t vfs_error = esp_vfs_fat_unregister_path(kMountPoint);
    if (vfs_error != ESP_OK)
      Serial.printf("[sd] soft SPI: VFS unregister failed (%s)\n", esp_err_to_name(vfs_error));
  }
  if (s_drive != FF_DRV_NOT_USED) ff_diskio_unregister(s_drive);
  if (!s_mutex) return;
  CardLock lock;
  s_drive = FF_DRV_NOT_USED;
  s_type = CARD_NONE;
  s_sectors = 0;
  s_mounted = false;
  s_vfs_registered = false;
}

sdcard_type_t CrowPanel35SD::cardType() const {
  if (!s_mutex) return CARD_NONE;
  CardLock lock;
  return s_mounted ? s_type : CARD_NONE;
}
uint64_t CrowPanel35SD::cardSize() const {
  if (!s_mutex) return 0;
  CardLock lock;
  return s_mounted ? (uint64_t)s_sectors * 512ULL : 0;
}
uint8_t CrowPanel35SD::driveNumber() const {
  if (!s_mutex) return FF_DRV_NOT_USED;
  CardLock lock;
  return s_mounted ? s_drive : FF_DRV_NOT_USED;
}
bool CrowPanel35SD::probeAlive() const {
  if (!s_mutex) return false;
  CardLock lock;
  return s_mounted && cardReady();
}
uint64_t CrowPanel35SD::totalBytes() const {
  uint64_t total = 0, used = 0;
  return space(total, used) ? total : 0;
}
uint64_t CrowPanel35SD::usedBytes() const {
  uint64_t total = 0, used = 0;
  return space(total, used) ? used : 0;
}
