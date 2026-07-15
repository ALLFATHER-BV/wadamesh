# RadioLib 7.7.1 as an ESP-IDF component for the T-Display P4 (raw SX1262 path).
#
# The Tanmatsu never needed RadioLib (it uses the LoRa bridge), so this component
# is P4-local — it is intentionally NOT added to the shared `ardlibs` so the
# Tanmatsu build stays RadioLib-free.
#
# Unlike RadioLib's stock IDF CMake (which excludes ALL src/hal/*.cpp and expects
# the caller to supply a RadioLibHal), we build with the *Arduino* HAL: our
# target.cpp constructs `new Module(NSS, DIO1, RST, BUSY, SPIClass&)`, whose
# default hal is ArduinoHal. So we KEEP src/hal/Arduino/ and drop only the other
# platforms' duino HALs (RPi/RPiPico/Stm32duino).
#
# RadioLib auto-selects the Arduino path via `#if defined(ARDUINO)` in BuildOpt.h,
# but arduino-esp32 exports -DARDUINO only PRIVATE-ly, so RadioLib's own TUs (and
# our consumers) wouldn't see it. We therefore force RADIOLIB_BUILD_ARDUINO as a
# PUBLIC define: it reaches both RadioLib's sources AND wadamesh_app (which
# includes <RadioLib.h>), keeping the Module class layout identical on both sides.

file(GLOB_RECURSE RADIOLIB_SOURCES "src/*.cpp")
list(FILTER RADIOLIB_SOURCES EXCLUDE REGEX "src/hal/(RPi|RPiPico|Stm32duino)/")

idf_component_register(
  SRCS ${RADIOLIB_SOURCES}
  INCLUDE_DIRS "." "src"
  REQUIRES espressif__arduino-esp32
)

# PUBLIC so consumers that include <RadioLib.h> agree on the Arduino build path.
#  - RADIOLIB_BUILD_ARDUINO : use the Arduino HAL (ArduinoHal.cpp).
#  - RADIOLIB_GODMODE       : expose SX126x private members — MeshCore's core
#                             CustomSX1262Wrapper/SX126xReset reach into mod/
#                             spreadingFactor/freqMHz (matches the S3 PlatformIO
#                             build flags: -DRADIOLIB_GODMODE=1 -DRADIOLIB_STATIC_ONLY=1).
#  - RADIOLIB_STATIC_ONLY   : static buffers (must match on both sides — layout).
target_compile_definitions(${COMPONENT_LIB} PUBLIC
  RADIOLIB_BUILD_ARDUINO RADIOLIB_GODMODE=1 RADIOLIB_STATIC_ONLY=1)
# Third-party code we don't own — never -Werror-fail on it.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
