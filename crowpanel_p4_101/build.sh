#!/usr/bin/env bash
# wadamesh / Elecrow CrowPanel Advanced 10.1" ESP32-P4 HMI build wrapper. EXPERIMENTAL.
# Standalone ESP-IDF app modelled on tdisplay_p4/build.sh: reuses the Tanmatsu's project-local
# ESP-IDF 5.5.1 (esp32p4) via the ../tanmatsu symlinks and the shared P4 components.
#   ./build.sh build                      # board revision V1.2 (default, newest)
#   WADA_CP_REV=1.1 ./build.sh build      # V1.1 (SX1262 DIO1/RST on GPIO53/54)
#   WADA_CP_REV=1.0 ./build.sh build      # V1.0 (same LoRa pins as V1.1, reversed 1-bit C6 SDIO)
#   ./build.sh merge                      # build + one flashable image at 0x0 (build/<target>/*-merged.bin)
#   ./build.sh flash -p /dev/cu.usbserial-XXXX
#   ./build.sh fullclean                  # remove this revision's build tree only
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

CP_REV="${WADA_CP_REV:-1.2}"
case "$CP_REV" in
  1.2) CP_SDIO_FRAG="sdio_v11" ;;
  1.1) CP_SDIO_FRAG="sdio_v11" ;;
  1.0) CP_SDIO_FRAG="sdio_v10" ;;
  *) echo "[build.sh] WADA_CP_REV must be 1.0, 1.1 or 1.2 (got '$CP_REV')" >&2; exit 1 ;;
esac
export WADA_CP_REV="$CP_REV"
P4_BUILD_TARGET="crowpanel_p4_101_v${CP_REV/./}"
P4_BUILD_DIR="$PROJECT_DIR/build/$P4_BUILD_TARGET"

# This project intentionally applies compatibility fixes inside managed_components
# before every build. ESP-IDF's stock fullclean also runs remove_managed_components,
# which rejects those expected hash changes and aborts. A P4 full clean therefore
# removes only the generated CMake/Ninja tree; dependencies stay in place and the
# idempotent patches below remain valid.
if [ "$#" -eq 1 ] && [ "$1" = "fullclean" ]; then
  # Plain rm -rf: cmake is not on PATH before export.sh. Each revision has its own build tree
  # (and its own sdkconfig inside it), so this removes only the selected one.
  if [ -d "$P4_BUILD_DIR" ]; then
    rm -rf "${P4_BUILD_DIR:?}" || { echo "[build.sh] FAILED to remove $P4_BUILD_DIR" >&2; exit 1; }
  fi
  if [ -d "$P4_BUILD_DIR" ]; then
    echo "[build.sh] FAILED to remove $P4_BUILD_DIR" >&2
    exit 1
  fi
  echo "[build.sh] removed build/$P4_BUILD_TARGET (patched managed components preserved)"
  exit 0
fi

# Keep the baked-in translation + Lua app tables in step with deploy/apps/.
# Same for the seeded Lua apps. The PlatformIO envs get this from a pre: hook;
# the IDF builds need it here or the P4 boards ship stale copies.
python3 "$(cd "$(dirname "$0")/.." && pwd)/scripts/build/pre_gen_baked.py"
# LVGL is vendored (gitignored) by fetch-deps.sh, so a copy vendored before the
# anim_timer use-after-free fix (#428) still needs it. Idempotent; fails on drift.
python3 "$(cd "$(dirname "$0")/.." && pwd)/scripts/build/patch_lvgl_anim_uaf.py" \
  --patch-file "$(cd "$(dirname "$0")" && pwd)/components/lvgl/upstream/src/misc/lv_anim.c"
python3 "$(cd "$(dirname "$0")/.." && pwd)/scripts/build/patch_lvgl_arabic.py" \
  --patch-file "$(cd "$(dirname "$0")" && pwd)/components/lvgl/upstream/src/misc/lv_txt_ap.c"
cd "$PROJECT_DIR"
export IDF_TOOLS_PATH="$PWD/esp-idf-tools"
# VS Code may launch this wrapper from PlatformIO's virtualenv. Pin the
# project-local IDF environment before export.sh inspects that unrelated Python.
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
  for idf_py_env in "$IDF_TOOLS_PATH"/python_env/idf5.5_py*_env; do
    if [ -x "$idf_py_env/bin/python" ]; then
      export IDF_PYTHON_ENV_PATH="$idf_py_env"
      break
    fi
  done
fi
unset VIRTUAL_ENV CONDA_PREFIX
# shellcheck disable=SC1091
source esp-idf/export.sh >/dev/null 2>&1

WADA_FW_TAG="${WADA_FW_TAG:-$(git describe --tags --match 'beta_*' --always 2>/dev/null || echo dev)}"
WADA_FW_DATE="$(date '+%-d %b %Y')"
IDF_ARGS=(-B "build/$P4_BUILD_TARGET" \
  -DDEVICE=crowpanel_p4_101 \
  -DSDKCONFIG="$P4_BUILD_DIR/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/wadamesh;sdkconfigs/crowpanel_p4_101;sdkconfigs/$CP_SDIO_FRAG" \
  -DWADA_FW_TAG="$WADA_FW_TAG" -DWADA_FW_DATE="$WADA_FW_DATE" \
  -DIDF_TARGET=esp32p4)

# A fresh clone has no managed_components yet. Configure once to download them,
# then apply the compatibility patches below before the first compilation.
if [ ! -f managed_components/espressif__libsodium/CMakeLists.txt ]; then
  idf.py "${IDF_ARGS[@]}" reconfigure
fi

# --- Build-time patch: libsodium forced includes in paths with spaces -----------------------------
# The managed component emits `SHELL:-include <absolute path>`. CMake leaves that
# path unquoted in Ninja, so a checkout such as "T7 Shield" reaches GCC as two
# input files. GCC accepts the joined -include<path> form, which cannot split.
SODIUM_CMAKE="managed_components/espressif__libsodium/CMakeLists.txt"
if [ -f "$SODIUM_CMAKE" ] && grep -q 'SHELL:-include' "$SODIUM_CMAKE"; then
  sed -i '' -e 's|SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}|-include${CMAKE_CURRENT_SOURCE_DIR}|g' \
             -e 's|SHELL:-include${CMAKE_CURRENT_SOURCE_DIR}|-include${CMAKE_CURRENT_SOURCE_DIR}|g' "$SODIUM_CMAKE"
  echo "[build.sh] patched libsodium forced includes (space-safe paths)"
fi

# --- Build-time patch: SD_MMC internal pull-ups (P4 boards) ------------------------------------------
# The board has no external pull-ups on the SD data lines; the IDF sdmmc host explicitly FLOATS the
# pads unless SDMMC_SLOT_FLAG_INTERNAL_PULLUP is set (so pre-begin gpio_pullup_en gets undone).
# Meck-P4's working SD init sets the flag; Arduino's SD_MMC never does and has no API for it. Patch
# the P4's own managed copy. Idempotent.
SDMMC_C="managed_components/espressif__arduino-esp32/libraries/SD_MMC/src/SD_MMC.cpp"
if [ -f "$SDMMC_C" ] && ! grep -q 'wadamesh P4: internal pullups' "$SDMMC_C"; then
  sed -i '' 's|sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();|sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();\n  slot_config.flags \|= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // wadamesh P4: internal pullups (no external ones on the SD lines)|' "$SDMMC_C"
  echo "[build.sh] patched arduino SD_MMC.cpp (internal pull-ups on the SD slot)"
fi
# --- Build-time patch: a dead C6 must not spin the SDIO rx task -----------------------------------
# With CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE=n (see sdkconfigs/tdisplay_p4 for why it is
# off), sdio_read_task posts ESP_HOSTED_EVENT_TRANSPORT_FAILURE and then `continue`s: straight back
# into a register read that is going to fail again. On a unit whose C6 runs factory ESP-AT the
# transport NEVER comes up, so that is an unbounded retry storm: measured at ~16 failed reads per
# second, forever, with the whole transport re-initialising on top of it. It floods the USB-CDC the
# companion app talks over and keeps a task busy for the life of the session.
#
# Turning the restart back on is not the answer: that is the boot loop this board had before.
# Both are the same mistake: a co-processor that cannot be reached is a permanent condition, and
# the driver treats it as transient. Park the task instead. The failure event has already been
# posted, Wi-Fi and BLE are simply unavailable, and everything else on the device keeps running.
# Idempotent.
HOSTED_SDIO="managed_components/nicolaielectronics__esp-hosted-tanmatsu/host/drivers/transport/sdio/sdio_drv.c"
if [ -f "$HOSTED_SDIO" ] && ! grep -q 'wadamesh: park instead of spinning' "$HOSTED_SDIO"; then
  python3 - "$HOSTED_SDIO" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
# Both failure arms look identical apart from the log line: event post, optional restart, continue.
old = """#if H_TRANSPORT_RESTART_ON_FAILURE
			ESP_LOGI(TAG, "Host is resetting itself, to avoid any sdio race condition");
			g_h.funcs->_h_restart_host();
#endif
			continue;"""
new = """#if H_TRANSPORT_RESTART_ON_FAILURE
			ESP_LOGI(TAG, "Host is resetting itself, to avoid any sdio race condition");
			g_h.funcs->_h_restart_host();
#else
			/* wadamesh: park instead of spinning. The transport is down and we are
			 * not allowed to reboot the host, so retrying can only fail again at
			 * full speed. The failure event is already posted; stop here and let the
			 * rest of the device run without Wi-Fi/BLE. */
			ESP_LOGE(TAG, "transport down, host restart disabled: parking rx task (Wi-Fi/BLE unavailable)");
			while (1) g_h.funcs->_h_msleep(60000);
#endif
			continue;"""
n = s.count(old)
if n == 0:
    sys.exit("esp-hosted sdio_drv.c: failure arm not found; patch needs updating")
s = s.replace(old, new)
open(p, "w").write(s)
print("  patched %d failure arm(s)" % n)
PYEOF
  echo "[build.sh] patched esp-hosted sdio_drv.c (park the rx task on a dead transport)"
fi

# ⚠️ The P4/slot-0 branch REBUILDS slot_config from scratch with `.flags = 0`, silently discarding
# the flag added above (that's the branch that actually runs with BOARD_SDMMC_SLOT=0): so patch that
# struct literal too. Without the flag the IDF host explicitly FLOATS the pads -> deterministic 0x109
# CRC at the first data read regardless of card/frequency.
if [ -f "$SDMMC_C" ] && ! grep -q 'wadamesh P4: slot0 pullups' "$SDMMC_C"; then
  sed -i '' 's|^    .flags = 0,$|    .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,  // wadamesh P4: slot0 pullups (struct rebuilt here; the earlier flag is discarded)|' "$SDMMC_C"
  echo "[build.sh] patched arduino SD_MMC.cpp (slot-0 struct pull-up flag)"
fi

# --- Build-time patch: the generic esp32p4 variant's SD power pin is THIS board's touch SDA ---------
# Arduino's esp32p4 variant (the Espressif P4 Function EV board) defines BOARD_SDMMC_POWER_PIN 45, so
# SD_MMC.begin() does pinMode(45, OUTPUT) + a 200 ms power toggle, leaves it driven LOW, and
# SD_MMC.end() clears the pin again. On the CrowPanel GPIO45 is I2C0 SDA (GT911, codec, HY2.0 port):
# the pinMode makes the peripheral manager detach I2C0 (i2cDeinit), so every Wire call afterwards
# failed with "bus is not initialized" and touch never came up (beta_97, tester V1.0 log). The card
# here is powered straight from VDD_3V3 (no switch), so the block has no job on this board at all.
# main/CMakeLists.txt defines WADA_SDMMC_NO_POWER_PIN; the guard below makes SD_MMC honour it. Fails
# the build if the upstream lines change, because touch depends on it. Idempotent.
if [ -f "$SDMMC_C" ] && ! grep -q 'WADA_SDMMC_NO_POWER_PIN' "$SDMMC_C"; then
  if [ "$(grep -c '^#if defined(BOARD_SDMMC_POWER_PIN)$' "$SDMMC_C")" -lt 2 ]; then
    echo "[build.sh] SD_MMC.cpp: BOARD_SDMMC_POWER_PIN blocks not found; the GPIO45 patch needs updating" >&2
    exit 1
  fi
  sed -i '' 's|^#if defined(BOARD_SDMMC_POWER_PIN)$|#if defined(BOARD_SDMMC_POWER_PIN) \&\& !defined(WADA_SDMMC_NO_POWER_PIN)  // wadamesh CrowPanel P4: GPIO45 is I2C SDA|' "$SDMMC_C"
  echo "[build.sh] patched arduino SD_MMC.cpp (no SD power pin: GPIO45 stays I2C SDA)"
fi

# --- Build-time patch: remember the C6's esp-hosted version from its init event ---------------------
# The slave reports its firmware version in the transport INIT event; the host only compares it
# (major.minor) and logs a warning. Keep the raw value so main.cpp can say plainly which C6
# firmware is fitted. Asking for it over RPC instead (esp_hosted_get_coprocessor_fwversion) is what
# an old co-processor may not answer. The variable is defined in variants/crowpanel_p4_101/target.cpp.
HOSTED_TDRV="managed_components/nicolaielectronics__esp-hosted-tanmatsu/host/drivers/transport/transport_drv.c"
if [ -f "$HOSTED_TDRV" ] && ! grep -q 'wadamesh_c6_fw_version' "$HOSTED_TDRV"; then
  python3 - "$HOSTED_TDRV" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "\t// if ESP_PRIV_FIRMWARE_VERSION was not received, slave version will be 0.0.0\n\tcompare_fw_version(slave_fw_version);\n"
new = ("\t// if ESP_PRIV_FIRMWARE_VERSION was not received, slave version will be 0.0.0\n"
       "\t{ extern volatile uint32_t wadamesh_c6_fw_version; extern volatile int wadamesh_c6_fw_known;\n"
       "\t  wadamesh_c6_fw_version = slave_fw_version; wadamesh_c6_fw_known = 1; }  /* wadamesh: C6 version for the boot log */\n"
       "\tcompare_fw_version(slave_fw_version);\n")
if s.count(old) != 1:
    sys.exit("esp-hosted transport_drv.c: version compare not found; patch needs updating")
open(p, "w").write(s.replace(old, new))
PYEOF
  echo "[build.sh] patched esp-hosted transport_drv.c (keep the C6 firmware version)"
fi

# --- Build-time patch: this board's C6 ships esp-hosted, so Arduino's hostedInit() must be the real
# one. A managed_components tree copied from a legacy ESP-AT T-Display build may carry the stub.
HOSTED_C="managed_components/espressif__arduino-esp32/cores/esp32/esp32-hal-hosted.c"
if [ -f "$HOSTED_C" ] && grep -q 'wadamesh P4: never start esp_hosted' "$HOSTED_C"; then
  sed -i '' 's|^static bool hostedInit() { return false; // wadamesh P4: never start esp_hosted.*$|static bool hostedInit() {|' "$HOSTED_C"
  echo "[build.sh] restored arduino hostedInit() (hosted C6 build)"
fi
BLEDEV_CPP="managed_components/espressif__arduino-esp32/libraries/BLE/src/BLEDevice.cpp"
if [ -f "$BLEDEV_CPP" ] && grep -q 'int rc = ble_gap_read_local_irk(irk);' "$BLEDEV_CPP"; then
  sed -i '' 's|int rc = ble_gap_read_local_irk(irk);|int rc = 0; (void)irk;  // wadamesh: arduino BLE unused; symbol renamed in IDF 5.5 NimBLE|' "$BLEDEV_CPP"
  echo "[build.sh] patched arduino BLEDevice.cpp (stub renamed ble_gap_read_local_irk)"
fi

# --- Build-time patch: neutralize esp-hosted's auto-init constructor (P4-ONLY) --------------------
# Initialization must not run before app_main: the C6 is brought up lazily through Arduino
# hostedInit() after the UI, so a C6 that does not answer can only cost Wi-Fi/BLE.
# Two copies get pulled in (espressif__esp_hosted via arduino-esp32, and the nicolaielectronics__
# esp-hosted-tanmatsu fork via tanmatsu-wifi/badge-bsp). The build actually links the fork, so patch
# BOTH so whichever is linked never grabs the SDIO.
for HDIR in espressif__esp_hosted nicolaielectronics__esp-hosted-tanmatsu; do
  HINIT_C="managed_components/$HDIR/host/port/esp/freertos/src/port_esp_hosted_host_init.c"
  if [ -f "$HINIT_C" ] && grep -q 'ESP_ERROR_CHECK(esp_hosted_init());' "$HINIT_C"; then
    sed -i '' 's|ESP_ERROR_CHECK(esp_hosted_init());|/* wadamesh P4: C6 initialized lazily via hostedInit() after the UI */|' "$HINIT_C"
    echo "[build.sh] neutralized $HDIR auto-init constructor (defer until board power)"
  fi
done

if [ "$#" -eq 1 ] && [ "$1" = "merge" ]; then
  idf.py "${IDF_ARGS[@]}" build
  OUT="$P4_BUILD_DIR/wadamesh-crowpanel-p4-101-v${CP_REV/./}-merged.bin"
  # Same offsets and flash settings as scripts/build/p4-release.sh (bootloader 0x2000 on the P4).
  python -m esptool --chip esp32p4 merge_bin --flash_mode dio --flash_size 16MB --flash_freq 80m \
    -o "$OUT" \
    0x2000 "$P4_BUILD_DIR/bootloader/bootloader.bin" \
    0x8000 "$P4_BUILD_DIR/partition_table/partition-table.bin" \
    0xf000 "$P4_BUILD_DIR/ota_data_initial.bin" \
    0x20000 "$P4_BUILD_DIR/application.bin"
  echo "[build.sh] merged image: $OUT (flash at 0x0)"
  exit 0
fi

exec idf.py "${IDF_ARGS[@]}" "$@"
