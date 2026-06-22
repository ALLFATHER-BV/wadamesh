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
  MAX_CONTACTS=500 MAX_GROUP_CHANNELS=40   # 2000 was a ~400KB internal-DRAM strand; BT's SDIO RX buffers (MALLOC_CAP_INTERNAL|DMA) need that headroom
  ENABLE_PRIVATE_KEY_IMPORT=1 ENABLE_PRIVATE_KEY_EXPORT=1
  DISPLAY_CLASS=TanmatsuDisplay
  # the core's ESP32Board.h reads these raw pins directly; Tanmatsu uses badge-bsp, so -1 (none):
  P_LORA_DIO_1=-1 P_LORA_TX_LED=-1 P_LORA_TX_NEOPIXEL_LED=-1
  PIN_BOARD_SCL=-1 PIN_BOARD_SDA=-1 PIN_VBAT_READ=-1)
foreach(_d ${WADA_DEFS})
  idf_build_set_property(COMPILE_OPTIONS "-D${_d}" APPEND)
endforeach()
# (the adcAttachPin compat shim is force-included per-component in meshcore + wadamesh_app —
#  NOT globally, which would feed the C++ header to FreeRTOS's .S assembly files and break them.)
