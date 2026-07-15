#!/usr/bin/env bash
# wadamesh / LilyGo T-Display P4 (AMOLED) build wrapper. Standalone ESP-IDF app
# (NOT a launcher/AppFS app like the Tanmatsu) — flashes directly over USB.
# Reuses the Tanmatsu's project-local ESP-IDF 5.5.1 (esp32p4) via the ../tanmatsu
# symlinks, and the board-neutral components (meshcore/ardlibs/lvgl/esp_hosted).
#   ./build.sh build          # compile
#   ./build.sh flash -p /dev/cu.usbmodemXXXX
#   ./build.sh menuconfig
set -e
cd "$(dirname "$0")"
export IDF_TOOLS_PATH="$PWD/esp-idf-tools"
# shellcheck disable=SC1091
source esp-idf/export.sh >/dev/null 2>&1

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

# --- Build-time patch: arduino-esp32 must NEVER start esp-hosted on the P4 ------------------------
# The T-Display P4's C6 runs ESP-AT (driven by the c6_at component over SDIO). If ANY code path
# reaches Arduino's real WiFi (i.e. not rebound to the C6WifiShim facade), its hostedInit() would run
# esp_hosted_init() and grab the C6 SDIO out from under c6_at -> panic ("connect Wi-Fi blue screen").
# Hard-stub hostedInit() to fail fast instead. Patches only the P4's own managed copy. Idempotent.
HOSTED_C="managed_components/espressif__arduino-esp32/cores/esp32/esp32-hal-hosted.c"
if [ -f "$HOSTED_C" ] && ! grep -q 'wadamesh P4: never start esp_hosted' "$HOSTED_C"; then
  sed -i '' 's|^static bool hostedInit() {|static bool hostedInit() { return false; // wadamesh P4: never start esp_hosted (C6 runs ESP-AT; see components/c6_at)|' "$HOSTED_C"
  echo "[build.sh] hard-stubbed arduino hostedInit() (P4 uses c6_at, never esp-hosted)"
fi
BLEDEV_CPP="managed_components/espressif__arduino-esp32/libraries/BLE/src/BLEDevice.cpp"
if [ -f "$BLEDEV_CPP" ] && grep -q 'int rc = ble_gap_read_local_irk(irk);' "$BLEDEV_CPP"; then
  sed -i '' 's|int rc = ble_gap_read_local_irk(irk);|int rc = 0; (void)irk;  // wadamesh: arduino BLE unused; symbol renamed in IDF 5.5 NimBLE|' "$BLEDEV_CPP"
  echo "[build.sh] patched arduino BLEDevice.cpp (stub renamed ble_gap_read_local_irk)"
fi

# --- Build-time patch: neutralize esp-hosted's auto-init constructor (P4-ONLY) --------------------
# The T-Display P4's on-board C6 runs ESP-AT firmware, which we drive over SDIO via the c6_at
# component (ESSL). esp-hosted (pulled in transitively by arduino-esp32) force-initialises itself in
# an unconditional __attribute__((constructor)) before app_main and grabs the same SDIO — its rx task
# then steals the interrupts/tokens our ESSL send needs, so essl_send_packet fails. There is no
# Kconfig to disable it, so stub the constructor's init call. This patches ONLY the P4's own copy of
# the managed component (tdisplay_p4/managed_components); the Tanmatsu's copy is untouched, so its
# genuine esp-hosted C6 still inits normally.
# Two copies get pulled in (espressif__esp_hosted via arduino-esp32, and the nicolaielectronics__
# esp-hosted-tanmatsu fork via tanmatsu-wifi/badge-bsp). The build actually links the fork, so patch
# BOTH so whichever is linked never grabs the SDIO.
for HDIR in espressif__esp_hosted nicolaielectronics__esp-hosted-tanmatsu; do
  HINIT_C="managed_components/$HDIR/host/port/esp/freertos/src/port_esp_hosted_host_init.c"
  if [ -f "$HINIT_C" ] && grep -q 'ESP_ERROR_CHECK(esp_hosted_init());' "$HINIT_C"; then
    sed -i '' 's|ESP_ERROR_CHECK(esp_hosted_init());|/* wadamesh P4: C6 runs ESP-AT (driven by c6_at over SDIO) — skip esp-hosted auto-init */|' "$HINIT_C"
    echo "[build.sh] neutralized $HDIR auto-init constructor (P4 uses AT, not esp-hosted)"
  fi
done

WADA_FW_TAG="${WADA_FW_TAG:-$(git describe --tags --match 'beta_*' --always 2>/dev/null || echo dev)}"
WADA_FW_DATE="$(date '+%-d %b %Y')"

exec idf.py -B build/tdisplay_p4 \
  -DDEVICE=tdisplay_p4 \
  -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tdisplay_p4;sdkconfigs/wadamesh" \
  -DWADA_FW_TAG="$WADA_FW_TAG" -DWADA_FW_DATE="$WADA_FW_DATE" \
  -DIDF_TARGET=esp32p4 "$@"
