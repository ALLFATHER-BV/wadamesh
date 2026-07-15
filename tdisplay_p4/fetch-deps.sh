#!/usr/bin/env bash
# Populate tdisplay_p4/components/ with the vendored payloads a fresh clone needs, sourced
# from PlatformIO's already-resolved libdeps (same convention as tanmatsu/fetch-deps.sh —
# payloads are gitignored, the committed component wrappers are tracked). Run an S3 build
# first to populate libdeps:
#   /Users/kaj/Library/Python/3.9/bin/pio run -e LilyGo_TDeck_companion_radio_touch
set -euo pipefail
cd "$(dirname "$0")"
LIBDEPS="../.pio/libdeps/LilyGo_TDeck_companion_radio_touch"
[ -d "$LIBDEPS/MeshCore" ] || { echo "!! $LIBDEPS/MeshCore missing — run an S3 'pio run' first"; exit 1; }

mkdir -p components

# --- MeshCore core: shared with the Tanmatsu — one vendored copy, symlinked here.
#     Run tanmatsu/fetch-deps.sh first (or let it be run by this one) to materialize it.
[ -d ../tanmatsu/components/meshcore/core/src ] || ../tanmatsu/fetch-deps.sh
mkdir -p components/meshcore
[ -e components/meshcore/core ] || ln -s ../../../tanmatsu/components/meshcore/core components/meshcore/core
echo "linked: meshcore core -> tanmatsu's vendored copy"

# --- LVGL 8.4 (own component; big; uses the repo's include/lv_conf.h) ---
rm -rf components/lvgl/upstream
mkdir -p components/lvgl
cp -R "$LIBDEPS/lvgl" components/lvgl/upstream
echo "vendored: lvgl ($(find components/lvgl/upstream/src -name '*.c' | wc -l | tr -d ' ') c)"

# --- RadioLib: the P4 drives a RAW SX1262 through the core's RadioLib path (unlike the
#     Tanmatsu's C6 LoRa bridge), so the resolved RadioLib is vendored as its own component. ---
rm -rf components/RadioLib/src components/RadioLib/library.properties components/RadioLib/license.txt
cp -R "$LIBDEPS/RadioLib/src"               components/RadioLib/src
cp    "$LIBDEPS/RadioLib/library.properties" components/RadioLib/library.properties
cp    "$LIBDEPS/RadioLib/LICENSE"            components/RadioLib/license.txt 2>/dev/null || \
  cp  "$LIBDEPS/RadioLib/license.txt"        components/RadioLib/license.txt 2>/dev/null || true
echo "vendored: RadioLib $(grep -m1 '^version=' components/RadioLib/library.properties | cut -d= -f2)"

# --- Arduino libraries: ALL into ONE 'ardlibs' component (inter-library #includes resolve
#     without per-lib REQUIRES). Same list as the Tanmatsu. ---
rm -rf components/ardlibs/upstream
mkdir -p components/ardlibs/upstream
vend() {
  local src="$1"; local dst="${src// /_}"
  [ -d "$LIBDEPS/$src" ] || { echo "  skip ($src not in libdeps)"; return; }
  cp -R "$LIBDEPS/$src" "components/ardlibs/upstream/$dst"
  echo "  + $src -> $dst"
}
vend "Crypto"
vend "RTClib"
vend "Adafruit BusIO"
vend "Adafruit Unified Sensor"
vend "Melopero RV3028"
vend "CayenneLPP"
vend "ArduinoJson"
vend "base64"
vend "MicroNMEA"
vend "AsyncTCP"
vend "ESPAsyncWebServer"
vend "AsyncElegantOTA"

touch components/meshcore/CMakeLists.txt components/ardlibs/CMakeLists.txt \
      components/RadioLib/CMakeLists.txt components/wadamesh_app/CMakeLists.txt 2>/dev/null || true
echo "done."
