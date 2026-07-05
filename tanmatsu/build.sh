#!/usr/bin/env bash
# Reusable wadamesh/Tanmatsu build wrapper: activates the project-local ESP-IDF 5.5.1
# and runs idf.py with our device/target/sdkconfig params.
#   ./build.sh build        # compile
#   ./build.sh flash -p ... # flash over USB (dev)
#   ./build.sh menuconfig
set -e
cd "$(dirname "$0")"
export IDF_TOOLS_PATH="$PWD/esp-idf-tools"
# shellcheck disable=SC1091
source esp-idf/export.sh >/dev/null 2>&1

# --- Build-time patch: arduino-esp32 esp-hosted WiFi init (Tanmatsu) ------------------------------
# esp_hosted is force-initialised by an unconditional constructor in the esp_hosted component BEFORE
# app_main, so arduino's WiFi.mode -> hostedInit() -> esp_hosted_sdio_set_config() returns
# ESP_ERR_NOT_ALLOWED ("already configured"). This arduino build treats that as fatal and never runs
# esp_wifi_init(), leaving WiFi at WIFI_NOT_INIT. Apply the fix the arduino source itself flags
# ("uncomment when second init is fixed"): also accept ESP_ERR_NOT_ALLOWED. Idempotent + re-applies
# after a dependency re-fetch. This patches a BUILD DEPENDENCY on this machine only — it compiles into
# our app .bin and never touches the device's own firmware. (On a brand-new checkout managed_components
# doesn't exist yet, so the very first build ships unpatched; a second build picks it up.)
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

# Firmware tag shown on About + used by the update check. Derived from git so dev
# builds are honest ("beta_28-14-gabc123" = 14 commits past beta_28) and a release
# build on a fresh beta_N tag bakes exactly "beta_N" — no more hand-bumping the
# old hardcoded CMake define (which left every dev flash claiming the last beta).
# Passed as a CMake cache var: idf.py reconfigures automatically when it changes.
WADA_FW_TAG="$(git describe --tags --match 'beta_*' --always 2>/dev/null || echo dev)"
WADA_FW_DATE="$(date '+%-d %b %Y')"

exec idf.py -B build/tanmatsu \
  -DDEVICE=tanmatsu \
  -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu;sdkconfigs/wadamesh" \
  -DWADA_FW_TAG="$WADA_FW_TAG" -DWADA_FW_DATE="$WADA_FW_DATE" \
  -DIDF_TARGET=esp32p4 "$@"
