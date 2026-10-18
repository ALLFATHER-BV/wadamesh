# SPDX-License-Identifier: GPL-3.0-or-later
# wadamesh in the browser: the real touch UI (src/ui-touch) built for a host OS.
#
#   web/build.sh host   -> build-host/wadamesh-host  (native, SDL2 window)
#   web/build.sh web    -> phase 2 (Emscripten, same CMake with emcmake)
#
# Layout (see WEB_BUILD.md at the repo root):
#   third_party/  pinned sources fetched by fetch-deps.sh (gitignored)
#   compat/       Arduino / ESP-IDF / FreeRTOS surface on a host OS
#   mesh/         MyMesh stand-in (the UI's compile-time contract) + backends
#   platform/     SDL2 display/input, the "web" board globals, main()
cmake_minimum_required(VERSION 3.20)
project(wadamesh_web C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(WEB  ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(REPO "${WEB}/.." ABSOLUTE)
set(TP   ${WEB}/third_party)
set(SRC  ${REPO}/src)
set(UI   ${SRC}/ui-touch)
set(CORE ${TP}/meshcore/src)

if(NOT EXISTS ${CORE}/Mesh.h OR NOT EXISTS ${TP}/lvgl/lvgl.h)
  message(FATAL_ERROR "third-party sources missing: run web/fetch-deps.sh")
endif()

# ---- defines every translation unit sees -------------------------------------
set(WADA_COMMON_DEFS
  WADA_WEB=1                 # the board macro (src/ui-touch/device_caps.h)
  ARDUINO=10819              # "an Arduino API is present" (libraries test it)
  LV_CONF_INCLUDE_SIMPLE=1
  LV_CONF_PATH=lv_conf.h
  LV_MEMCPY_MEMSET_STD=1     # as every touch env in platformio.ini
)
set(WADA_COMPAT_INC ${WEB}/compat ${WEB}/compat/arduino)

# ---- LVGL 8.4 (same lv_conf.h as the firmware: include/lv_conf.h) ----------------
file(GLOB_RECURSE LVGL_SRCS CONFIGURE_DEPENDS ${TP}/lvgl/src/*.c)
add_library(wada_lvgl STATIC ${LVGL_SRCS})
target_include_directories(wada_lvgl PUBLIC ${REPO}/include ${TP}/lvgl ${TP} PRIVATE ${WADA_COMPAT_INC})
target_compile_definitions(wada_lvgl PUBLIC ${WADA_COMMON_DEFS})
target_compile_options(wada_lvgl PRIVATE -w -O2)

# ---- Lua 5.4 (lib/lua, as the firmware) ------------------------------------------
file(GLOB LUA_SRCS ${REPO}/lib/lua/src/*.c)
list(FILTER LUA_SRCS EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(wada_lua STATIC ${LUA_SRCS})
target_include_directories(wada_lua PUBLIC ${REPO}/lib/lua/src)
target_compile_options(wada_lua PRIVATE -w)

# ---- third-party libraries (no ESP32 define: their portable code paths) ----------
set(CRYPTO ${TP}/arduinolibs/libraries/Crypto)
add_library(wada_thirdparty STATIC
  ${CRYPTO}/Crypto.cpp ${CRYPTO}/Hash.cpp ${CRYPTO}/SHA256.cpp ${CRYPTO}/SHA512.cpp
  ${CRYPTO}/BlockCipher.cpp ${CRYPTO}/AESCommon.cpp ${CRYPTO}/AES128.cpp ${CRYPTO}/AES192.cpp ${CRYPTO}/AES256.cpp
  ${CRYPTO}/Ed25519.cpp ${CRYPTO}/Curve25519.cpp ${CRYPTO}/BigNumberUtil.cpp ${CRYPTO}/SHA3.cpp ${CRYPTO}/KeccakCore.cpp
  ${TP}/CayenneLPP/src/CayenneLPP.cpp
  ${TP}/miniz/miniz.c ${TP}/miniz/miniz_tinfl.c ${TP}/miniz/miniz_tdef.c   # ROM tinfl stand-in + the PNG writer
  # the String/Print/Stream/FS layer of arduino-esp32 2.0.17, unmodified
  ${WEB}/compat/arduino/WString.cpp ${WEB}/compat/arduino/Print.cpp ${WEB}/compat/arduino/Stream.cpp
  ${WEB}/compat/arduino/StreamString.cpp ${WEB}/compat/arduino/stdlib_noniso.c
  ${WEB}/compat/arduino/FS.cpp ${WEB}/compat/arduino/vfs_api.cpp
)
target_include_directories(wada_thirdparty PUBLIC ${WADA_COMPAT_INC} ${CRYPTO} ${TP}/CayenneLPP/src
                           ${TP}/ArduinoJson/src ${TP}/miniz)
target_compile_definitions(wada_thirdparty PUBLIC ${WADA_COMMON_DEFS})
target_compile_options(wada_thirdparty PRIVATE -w)

# ---- MeshCore core: only the plain-data / crypto parts the UI touches ------------
# (No Dispatcher/Mesh/BaseChatMesh .cpp: the mesh is the stand-in in mesh/.)
file(GLOB ED25519_SRCS ${TP}/meshcore/lib/ed25519/*.c)
add_library(wada_core STATIC
  ${CORE}/Identity.cpp ${CORE}/Utils.cpp ${CORE}/Packet.cpp
  ${CORE}/helpers/AdvertDataHelpers.cpp ${CORE}/helpers/TxtDataHelpers.cpp
  ${ED25519_SRCS})
target_include_directories(wada_core PUBLIC ${CORE} ${TP}/meshcore/lib/ed25519)
target_link_libraries(wada_core PUBLIC wada_thirdparty)
target_compile_options(wada_core PRIVATE -w)

# ---- the app: the REAL touch UI + the web board ------------------------------------
# Every .cpp/.c in src/ui-touch is compiled (a new UI file joins automatically); only
# files that are another board's driver are excluded, each with its reason.
file(GLOB UI_SRCS CONFIGURE_DEPENDS ${UI}/*.cpp ${UI}/*.c)
set(APP_SRCS
  ${UI_SRCS}
  ${SRC}/LvglPsramAlloc.cpp
  ${SRC}/helpers/TouchDiagTrace.cpp
  ${SRC}/helpers/esp32/TouchPrefsStore.cpp
  ${SRC}/helpers/esp32/SdNvsPrefs.cpp
  ${SRC}/helpers/esp32/WebMirror.cpp
  ${SRC}/helpers/esp32/WifiRuntimeStore.cpp
  ${SRC}/helpers/ui/UIColorPalette.cpp
  # core helpers whose file layer is fs::FS (needs the ESP32 define, so they build with the app)
  ${CORE}/helpers/RegionMap.cpp
  ${CORE}/helpers/TransportKeyStore.cpp
  ${CORE}/helpers/IdentityStore.cpp
)
file(GLOB WEB_SRCS CONFIGURE_DEPENDS ${WEB}/compat/src/*.cpp ${WEB}/mesh/*.cpp ${WEB}/platform/*.cpp)

# Version strings: the defaults src/MyMesh.h carries (release.sh refreshes them), so the
# web build's About page names the same firmware as the device build of this commit.
file(STRINGS ${SRC}/MyMesh.h _mm_ver REGEX "^#define FIRMWARE_(VERSION|BUILD_DATE|CORE_VERSION|VER_CODE) ")
set(WADA_VERSION_DEFS "")
foreach(_l IN LISTS _mm_ver)
  string(REGEX REPLACE "^#define (FIRMWARE_[A-Z_]+) +(.*)$" "\\1=\\2" _d "${_l}")
  list(APPEND WADA_VERSION_DEFS "${_d}")
endforeach()

set(WADA_APP_DEFS
  ESP32=1                    # the UI's ESP32 code paths ARE the device code paths; compat/ backs them
  HAS_TOUCH_UI=1
  UI_LVGL=1
  WADAMESH_FORK_BUILD=1
  HAS_CARDKB=1               # the host keyboard drives the UI's external-keyboard path (platform/WebKeyboard.cpp)
  MULTI_TRANSPORT_COMPANION=1   # every touch board builds the multi-transport UI; compat/ stubs the transports
  MAX_CONTACTS=500
  MAX_GROUP_CHANNELS=40
  ENABLE_ADVERT_ON_BOOT=0
  NDEBUG
)

find_package(PkgConfig QUIET)
if(EMSCRIPTEN)
  set(SDL2_LIBS "")
  set(SDL2_CFLAGS "-sUSE_SDL=2")
else()
  find_package(SDL2 REQUIRED)
  set(SDL2_LIBS SDL2::SDL2)
endif()

add_executable(wadamesh-host ${APP_SRCS} ${WEB_SRCS})
target_include_directories(wadamesh-host PRIVATE
  ${WEB}/mesh ${WEB}/platform     # <target.h> + the MyMesh stand-in
  ${WADA_COMPAT_INC}
  ${SRC} ${UI} ${REPO}/include
  ${CORE}
  ${TP}/ArduinoJson/src)
target_compile_definitions(wadamesh-host PRIVATE ${WADA_APP_DEFS} ${WADA_VERSION_DEFS})
target_link_libraries(wadamesh-host PRIVATE wada_core wada_lvgl wada_lua wada_thirdparty ${SDL2_LIBS})
# The UI is warning-clean under the firmware's GCC flags, not under clang -Wall; keep
# the host build readable. Errors are what matter here (they are the drift alarm).
target_compile_options(wadamesh-host PRIVATE -w -fno-strict-aliasing)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # 81k-line UITask.cpp: keep clang's bracket-nesting / constexpr limits out of the way.
  target_compile_options(wadamesh-host PRIVATE -fbracket-depth=512)
endif()

# The browser companion transports (Web Serial / Web Bluetooth / WebSocket) are JS glue
# that shares the module scope; the framing is C++ (web/mesh/BrowserTransport.cpp).
if(EMSCRIPTEN)
  target_link_options(wadamesh-host PRIVATE "SHELL:--pre-js ${WEB}/mesh/js/companion-transports.js")
endif()
