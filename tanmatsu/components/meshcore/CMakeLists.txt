# MeshCore core as an IDF component. Payload (core/src, core/variants) is vendored by
# fetch-deps.sh and gitignored; this wrapper is committed.
#
# Source selection mirrors the monorepo's build_as_lib.py for an ESP32 + MC_VENDORED_TOUCH_APP
# build, MINUS the bits the Tanmatsu replaces:
#   - helpers/radiolib/*  -> we use variants/tanmatsu/TanmatsuLoraRadio (no RadioLib)
#   - *Board.cpp          -> the S3/other board classes; Tanmatsu uses variants/tanmatsu/TanmatsuBoard
#   - helpers/ui/*Display* -> S3 panel drivers; we render via LVGL -> badge-bsp
#   - helpers/sensors/*   -> telemetry; deferred (re-add with the sensor libs later)
#   - the MC_VENDORED_TOUCH_APP files -> our own src/ ships them (avoid duplicate symbols)
set(CORE "core/src")
file(GLOB_RECURSE CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/${CORE}/*.cpp")
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/radiolib/")
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/sensors/")
list(FILTER CORE_SRCS EXCLUDE REGEX "(NRF52Board|TBeamBoard)\\.cpp$")   # keep ESP32Board (base TanmatsuBoard extends); drop nRF52 + the RadioLib S3 board
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/ui/.*(Display|OLED|TFT|Epaper|EInk|GxEPD|U8g2|SH1106|SSD1306|ST77|ILI9|Adafruit).*\\.cpp$")
list(FILTER CORE_SRCS EXCLUDE REGEX "(LvglPsramAlloc|TouchDiagTrace)\\.cpp$")
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/esp32/(MultiTransportCompanionInterface|SdNvsPrefs|TCPCompanionServer|TouchPrefsStore|WifiRuntimeStore|WebSocketCompanionServer)\\.cpp$")
# other platforms' helpers (glob is recursive; build_as_lib.py only takes the current platform):
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/(nrf52|stm32|rp2040)/")
list(FILTER CORE_SRCS EXCLUDE REGEX "/ESPNOWRadio\\.cpp$")                       # ESP-NOW radio — we use the LoRa bridge
# SerialBLEInterface.cpp IS compiled now (BLE over the C6, ported to esp-nimble-cpp 2.x).

# Core's bundled ed25519 C implementation (Identity.cpp does #include <ed_25519.h>);
# PlatformIO auto-compiles it as a nested lib, IDF needs it added explicitly.
file(GLOB ED25519_SRCS "${CMAKE_CURRENT_LIST_DIR}/core/lib/ed25519/*.c")

idf_component_register(
  SRCS ${CORE_SRCS} ${ED25519_SRCS}
  INCLUDE_DIRS "${CORE}" "core/lib/ed25519"
  REQUIRES espressif__arduino-esp32 ardlibs nvs_flash efuse h2zero__esp-nimble-cpp bt
)
# Vendored core — builds clean under PlatformIO's laxer flags; don't let IDF's strict -Werror
# (reorder/unused/…) fail the build on code we consume as a library.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error
  -include "${CMAKE_CURRENT_LIST_DIR}/../../../variants/tanmatsu/tanmatsu_compat.h")   # adcAttachPin() shim
