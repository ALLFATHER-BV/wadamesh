# The wadamesh app layer (MyMesh + DataStore + helpers + transports + the LVGL UITask),
# compiled directly from the repo's src/ — our own in-tree code (NOT vendored). Mirrors
# tanmatsu/components/wadamesh_app but builds variants/tdisplay_p4/ instead of variants/tanmatsu/,
# and — unlike the badge-bsp Tanmatsu — keeps the RAW-pin radio path (the T-Display P4 has a real
# SX1262 on P4 GPIOs). Excludes main.cpp (our app_main is in tdisplay_p4/main/) and the S3 input
# drivers (the P4's touch is HI8561, in variants/tdisplay_p4/).
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(APP "${REPO}/src")
file(GLOB_RECURSE APP_SRCS "${APP}/*.cpp" "${APP}/*.c" "${REPO}/variants/tdisplay_p4/*.cpp")
list(FILTER APP_SRCS EXCLUDE REGEX "/main\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX "/helpers/input/(HeltecV4CapTouch|TDeckKeyboard|TDeckTouch|TDeckTrackball|RakTapV2Touch)\\.cpp$")

idf_component_register(
  SRCS ${APP_SRCS}
  INCLUDE_DIRS "${APP}" "${APP}/ui-touch" "${REPO}/include" "${REPO}/variants/tdisplay_p4"
  REQUIRES espressif__arduino-esp32 meshcore lvgl ardlibs RadioLib nvs_flash esp_lcd fatfs
           esp_driver_i2c esp_driver_gpio esp_hosted h2zero__esp-nimble-cpp bt
           soc esp_hw_support app_update driver esp_timer esp_partition spi_flash esp_adc esp_pm espcoredump
           c6_at   # C6WifiShim facade (WiFi.* -> AT-over-SDIO; UITask + WifiRuntimeStore include it)
  # See the Tanmatsu note: whole-archive so the strong ssl_client stub defs are always present.
  WHOLE_ARCHIVE
)
# Our own code, but huge + LVGL-heavy — keep the first port building; don't -Werror on warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -w
  -include "${REPO}/variants/tdisplay_p4/tdisplay_p4_compat.h")   # adcAttachPin() shim (arduino 3.x dropped it)
