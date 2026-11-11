# SPDX-License-Identifier: GPL-3.0-or-later

# ESP-IDF hard-codes FF_FS_EXFAT 0 in components/fatfs/src/ffconf.h (there is no
# Kconfig option for it), so SD_MMC could not mount exFAT cards, which is how
# every SDXC card (64 GB and up) ships. The IDF builds (Tanmatsu, T-Display P4,
# CrowPanel P4) all compile FatFs from that tree, so they switch it on here. IDF's
# own vfs_fat.c already handles FF_FS_EXFAT; LFN, which exFAT requires, is on in
# tanmatsu/sdkconfigs/general (CONFIG_FATFS_LFN_HEAP), shared by all three.
#
# The PlatformIO boards link a precompiled FatFs instead; scripts/build/fatfs_exfat.py
# covers them.
#
# Idempotent and fail-closed, like patch_lvgl_anim_uaf.py:
#   python3 patch_idf_fatfs_exfat.py --patch-file "$IDF_PATH/components/fatfs/src/ffconf.h"

import sys

# (old, new) pairs, each applied exactly once.
PATCHES = [
    ("#define FF_FS_EXFAT\t\t0",
     "#define FF_FS_EXFAT\t\t1\t/* wadamesh: exFAT microSD (scripts/build/patch_idf_fatfs_exfat.py) */"),
    # IDF maps FF_USE_LABEL to CONFIG_FATFS_USE_LABEL, which sdkconfig leaves undefined
    # while the option is off. #if treats that as 0, but exFAT's dir_read() tests
    # FF_USE_LABEL in plain C (ff.c), where an undefined name does not compile.
    ("#define FF_USE_LABEL\tCONFIG_FATFS_USE_LABEL",
     "#ifdef CONFIG_FATFS_USE_LABEL\t/* wadamesh: defined when off too, for exFAT's dir_read() */\n"
     "#define FF_USE_LABEL\tCONFIG_FATFS_USE_LABEL\n"
     "#else\n"
     "#define FF_USE_LABEL\t0\n"
     "#endif"),
]


def patch_source(source):
    changed = False
    for old, new in PATCHES:
        if new in source:
            continue
        if source.count(old) != 1:
            raise RuntimeError("ffconf.h line %r does not match ESP-IDF 5.5 (IDF version drift?)" % old)
        source = source.replace(old, new, 1)
        changed = True
    return source, changed


def patch_file(path):
    with open(path, encoding="utf-8", newline="") as source_file:
        source = source_file.read()
    patched, changed = patch_source(source)
    if changed:
        with open(path, "w", encoding="utf-8", newline="") as source_file:
            source_file.write(patched)
    return changed


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--patch-file":
        raise SystemExit("usage: patch_idf_fatfs_exfat.py --patch-file PATH")
    try:
        changed = patch_file(sys.argv[2])
    except (OSError, RuntimeError) as error:
        print("patch_idf_fatfs_exfat: ERROR: %s" % error, file=sys.stderr)
        raise SystemExit(1)
    print("patch_idf_fatfs_exfat: %s" % ("patched" if changed else "already patched"))
