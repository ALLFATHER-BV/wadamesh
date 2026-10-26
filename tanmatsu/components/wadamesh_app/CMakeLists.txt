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
# The colour emoji ship deflated here (variants/tanmatsu/emoji_deflated.cpp, ~105 KB smaller):
# the launcher maps at most 50 x 64 KB of an AppFS app. emoji_data.c stays the source; its
# images are deflated into the build directory below.
list(FILTER APP_SRCS EXCLUDE REGEX "/ui-touch/emoji_data\\.c$")

# Vendored Lua 5.4 VM (lib/lua) — the Lua app host (LUA_APPS.md). Plain C, RISC-V clean.
file(GLOB LUA_SRCS "${REPO}/lib/lua/src/*.c")

idf_component_register(
  SRCS ${APP_SRCS} ${LUA_SRCS}
  INCLUDE_DIRS "${APP}" "${APP}/ui-touch" "${REPO}/include" "${REPO}/variants/tanmatsu" "${REPO}/lib/lua/src"
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
# emoji_deflated.h: generated from emoji_data.c on every change, never committed. The script
# verifies the round trip before it writes, so a bad blob fails the build, not the device.
idf_build_get_property(_python PYTHON)
set(_emoji_src "${APP}/ui-touch/emoji_data.c")
set(_emoji_gen "${REPO}/scripts/build/gen-emoji-deflated.py")
set(_emoji_h "${CMAKE_CURRENT_BINARY_DIR}/emoji_deflated/emoji_deflated.h")
add_custom_command(OUTPUT "${_emoji_h}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/emoji_deflated"
  COMMAND ${_python} "${_emoji_gen}" "${_emoji_src}" "${_emoji_h}"
  DEPENDS "${_emoji_src}" "${_emoji_gen}"
  VERBATIM)
add_custom_target(wadamesh_emoji_deflated DEPENDS "${_emoji_h}")
add_dependencies(${COMPONENT_LIB} wadamesh_emoji_deflated)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/emoji_deflated")
target_compile_definitions(${COMPONENT_LIB} PRIVATE WADA_EMOJI_DEFLATED=1)

# our own code, but huge + LVGL-heavy — keep the first port building; don't -Werror on warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -w
  -include "${REPO}/variants/tanmatsu/tanmatsu_compat.h")   # adcAttachPin() shim (arduino 3.x dropped it)
