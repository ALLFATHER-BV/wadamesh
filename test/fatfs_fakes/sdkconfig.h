// SPDX-License-Identifier: GPL-3.0-or-later
// The Arduino-ESP32 2.0.17 FATFS settings arch/esp32/fatfs_exfat/src/ffconf.h reads,
// copied from the framework's esp32s3 sdkconfig.h. Host tests only.
#pragma once
#define CONFIG_FATFS_CODEPAGE 850
#define CONFIG_FATFS_LFN_STACK 1
#define CONFIG_FATFS_MAX_LFN 255
#define CONFIG_FATFS_API_ENCODING_UTF_8 1
#define CONFIG_FATFS_FS_LOCK 0
#define CONFIG_FATFS_TIMEOUT_MS 10000
#define CONFIG_FATFS_PER_FILE_CACHE 1
#define CONFIG_WL_SECTOR_SIZE 4096
