#!/usr/bin/env bash
# Populate tanmatsu/components/ with the MeshCore core + the Arduino libraries wadamesh needs,
# sourced from PlatformIO's already-resolved libdeps. Run an S3 build first to populate them:
#   /Users/kaj/Library/Python/3.9/bin/pio run -e LilyGo_TDeck_companion_radio_touch
# Vendored payloads are gitignored (regenerate with this script); the committed component
# wrappers (components/meshcore/CMakeLists.txt, components/ardlibs/CMakeLists.txt, the
# esp_hosted override) are tracked.
set -euo pipefail
cd "$(dirname "$0")"
LIBDEPS="../.pio/libdeps/LilyGo_TDeck_companion_radio_touch"
[ -d "$LIBDEPS/MeshCore" ] || { echo "!! $LIBDEPS/MeshCore missing — run an S3 'pio run' first"; exit 1; }

mkdir -p components

# --- MeshCore core: copy src/ + variants/ under the committed meshcore wrapper ---
rm -rf components/meshcore/core
mkdir -p components/meshcore/core
cp -R "$LIBDEPS/MeshCore/src"      components/meshcore/core/src
cp -R "$LIBDEPS/MeshCore/variants" components/meshcore/core/variants 2>/dev/null || true
mkdir -p components/meshcore/core/lib
cp -R "$LIBDEPS/MeshCore/lib/ed25519" components/meshcore/core/lib/ed25519   # bundled C ed25519 (Identity.cpp)
echo "vendored: meshcore core ($(find components/meshcore/core/src -name '*.cpp' | wc -l | tr -d ' ') cpp)"

# --- LVGL 8.4 (own component; big; uses the repo's include/lv_conf.h) ---
rm -rf components/lvgl/upstream
mkdir -p components/lvgl
cp -R "$LIBDEPS/lvgl" components/lvgl/upstream
echo "vendored: lvgl ($(find components/lvgl/upstream/src -name '*.c' | wc -l | tr -d ' ') c)"

# --- Arduino libraries: ALL go into ONE 'ardlibs' component so inter-library #includes
#     resolve without per-lib REQUIRES (e.g. RTClib -> Adafruit BusIO). Payload gitignored. ---
rm -rf components/ardlibs/upstream
mkdir -p components/ardlibs/upstream
vend() {  # vend <libdeps-subdir>
  local src="$1"; local dst="${src// /_}"   # spaces in dir names break IDF component-req matching
  [ -d "$LIBDEPS/$src" ] || { echo "  skip ($src not in libdeps)"; return; }
  cp -R "$LIBDEPS/$src" "components/ardlibs/upstream/$dst"
  echo "  + $src -> $dst"
}
# the core's own lib_deps (minus RadioLib — replaced by the TanmatsuLoraRadio bridge) + their deps:
vend "Crypto"
vend "RTClib"
vend "Adafruit BusIO"        # RTClib + RV3028 need Adafruit_I2CDevice.h
vend "Adafruit Unified Sensor"
vend "Melopero RV3028"
vend "CayenneLPP"
vend "ArduinoJson"           # header-only; some core/app files include it
vend "base64"                # base64.hpp (companion / QR code paths)
# app-layer libs (validate they build on P4; used once wadamesh_app is wired):
vend "MicroNMEA"             # GPS NMEA parsing
vend "AsyncTCP"              # the core's ESP32Board HTTP-OTA + companion-over-IP transport
vend "ESPAsyncWebServer"     # WebSocket/HTTP companion server (needs the task WDT on — sdkconfigs/wadamesh)
vend "AsyncElegantOTA"       # OTA web UI (the core's ESP32Board HTTP-OTA)
# Bump the component CMakeLists so the next build re-globs (IDF component scripts can't use
# file(GLOB CONFIGURE_DEPENDS) — they run in script mode during requirement expansion).
touch components/meshcore/CMakeLists.txt components/ardlibs/CMakeLists.txt 2>/dev/null || true
echo "done."
