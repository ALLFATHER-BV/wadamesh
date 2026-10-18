idf_build_get_property(idf_target IDF_TARGET)

idf_component_register(
	SRCS
		"main.cpp"
	PRIV_REQUIRES
		esp_lcd            # MIPI-DSI (EK79007)
		esp_driver_i2c
		esp_driver_gpio
		fatfs              # SD_MMC storage
		nvs_flash
		bt                 # NimBLE companion (BLE over the C6)
		espressif__arduino-esp32
		esp_hosted         # ESP32-C6 Wi-Fi/BLE over SDIO slot 1
		meshcore
		lvgl
		RadioLib           # target.h (included by main.cpp) pulls <RadioLib.h>
		wadamesh_app
	INCLUDE_DIRS
		"."
)

# Same warning policy as tdisplay_p4/main: vendored code must not fail the build on warnings.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-overflow" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=array-bounds" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=restrict" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)
idf_build_get_property(_co COMPILE_OPTIONS)
list(REMOVE_ITEM _co "-Werror=all")
idf_build_set_property(COMPILE_OPTIONS "${_co}")

# ---- Elecrow CrowPanel Advanced 10.1" ESP32-P4 (EXPERIMENTAL) app define set. -------------------
# Pins: Elecrow schematics V1.0/V1.1/V1.2 + their ESP-IDF lessons; per-pin status and sources in
# the pin table at the top of variants/crowpanel_p4_101/target.h.
#
# Board revision (build.sh WADA_CP_REV=1.0|1.1|1.2, default 1.2). Only two things differ:
#   - the SX1262 DIO1/NRST pair (J9.1 / J11.1): GPIO53/54 on V1.0 + V1.1, GPIO27/28 on V1.2;
#   - the C6 SDIO data-pin order (sdkconfig fragment, build.sh).
if(NOT DEFINED ENV{WADA_CP_REV} OR "$ENV{WADA_CP_REV}" STREQUAL "1.2")
  set(CP_REV_DEFS CP_REV=12 P_LORA_DIO_1=27 P_LORA_RESET=28)
  message(STATUS "wadamesh: CrowPanel P4 10.1 board revision V1.2")
elseif("$ENV{WADA_CP_REV}" STREQUAL "1.1")
  set(CP_REV_DEFS CP_REV=11 P_LORA_DIO_1=53 P_LORA_RESET=54)
  message(STATUS "wadamesh: CrowPanel P4 10.1 board revision V1.1")
elseif("$ENV{WADA_CP_REV}" STREQUAL "1.0")
  set(CP_REV_DEFS CP_REV=10 P_LORA_DIO_1=53 P_LORA_RESET=54)
  message(STATUS "wadamesh: CrowPanel P4 10.1 board revision V1.0")
else()
  message(FATAL_ERROR "WADA_CP_REV must be 1.0, 1.1 or 1.2 (got '$ENV{WADA_CP_REV}')")
endif()

set(WADA_DEFS
  HAS_CROWPANEL_P4_101=1 ESP32_PLATFORM MC_VENDORED_TOUCH_APP UI_LVGL=1 HAS_TOUCH_UI=1
  ${CP_REV_DEFS}
  USE_SX1262 RADIO_CLASS=CustomSX1262 WRAPPER_CLASS=CustomSX1262Wrapper
  SX126X_RX_BOOSTED_GAIN=1 SX126X_CURRENT_LIMIT=140
  SX126X_DIO2_AS_RF_SWITCH=true SX126X_DIO3_TCXO_VOLTAGE=1.6f   # Elecrow Lesson14: RadioLib defaults + 1.6 V TCXO
  MULTI_TRANSPORT_COMPANION TCP_PORT=5000 WS_PORT=8765 BLE_PIN_CODE=123456
  LORA_FREQ=869.618 LORA_BW=62.5 LORA_SF=8            # EU 868 default like the other P4 boards; region is a setting
  LORA_TX_POWER=22 MAX_LORA_TX_POWER=22
  MAX_CONTACTS=2000 MAX_GROUP_CHANNELS=40             # 32 MB PSRAM
  ENABLE_PRIVATE_KEY_IMPORT=1 ENABLE_PRIVATE_KEY_EXPORT=1

  # --- SX1262 on the external wireless header J9/J11 (SPI3). DIO1/NRST from the revision block. ---
  P_LORA_NSS=10 P_LORA_BUSY=9 P_LORA_SCLK=8 P_LORA_MISO=7 P_LORA_MOSI=6

  # --- Display: EK79007 1024x600 MIPI-DSI, native landscape, LVGL at native resolution. ---
  DISPLAY_CLASS=EK79007Display SCREEN_WIDTH=1024 SCREEN_HEIGHT=600

  # --- Touch GT911 + shared board I2C0 (codec, HY2.0 port). Wire is begun by ESP32Board::begin. ---
  PIN_BOARD_SDA=45 PIN_BOARD_SCL=46 PIN_TOUCH_SDA=45 PIN_TOUCH_SCL=46 PIN_TOUCH_RST=40 PIN_TOUCH_INT=42

  # --- microSD: SDMMC slot 0, 1-bit (only D0 is wired): CLK43 / CMD44 / D0 39. C6 is slot 1. ---
  BOARD_HAS_SDMMC=1 BOARD_SDMMC_SLOT=0
  # The generic esp32p4 Arduino variant names GPIO45 as an SD power switch (BOARD_SDMMC_POWER_PIN);
  # here 45 is the touch/codec I2C SDA and the card has no power switch. build.sh makes SD_MMC.cpp
  # skip that pin when this is defined; without it every SD_MMC.begin()/end() tore down I2C0.
  WADA_SDMMC_NO_POWER_PIN=1
)
foreach(_d ${WADA_DEFS})
  idf_build_set_property(COMPILE_OPTIONS "-D${_d}" APPEND)
endforeach()

# Firmware version/tag (build.sh passes WADA_FW_TAG/-DATE from git describe + date).
if(NOT WADA_FW_TAG)
  set(WADA_FW_TAG "dev")
endif()
if(NOT WADA_FW_DATE)
  set(WADA_FW_DATE "")
endif()
idf_build_set_property(COMPILE_OPTIONS "-DFIRMWARE_RELEASE_TAG=\"${WADA_FW_TAG}\"" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DFIRMWARE_BUILD_DATE=\"${WADA_FW_DATE}\"" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DFIRMWARE_VERSION=\"wadamesh ${WADA_FW_TAG}\"" APPEND)
# NOTE: no FIRMWARE_OTA_ENV on purpose. This is an experimental board with no release artifact on
# firmware.wadamesh.com yet, so the on-device update UI stays hidden (CAP_OTA 0 in device_caps.h).
