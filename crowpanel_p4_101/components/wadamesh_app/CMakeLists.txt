# The wadamesh app layer (MyMesh + DataStore + helpers + transports + the LVGL UITask), compiled from
# the repo's src/ plus variants/crowpanel_p4_101/. Mirrors tdisplay_p4/components/wadamesh_app, minus
# the ESP-AT c6_at component (this board's C6 ships esp-hosted firmware).
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(APP "${REPO}/src")
file(GLOB_RECURSE APP_SRCS "${APP}/*.cpp" "${APP}/*.c" "${REPO}/variants/crowpanel_p4_101/*.cpp" "${REPO}/variants/crowpanel_p4_101/*.c")
list(FILTER APP_SRCS EXCLUDE REGEX "/main\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX "/helpers/input/(HeltecV4CapTouch|TDeckKeyboard|TDeckTouch|TDeckTrackball|RakTapV2Touch)\\.cpp$")

file(GLOB LUA_SRCS "${REPO}/lib/lua/src/*.c")

idf_component_register(
  SRCS ${APP_SRCS} ${LUA_SRCS}
  INCLUDE_DIRS "${APP}" "${APP}/ui-touch" "${REPO}/include" "${REPO}/variants/crowpanel_p4_101" "${REPO}/lib/lua/src"
  REQUIRES espressif__arduino-esp32 meshcore lvgl ardlibs RadioLib nvs_flash esp_lcd fatfs
           esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_hosted h2zero__esp-nimble-cpp bt
           soc esp_hw_support app_update driver esp_timer esp_partition spi_flash esp_adc esp_pm espcoredump
           espressif__esp_lcd_ek79007 esp_mm   # esp_mm: esp_cache_msync (framebuffer clear)
  WHOLE_ARCHIVE
)
target_compile_options(${COMPONENT_LIB} PRIVATE -w
  -include "${REPO}/variants/tdisplay_p4/tdisplay_p4_compat.h")   # adcAttachPin() shim (arduino 3.x dropped it)
