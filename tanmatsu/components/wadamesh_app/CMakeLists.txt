# The wadamesh app layer (MyMesh + DataStore + helpers + transports + the LVGL UITask),
# compiled directly from the repo's src/ — NOT vendored (it's our own code, in-tree).
# Excluded: main.cpp (our app_main lives in tanmatsu/main/), and the S3 input drivers
# (HeltecV4CapTouch / TDeck*) — the Tanmatsu's input comes from bsp_input.
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(APP "${REPO}/src")
# app src/ + the Tanmatsu variant glue (target.cpp = board/radio/display globals + radio_init,
# TanmatsuLoraRadio.cpp = the LoRa bridge).
file(GLOB_RECURSE APP_SRCS "${APP}/*.cpp" "${APP}/*.c" "${REPO}/variants/tanmatsu/*.cpp")
list(FILTER APP_SRCS EXCLUDE REGEX "/main\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX "/helpers/input/(HeltecV4CapTouch|TDeckKeyboard|TDeckTouch|TDeckTrackball)\\.cpp$")

idf_component_register(
  SRCS ${APP_SRCS}
  INCLUDE_DIRS "${APP}" "${APP}/ui-touch" "${REPO}/include" "${REPO}/variants/tanmatsu"
  REQUIRES espressif__arduino-esp32 meshcore lvgl ardlibs nvs_flash badge-bsp esp_lcd fatfs
           tanmatsu-lora tanmatsu-wifi h2zero__esp-nimble-cpp bt
           soc esp_hw_support app_update driver esp_timer esp_partition spi_flash esp_adc esp_pm espcoredump
  # IDF links component archives in one groupless ordered pass (with a repeat trick), NOT a
  # --start-group. arduino's NetworkClientSecure references our ssl_client stubs, but arduino is
  # re-scanned (pulling NetworkClientSecure.cpp.obj, which makes start_ssl_client undefined) only
  # AFTER this archive's last occurrence — so the stub object never gets pulled. Whole-archive this
  # component so the strong stub defs are always present. --gc-sections still drops unreferenced
  # sections, so the size impact is nil.
  WHOLE_ARCHIVE
)
# our own code, but huge + LVGL-heavy — keep the first port building; don't -Werror on warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -w
  -include "${REPO}/variants/tanmatsu/tanmatsu_compat.h")   # adcAttachPin() shim (arduino 3.x dropped it)
