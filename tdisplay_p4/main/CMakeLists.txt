idf_build_get_property(idf_target IDF_TARGET)

idf_component_register(
	SRCS
		"main.cpp"
	PRIV_REQUIRES
		esp_lcd            # MIPI-DSI (RM69A10 AMOLED)
		esp_driver_i2c     # XL9535 expander + HI8561 touch + RTC/gauge
		esp_driver_gpio
		fatfs              # SD_MMC storage
		nvs_flash
		bt                 # NimBLE companion (BLE over the C6)
		espressif__arduino-esp32
		esp_hosted         # ESP32-C6 Wi-Fi/BLE over SDIO_2
		meshcore
		lvgl
		RadioLib           # target.h (included by main.cpp) pulls <RadioLib.h>
		wadamesh_app
		c6_at              # AT-over-SDIO Wi-Fi/BLE driver for the on-board ESP32-C6 (P4-only)
	INCLUDE_DIRS
		"."
)

# The vendored MeshCore core + Arduino libs trip IDF's blanket -Werror=all. A bare
# -Wno-error does not cancel it, so strip it outright (warnings stay visible, non-fatal).
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
idf_build_get_property(_co COMPILE_OPTIONS)
list(REMOVE_ITEM _co "-Werror=all")
idf_build_set_property(COMPILE_OPTIONS "${_co}")

# ---- LilyGo T-Display P4 (AMOLED) app define set. -------------------------------------------------
# Unlike the Tanmatsu (badge-bsp + a LoRa bridge), the T-Display P4 has a RAW SX1262 on P4 GPIOs, so
# it uses the core's RadioLib path like the S3 boards — except SX1262 RESET + DIO1 sit on the XL9535
# I2C GPIO expander (the variant's target.cpp drives them; the raw P_LORA_RESET/DIO_1 are -1 = "none").
# Pins from LilyGo's components/private_library/t_display_p4_config.h (see [[tdisplay-p4-port]]).
set(WADA_DEFS
  HAS_TDISPLAY_P4=1 ESP32_PLATFORM MC_VENDORED_TOUCH_APP UI_LVGL=1 HAS_TOUCH_UI=1
  MULTI_TRANSPORT_COMPANION TCP_PORT=5000 WS_PORT=8765 BLE_PIN_CODE=123456
  LORA_FREQ=869.618 LORA_BW=62.5 LORA_SF=8            # EU 868 (same as the V4)
  LORA_TX_POWER=22 MAX_LORA_TX_POWER=22               # SX1262 PA max (dBm); user-tunable in Settings
  MAX_CONTACTS=2000 MAX_GROUP_CHANNELS=40             # P4 + 32MB PSRAM: ample
  ENABLE_PRIVATE_KEY_IMPORT=1 ENABLE_PRIVATE_KEY_EXPORT=1

  # --- LoRa SX1262 (SPI_1) — CS/BUSY/SPI on raw P4 GPIO; RST+DIO1 on the XL9535 expander ---
  USE_SX1262 RADIO_CLASS=CustomSX1262 WRAPPER_CLASS=CustomSX1262Wrapper
  P_LORA_NSS=24 P_LORA_BUSY=6 P_LORA_SCLK=2 P_LORA_MISO=4 P_LORA_MOSI=3
  P_LORA_RESET=-1 P_LORA_DIO_1=-1                     # on XL9535 IO16/IO17 — variant drives these
  MESH_RADIO_DIO1_POLLED=1                            # DIO1 behind the expander → poll RxDone/TxDone over SPI (no host IRQ)
  P_LORA_TX_LED=-1 P_LORA_TX_NEOPIXEL_LED=-1
  SX126X_RX_BOOSTED_GAIN=1
  XL9535_SX1262_RST_IO=16 XL9535_SX1262_DIO1_IO=17    # SX1262 reset + IRQ behind the expander
  XL9535_RF_SWITCH_IO=1                               # SKY13453 VCTL (TX/RX path)

  # --- Display: RM69A10 AMOLED, MIPI-DSI, 568x1232 portrait (self-emissive; no backlight pin) ---
  DISPLAY_CLASS=RM69A10Display
  SCREEN_WIDTH=568 SCREEN_HEIGHT=1232
  XL9535_SCREEN_RST_IO=2

  # --- Touch: HI8561 (I2C_1); RST/INT on the expander ---
  HAS_HI8561_TOUCH=1 PIN_TOUCH_SDA=7 PIN_TOUCH_SCL=8
  XL9535_TOUCH_RST_IO=3 XL9535_TOUCH_INT_IO=4

  # --- Board I2C_1 (XL9535 + touch + PCF8563 RTC + BQ27220 gauge) ---
  PIN_BOARD_SDA=7 PIN_BOARD_SCL=8
  XL9535_SDA=7 XL9535_SCL=8 XL9535_INT_PIN=5

  # --- GPS L76K (UART): module TX -> GPIO22 (ESP receives), module RX -> GPIO23 (ESP transmits).
  # Core convention (initBasicGPS does Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX) = (rx, tx)):
  # PIN_GPS_TX = the GPIO wired to the MODULE'S TX pin; PIN_GPS_RX = wired to the module's RX.
  # Matches LilyGo's t_display_p4_config.h "GPS_TX 22 / GPS_RX 23" naming. The first cut had them
  # swapped (silent: UART opens fine, just never hears NMEA). Wake: XL9535 GPS_WAKE must be HIGH
  # (powerOnSequence) or the L76K sleeps. Baud: L76K power-on default 9600 = the core default. ---
  ENV_INCLUDE_GPS=1 PIN_GPS_TX=22 PIN_GPS_RX=23

  # --- microSD on SDMMC slot 0 (CLK43/CMD44/D0-3=39-42); C6 esp-hosted is slot 1 ---
  BOARD_HAS_SDMMC=1 BOARD_SDMMC_SLOT=0

  # battery via BQ27220 I2C gauge (not ADC); no raw VBAT pin
  PIN_VBAT_READ=-1)
foreach(_d ${WADA_DEFS})
  idf_build_set_property(COMPILE_OPTIONS "-D${_d}" APPEND)
endforeach()
# One-off C6-reflash helper: build with `C6_FLASH_HELPER=1 ./build.sh build` to boot a P4 image that
# never touches the C6 (no C6_EN reset pulse, no AT worker), so the C6 can be held in ROM download mode
# and reflashed over its own USB. Harmless when the env var is unset (normal builds are unaffected).
if(DEFINED ENV{C6_FLASH_HELPER})
  idf_build_set_property(COMPILE_OPTIONS "-DTDP4_C6_FLASH_HELPER=1" APPEND)
  message(STATUS "wadamesh: BUILDING C6-FLASH HELPER (P4 leaves the C6 free)")
endif()

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
