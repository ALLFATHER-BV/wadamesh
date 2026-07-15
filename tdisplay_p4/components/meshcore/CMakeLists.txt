# MeshCore core as an IDF component for the T-Display P4. Shares the SAME vendored
# core source as the Tanmatsu (core/ is a symlink to ../../tanmatsu/components/meshcore/core),
# but with P4-specific build rules: unlike the Tanmatsu (which reaches LoRa through the
# TanmatsuLoraRadio bridge and drops RadioLib entirely), the T-Display P4 has a REAL SX1262,
# so it compiles the core's helpers/radiolib/ RadioLib wrappers (RadioLibWrappers.cpp — the
# CustomSX1262Wrapper + buffered-RX drain/acquire methods MyMesh calls) and REQUIREs RadioLib.
#
# Everything else mirrors the Tanmatsu selection (see its CMakeLists): drop nRF52/TBeam boards,
# the S3 panel drivers, sensors, other-platform helpers, ESP-NOW, and the MC_VENDORED_TOUCH_APP
# files our own src/ ships.
set(CORE "core/src")
file(GLOB_RECURSE CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/${CORE}/*.cpp")
# NOTE: helpers/radiolib/ is NOT excluded here (unlike the Tanmatsu) — the P4 needs it.
# helpers/sensors/ is NOT fully excluded either: the P4 has a real GPS (L76K), so it compiles
# EnvironmentSensorManager.cpp (its heavy telemetry drivers are all #if ENV_INCLUDE_* — off here,
# leaving just the GPS/NMEA path). Only the RAK soil-moisture sensor (a separate I2C lib) is dropped.
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/sensors/RAK12035_SoilMoisture\\.cpp$")
list(FILTER CORE_SRCS EXCLUDE REGEX "(NRF52Board|TBeamBoard)\\.cpp$")   # keep ESP32Board (base TDisplayP4Board extends); drop nRF52 + TBeam
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/ui/.*(Display|OLED|TFT|Epaper|EInk|GxEPD|U8g2|SH1106|SSD1306|ST77|ILI9|Adafruit).*\\.cpp$")
list(FILTER CORE_SRCS EXCLUDE REGEX "(LvglPsramAlloc|TouchDiagTrace)\\.cpp$")
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/esp32/(MultiTransportCompanionInterface|SdNvsPrefs|TCPCompanionServer|TouchPrefsStore|WifiRuntimeStore|WebSocketCompanionServer)\\.cpp$")
list(FILTER CORE_SRCS EXCLUDE REGEX "/helpers/(nrf52|stm32|rp2040)/")
list(FILTER CORE_SRCS EXCLUDE REGEX "/ESPNOWRadio\\.cpp$")               # ESP-NOW radio — the P4 uses the SX1262

# Core's bundled ed25519 C implementation (Identity.cpp does #include <ed_25519.h>).
file(GLOB ED25519_SRCS "${CMAKE_CURRENT_LIST_DIR}/core/lib/ed25519/*.c")

idf_component_register(
  SRCS ${CORE_SRCS} ${ED25519_SRCS}
  INCLUDE_DIRS "${CORE}" "core/lib/ed25519"
  REQUIRES espressif__arduino-esp32 ardlibs RadioLib nvs_flash efuse h2zero__esp-nimble-cpp bt
)
# Vendored core — don't let IDF's strict -Werror fail the build on code we consume as a library.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error
  -include "${CMAKE_CURRENT_LIST_DIR}/../../../variants/tdisplay_p4/tdisplay_p4_compat.h")   # adcAttachPin() shim
