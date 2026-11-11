# SPDX-License-Identifier: GPL-3.0-or-later

# PlatformIO pre-script: build FatFs with exFAT for the Arduino-ESP32 boards.
#
# Arduino-ESP32 2.0.17 links a precompiled libfatfs.a (ESP-IDF v4.4.7) built with
# FF_FS_EXFAT=0, so every SDXC card (64 GB and up ships as exFAT) failed to mount.
# arch/esp32/fatfs_exfat is that same component, copied unchanged from ESP-IDF
# v4.4.7 (components/fatfs: diskio/, port/freertos/, src/, vfs/; the framework's
# headers are byte-identical to it), except for two lines in src/ffconf.h:
# FF_FS_EXFAT=1 and the heap LFN buffer (see the comment there).
#
# - Its objects go straight into the program, so the linker resolves every f_*,
#   ff_* and esp_vfs_fat_* symbol from them and never pulls a member of
#   libfatfs.a. Object files are not archive members: there is no duplicate.
# - exFAT changes the layout of FATFS, FIL and FILINFO. Every translation unit
#   that includes ff.h (the Arduino SD, SD_MMC and FFat libraries, sd_diskio,
#   CrowPanel35SD, UsbFilesSession) must see this ffconf.h, not the framework's.
#   A pre-script runs before the framework adds its include paths, so the ones
#   prepended here come first, and every library builder clones this env.
import os

Import("env")  # noqa: F821 - provided by PlatformIO/SCons

ROOT = os.path.join(env.subst("$PROJECT_DIR"), "arch", "esp32", "fatfs_exfat")  # noqa: F821

env.Prepend(CPPPATH=[os.path.join(ROOT, d) for d in ("src", "vfs", "diskio")])  # noqa: F821
env.BuildSources(os.path.join("$BUILD_DIR", "fatfs_exfat"), ROOT)  # noqa: F821
