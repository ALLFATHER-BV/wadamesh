#!/usr/bin/env bash
# wadamesh / LilyGo T-Display P4 (AMOLED) build wrapper. Standalone ESP-IDF app
# (NOT a launcher/AppFS app like the Tanmatsu) — flashes directly over USB.
# Reuses the Tanmatsu's project-local ESP-IDF 5.5.1 (esp32p4) via the ../tanmatsu
# symlinks, and the board-neutral components (meshcore/ardlibs/lvgl/esp_hosted).
#   ./build.sh build          # compile
#   ./build.sh flash -p /dev/cu.usbmodemXXXX
#   ./build.sh fullclean      # remove generated build output; keep patched managed components
#   ./build.sh menuconfig
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

# This project intentionally applies compatibility fixes inside managed_components
# before every build. ESP-IDF's stock fullclean also runs remove_managed_components,
# which rejects those expected hash changes and aborts. A P4 full clean therefore
# removes only the generated CMake/Ninja tree; dependencies stay in place and the
# idempotent patches below remain valid.
if [ "$#" -eq 1 ] && [ "$1" = "fullclean" ]; then
  cmake -E remove_directory "$PROJECT_DIR/build/tdisplay_p4"
  echo "[build.sh] removed build/tdisplay_p4 (patched managed components preserved)"
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
IDF_ARGS=(-B build/tdisplay_p4 \
  -DDEVICE=tdisplay_p4 \
  -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/wadamesh;sdkconfigs/tdisplay_p4" \
  -DWADA_FW_TAG="$WADA_FW_TAG" -DWADA_FW_DATE="$WADA_FW_DATE" \
  -DIDF_TARGET=esp32p4)

# Existing checkouts may carry a generated sdkconfig from the old host-only
# UART-HCI setup. sdkconfig.defaults does not override an already-saved value,
# so migrate it before CMake runs. Hosted supplies NimBLE's HCI transport over
# SDIO; GPIO4/5 UART HCI has no controller attached and ends in hci_h4 asserts.
if [ -z "${WADA_P4_LEGACY_AT:-}" ] && [ -f sdkconfig ] &&
   grep -q '^CONFIG_BT_NIMBLE_TRANSPORT_UART=y$' sdkconfig; then
  sed -i '' 's/^CONFIG_BT_NIMBLE_TRANSPORT_UART=y$/# CONFIG_BT_NIMBLE_TRANSPORT_UART is not set/' sdkconfig
  echo "[build.sh] disabled stale NimBLE UART HCI (P4 uses hosted HCI over SDIO)"
fi

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

# --- Build-time patch: SD_MMC internal pull-ups (T-Display P4) ------------------------------------
# The board has no external pull-ups on the SD data lines; the IDF sdmmc host explicitly FLOATS the
# pads unless SDMMC_SLOT_FLAG_INTERNAL_PULLUP is set (so pre-begin gpio_pullup_en gets undone).
# Meck-P4's working SD init sets the flag; Arduino's SD_MMC never does and has no API for it. Patch
# the P4's own managed copy. Idempotent.
SDMMC_C="managed_components/espressif__arduino-esp32/libraries/SD_MMC/src/SD_MMC.cpp"
if [ -f "$SDMMC_C" ] && ! grep -q 'wadamesh P4: internal pullups' "$SDMMC_C"; then
  sed -i '' 's|sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();|sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();\n  slot_config.flags \|= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // wadamesh P4: internal pullups (no external ones on the SD lines)|' "$SDMMC_C"
  echo "[build.sh] patched arduino SD_MMC.cpp (internal pull-ups on the SD slot)"
fi
# ⚠️ The P4/slot-0 branch REBUILDS slot_config from scratch with `.flags = 0`, silently discarding
# the flag added above (that's the branch that actually runs with BOARD_SDMMC_SLOT=0) — so patch that
# struct literal too. Without the flag the IDF host explicitly FLOATS the pads -> deterministic 0x109
# CRC at the first data read regardless of card/frequency.
if [ -f "$SDMMC_C" ] && ! grep -q 'wadamesh P4: slot0 pullups' "$SDMMC_C"; then
  sed -i '' 's|^    .flags = 0,$|    .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,  // wadamesh P4: slot0 pullups (struct rebuilt here; the earlier flag is discarded)|' "$SDMMC_C"
  echo "[build.sh] patched arduino SD_MMC.cpp (slot-0 struct pull-up flag)"
fi

# --- Build-time patch: select Arduino's hosted entry point per C6 firmware generation ------------
# Current P4 units ship hosted C6 firmware; old units can opt into ESP-AT with
# WADA_P4_LEGACY_AT=1. Restore hostedInit normally and hard-stub it only for legacy builds.
HOSTED_C="managed_components/espressif__arduino-esp32/cores/esp32/esp32-hal-hosted.c"
if [ -f "$HOSTED_C" ]; then
  if [ -n "${WADA_P4_LEGACY_AT:-}" ]; then
    if ! grep -q 'wadamesh P4: never start esp_hosted' "$HOSTED_C"; then
      sed -i '' 's|^static bool hostedInit() {|static bool hostedInit() { return false; // wadamesh P4: never start esp_hosted (legacy C6 uses ESP-AT)|' "$HOSTED_C"
      echo "[build.sh] hard-stubbed arduino hostedInit() (legacy ESP-AT build)"
    fi
  elif grep -q 'wadamesh P4: never start esp_hosted' "$HOSTED_C"; then
    sed -i '' 's|^static bool hostedInit() { return false; // wadamesh P4: never start esp_hosted.*$|static bool hostedInit() {|' "$HOSTED_C"
    echo "[build.sh] restored arduino hostedInit() (hosted C6 build)"
  fi
fi
BLEDEV_CPP="managed_components/espressif__arduino-esp32/libraries/BLE/src/BLEDevice.cpp"
if [ -f "$BLEDEV_CPP" ] && grep -q 'int rc = ble_gap_read_local_irk(irk);' "$BLEDEV_CPP"; then
  sed -i '' 's|int rc = ble_gap_read_local_irk(irk);|int rc = 0; (void)irk;  // wadamesh: arduino BLE unused; symbol renamed in IDF 5.5 NimBLE|' "$BLEDEV_CPP"
  echo "[build.sh] patched arduino BLEDevice.cpp (stub renamed ble_gap_read_local_irk)"
fi

# --- Build-time patch: neutralize esp-hosted's auto-init constructor (P4-ONLY) --------------------
# Initialization must happen after XL9535 power/reset is available. Stub the pre-app_main
# constructor for both backends: hosted builds initialize lazily through Arduino hostedInit(),
# while legacy AT builds reserve slot 1 for c6_at.
# Two copies get pulled in (espressif__esp_hosted via arduino-esp32, and the nicolaielectronics__
# esp-hosted-tanmatsu fork via tanmatsu-wifi/badge-bsp). The build actually links the fork, so patch
# BOTH so whichever is linked never grabs the SDIO.
for HDIR in espressif__esp_hosted nicolaielectronics__esp-hosted-tanmatsu; do
  HINIT_C="managed_components/$HDIR/host/port/esp/freertos/src/port_esp_hosted_host_init.c"
  if [ -f "$HINIT_C" ] && grep -q 'ESP_ERROR_CHECK(esp_hosted_init());' "$HINIT_C"; then
    sed -i '' 's|ESP_ERROR_CHECK(esp_hosted_init());|/* wadamesh P4: initialize C6 only after XL9535 board power */|' "$HINIT_C"
    echo "[build.sh] neutralized $HDIR auto-init constructor (defer until board power)"
  fi
done

exec idf.py "${IDF_ARGS[@]}" "$@"
