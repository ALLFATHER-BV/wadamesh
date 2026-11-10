#!/usr/bin/env bash
# Reusable wadamesh/Tanmatsu build wrapper: activates the project-local ESP-IDF 5.5.1
# and runs idf.py with our device/target/sdkconfig params.
#   ./build.sh build        # compile the AppFS app (launcher / app store)
#   ./build.sh standalone   # + bootloader + standalone table -> build/tanmatsu/wadamesh-tanmatsu-merged.bin
#   ./build.sh standalone-flash [-p /dev/cu.usbmodemXXXX] [--erase]
#                           # write that merged image at 0x0. REPLACES the launcher.
#                           # Without -p, tries each USB serial port (P4, then C6).
#   ./build.sh menuconfig
#
# Never `./build.sh flash`: idf.py writes the launcher's 16M.csv table plus the app
# into a 2 MB OTA slot it does not fit. Use tools/tan_flash.sh (AppFS) or standalone-flash.
set -e

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
cd "$(dirname "$0")"
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

# Firmware tag shown on About + used by the update check. Derived from git so dev
# builds are honest ("beta_28-14-gabc123" = 14 commits past beta_28) and a release
# build on a fresh beta_N tag bakes exactly "beta_N".
WADA_FW_TAG="${WADA_FW_TAG:-$(git describe --tags --match 'beta_*' --always 2>/dev/null || echo dev)}"
WADA_FW_DATE="$(date '+%-d %b %Y')"
IDF_ARGS=(-B build/tanmatsu \
  -DDEVICE=tanmatsu \
  -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu;sdkconfigs/wadamesh;sdkconfigs/wadamesh_tanmatsu" \
  -DWADA_FW_TAG="$WADA_FW_TAG" -DWADA_FW_DATE="$WADA_FW_DATE" \
  -DIDF_TARGET=esp32p4)

# An existing sdkconfig keeps its old values: SDKCONFIG_DEFAULTS only apply when it is
# generated. If a setting from the Tanmatsu overlay is missing, regenerate it from the
# defaults (what a fresh clone gets), or a release built here would miss the size savings
# that keep application.bin under the launcher's 50-page limit.
if [ -f sdkconfig ]; then
  while IFS= read -r line; do
    case "$line" in CONFIG_*=n) want="# ${line%=n} is not set" ;; CONFIG_*=*) want="$line" ;; *) continue ;; esac
    if ! grep -qxF "$want" sdkconfig; then
      echo "[build.sh] sdkconfig lacks '$want': regenerating it from sdkconfigs/"
      rm -f sdkconfig
      break
    fi
  done < sdkconfigs/wadamesh_tanmatsu
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

# --- Build-time patch: arduino-esp32 esp-hosted WiFi init (Tanmatsu) ------------------------------
# esp_hosted is force-initialised by an unconditional constructor in the esp_hosted component BEFORE
# app_main, so arduino's WiFi.mode -> hostedInit() -> esp_hosted_sdio_set_config() returns
# ESP_ERR_NOT_ALLOWED ("already configured"). This arduino build treats that as fatal and never runs
# esp_wifi_init(), leaving WiFi at WIFI_NOT_INIT. Apply the fix the arduino source itself flags
# ("uncomment when second init is fixed"): also accept ESP_ERR_NOT_ALLOWED. Idempotent + re-applies
# after a dependency re-fetch. This patches a BUILD DEPENDENCY on this machine only — it compiles into
# our app .bin and never touches the device's own firmware.
HOSTED_C="managed_components/espressif__arduino-esp32/cores/esp32/esp32-hal-hosted.c"
if [ -f "$HOSTED_C" ] && grep -q 'if (err != ESP_OK) {  *//&& err != ESP_ERR_NOT_ALLOWED' "$HOSTED_C"; then
  sed -i '' 's|if (err != ESP_OK) {  *//&& err != ESP_ERR_NOT_ALLOWED.*|if (err != ESP_OK \&\& err != ESP_ERR_NOT_ALLOWED) {  // wadamesh: tolerate esp_hosted pre-init|' "$HOSTED_C"
  echo "[build.sh] patched arduino esp32-hal-hosted.c (esp-hosted NOT_ALLOWED tolerance for WiFi)"
fi

# --- Build-time patch: arduino-esp32 bundled BLE lib vs IDF 5.5 NimBLE ----------------------------
# With BLE enabled (NimBLE), arduino-esp32's libraries/BLE compiles its NimBLE path, which calls
# ble_gap_read_local_irk() — renamed/removed in IDF 5.5.1's NimBLE. We don't use arduino's BLE (the
# companion uses esp-nimble-cpp's SerialBLEInterface), and getLocalIRK() is never called, so stub the
# call so the file compiles. Idempotent.
BLEDEV_CPP="managed_components/espressif__arduino-esp32/libraries/BLE/src/BLEDevice.cpp"
if [ -f "$BLEDEV_CPP" ] && grep -q 'int rc = ble_gap_read_local_irk(irk);' "$BLEDEV_CPP"; then
  sed -i '' 's|int rc = ble_gap_read_local_irk(irk);|int rc = 0; (void)irk;  // wadamesh: arduino BLE unused; symbol renamed in IDF 5.5 NimBLE|' "$BLEDEV_CPP"
  echo "[build.sh] patched arduino BLEDevice.cpp (stub renamed ble_gap_read_local_irk)"
fi

# Tanmatsu ships application.bin inside the 8 MB AppFS partition, not either
# 2 MB OTA slot in the device partition table. Build the image target directly
# so the full compile/link runs without IDF's irrelevant OTA partition check.
if [ "$#" -eq 1 ] && [ "$1" = build ]; then
  exec idf.py "${IDF_ARGS[@]}" gen_project_binary
fi

# Standalone (web flasher) image: the SAME application.bin as the AppFS build, booted
# by the stock IDF bootloader from partition_tables/wadamesh_standalone.csv instead of
# by the launcher. The app finds its data by partition label ('locfd') and the table
# is at 0x8000 either way, so nothing in the app changes between the two.
STANDALONE_BIN="build/tanmatsu/wadamesh-tanmatsu-merged.bin"
# Port the last standalone-flash succeeded on (the P4), for build-upload-monitor.sh.
STANDALONE_PORT_FILE="build/tanmatsu/standalone/port"
build_standalone() {
  local B=build/tanmatsu PT=build/tanmatsu/standalone/partition-table.bin
  idf.py "${IDF_ARGS[@]}" gen_project_binary bootloader
  mkdir -p "$B/standalone"
  python "$IDF_PATH/components/partition_table/gen_esp32part.py" --quiet --flash-size 16MB \
    partition_tables/wadamesh_standalone.csv "$PT"
  python "$IDF_PATH/components/partition_table/check_sizes.py" --offset 0x8000 \
    partition --type app "$PT" "$B/application.bin"
  # No otadata/nvs parts: merge_bin pads the 0x9000-0x20000 gap with 0xFF, so the
  # launcher's otadata and NVS are erased and the bootloader starts ota_0. 'locfd'
  # lies past the end of the image and is left alone.
  python -m esptool --chip esp32p4 merge_bin --flash_mode dio --flash_size 16MB --flash_freq 80m \
    -o "$STANDALONE_BIN" \
    0x2000 "$B/bootloader/bootloader.bin" \
    0x8000 "$PT" \
    0x20000 "$B/application.bin" >/dev/null
  echo "[build.sh] standalone image: tanmatsu/$STANDALONE_BIN ($(wc -c < "$STANDALONE_BIN" | tr -d ' ') bytes)"
}

case "${1:-}" in
  standalone)
    [ "$#" -eq 1 ] || { echo "usage: ./build.sh standalone" >&2; exit 2; }
    build_standalone
    exit 0 ;;
  standalone-flash)
    shift
    port=""; erase=0
    while [ "$#" -gt 0 ]; do
      case "$1" in
        -p|--port) port="${2:-}"; shift ;;
        --erase)   erase=1 ;;
        *) echo "usage: ./build.sh standalone-flash [-p PORT] [--erase]" >&2; exit 2 ;;
      esac
      shift
    done
    # The Tanmatsu shows two USB serial ports, the ESP32-P4 and its ESP32-C6 radio, and
    # their names swap between replugs. Trying the C6 is harmless: with --chip esp32p4,
    # esptool aborts on the chip mismatch before writing. So without -p, try each port
    # and keep the first that works (recorded in $STANDALONE_PORT_FILE for monitoring).
    ports=()
    if [ -n "$port" ]; then
      ports=("$port")
    else
      for p in /dev/cu.usbmodem* /dev/ttyACM*; do [ -e "$p" ] && ports+=("$p"); done
      [ "${#ports[@]}" -gt 0 ] || { echo "[build.sh] no USB serial ports found. Is the Tanmatsu connected?" >&2; exit 1; }
    fi
    # Build once up front so a compile error fails once, not once per port.
    build_standalone
    rm -f "$STANDALONE_PORT_FILE"
    for p in "${ports[@]}"; do
      echo "[build.sh] flashing the standalone image to $p: this REPLACES the badge.team launcher"
      if { [ "$erase" = 0 ] || python -m esptool --chip esp32p4 -p "$p" erase_flash; } &&
         python -m esptool --chip esp32p4 -p "$p" write_flash 0x0 "$STANDALONE_BIN"; then
        echo "$p" > "$STANDALONE_PORT_FILE"
        exit 0
      fi
      echo "[build.sh] flashing failed on $p." >&2
    done
    echo "[build.sh] flashing failed on every port: ${ports[*]}" >&2
    exit 1 ;;
esac

exec idf.py "${IDF_ARGS[@]}" "$@"
