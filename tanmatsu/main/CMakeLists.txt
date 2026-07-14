idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.cpp"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
		espressif__arduino-esp32
		meshcore
		lvgl
		wadamesh_app
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
# The vendored MeshCore core + Arduino libs build clean under PlatformIO's laxer flags but trip
# IDF's blanket -Werror=all (reorder/format/class-memaccess) as ERRORS. A bare -Wno-error does
# NOT cancel -Werror=all, so strip it from the global options outright — warnings stay visible,
# just non-fatal. (Our own new code stays warning-clean by convention.)
idf_build_get_property(_co COMPILE_OPTIONS)
list(REMOVE_ITEM _co "-Werror=all")
idf_build_set_property(COMPILE_OPTIONS "${_co}")

# --- Tanmatsu app define set: curated subset of the S3 touch build_flags (see TANMATSU_PORT.md).
# Applied globally so the core + app + UITask all see a consistent config. No S3-board defines
# (HELTEC_*, ST7789_*, P_LORA_*, RADIOLIB_*) — the Tanmatsu uses badge-bsp + the LoRa bridge.
set(WADA_DEFS
  HAS_TANMATSU=1 ESP32_PLATFORM MC_VENDORED_TOUCH_APP UI_LVGL=1 HAS_TOUCH_UI=1
  MULTI_TRANSPORT_COMPANION TCP_PORT=5000 WS_PORT=8765
  BLE_PIN_CODE=123456                       # enables the app's NimBLE companion (BLE over the C6)
  LORA_FREQ=869.618 LORA_BW=62.5 LORA_SF=8
  MAX_CONTACTS=2000 MAX_GROUP_CHANNELS=40  # P4 has ample RAM; contacts array is PSRAM-resident (the_mesh placement-new)
  ENABLE_PRIVATE_KEY_IMPORT=1 ENABLE_PRIVATE_KEY_EXPORT=1
  DISPLAY_CLASS=TanmatsuDisplay
  # microSD lives on the P4's SDMMC *slot 0* (IOMUX pins CLK43/CMD44/D0-3 39-42); slot 1 is the
  # C6 radio (esp-hosted SDIO), so these make Arduino SD_MMC target slot 0 only — no radio conflict.
  BOARD_HAS_SDMMC=1 BOARD_SDMMC_SLOT=0
  # the core's ESP32Board.h reads these raw pins directly; Tanmatsu uses badge-bsp, so -1 (none):
  P_LORA_DIO_1=-1 P_LORA_TX_LED=-1 P_LORA_TX_NEOPIXEL_LED=-1
  PIN_BOARD_SCL=-1 PIN_BOARD_SDA=-1 PIN_VBAT_READ=-1)
foreach(_d ${WADA_DEFS})
  idf_build_set_property(COMPILE_OPTIONS "-D${_d}" APPEND)
endforeach()

# Firmware version shown on the About page + parsed by the update check. build.sh passes
# WADA_FW_TAG/-DATE from git describe + date, so dev builds show the real state
# ("beta_28-14-gabc123") and a release build on the beta_N tag bakes exactly "beta_N" —
# the old hardcoded define left every dev flash claiming the last hand-bumped beta.
# firmwareReleaseN() parses the leading "beta_%d", so the -dev suffix keeps the update
# check on the right baseline. Quotes escaped so the compiler sees string literals.
if(NOT WADA_FW_TAG)
  set(WADA_FW_TAG "dev")
endif()
if(NOT WADA_FW_DATE)
  set(WADA_FW_DATE "")
endif()
idf_build_set_property(COMPILE_OPTIONS "-DFIRMWARE_RELEASE_TAG=\"${WADA_FW_TAG}\"" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DFIRMWARE_BUILD_DATE=\"${WADA_FW_DATE}\"" APPEND)
# The About page shows FIRMWARE_VERSION (not the tag). The S3 release.sh sets it to
# "wadamesh <tag>"; without this the P4 build fell back to the MeshCore default
# ("v1.16.0-touch") and the Tanmatsu never showed its beta number. Keep them in sync.
idf_build_set_property(COMPILE_OPTIONS "-DFIRMWARE_VERSION=\"wadamesh ${WADA_FW_TAG}\"" APPEND)
# (the adcAttachPin compat shim is force-included per-component in meshcore + wadamesh_app —
#  NOT globally, which would feed the C++ header to FreeRTOS's .S assembly files and break them.)
