#!/usr/bin/env bash
# Fetch the third-party sources the web/host build compiles, at PINNED versions, into
# web/third_party/ (gitignored). Nothing here depends on PlatformIO having run: the
# MeshCore core tag is read from the root platformio.ini (the same pin the firmware
# builds use), LVGL gets the same two source patches the firmware applies.
#
#   web/fetch-deps.sh          fetch what is missing
#   web/fetch-deps.sh --force  re-fetch everything
set -euo pipefail
cd "$(dirname "$0")"
WEB="$(pwd)"
REPO="$(cd .. && pwd)"
TP="$WEB/third_party"
FORCE=0; [ "${1:-}" = "--force" ] && FORCE=1
mkdir -p "$TP"

# MeshCore core pin, straight from the firmware's lib_deps (first touch env wins; all agree).
CORE_TAG="$(grep -m1 -oE 'meshcomod\.git#core-v[0-9][0-9.]*' "$REPO/platformio.ini" | sed 's/.*#//')"
[ -n "$CORE_TAG" ] || { echo "!! could not read the core tag from platformio.ini"; exit 1; }

LVGL_TAG="v8.4.0"            # platformio.ini: lvgl/lvgl @ ^8.3.11 -> 8.4.0 (last 8.x)
CRYPTO_REF="37a76b8f7516568e1c575b6dc9268da1ccaac6b6"   # rweather/arduinolibs (Crypto 0.4.0)
CAYENNE_TAG="1.6.1"          # electroniccats/CayenneLPP @ 1.6.1
AJSON_TAG="v7.4.3"           # bblanchon/ArduinoJson (CayenneLPP.h includes it)
MINIZ_TAG="3.0.2"            # richgel999/miniz: host stand-in for the ESP ROM's tinfl

clone() {  # clone <dir> <url> <ref>
  local dir="$1" url="$2" ref="$3"
  if [ -d "$TP/$dir" ] && [ "$FORCE" = 0 ]; then
    if [ "$(cat "$TP/$dir/.wada-ref" 2>/dev/null)" = "$ref" ]; then echo "  ok  $dir @ $ref"; return; fi
  fi
  rm -rf "$TP/$dir"
  echo "  get $dir @ $ref"
  if git ls-remote --exit-code --tags --heads "$url" "$ref" >/dev/null 2>&1; then
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$ref" "$url" "$TP/$dir"
  else   # a commit id
    git init -q "$TP/$dir"
    git -C "$TP/$dir" fetch -q --depth 1 "$url" "$ref"
    git -C "$TP/$dir" checkout -q FETCH_HEAD
  fi
  rm -rf "$TP/$dir/.git"
  echo "$ref" > "$TP/$dir/.wada-ref"
  FRESH="$FRESH $dir"
}

FRESH=""
clone meshcore     https://github.com/ALLFATHER-BV/meshcomod.git "$CORE_TAG"
clone lvgl         https://github.com/lvgl/lvgl.git              "$LVGL_TAG"
clone arduinolibs  https://github.com/rweather/arduinolibs.git   "$CRYPTO_REF"
clone CayenneLPP   https://github.com/ElectronicCats/CayenneLPP.git "$CAYENNE_TAG"
clone ArduinoJson  https://github.com/bblanchon/ArduinoJson.git  "$AJSON_TAG"
clone miniz        https://github.com/richgel999/miniz.git       "$MINIZ_TAG"

case " $FRESH " in *" lvgl "*)
  # the same LVGL source patches the firmware builds apply (fail-closed scripts)
  python3 "$REPO/scripts/build/patch_lvgl_anim_uaf.py" --patch-file "$TP/lvgl/src/misc/lv_anim.c"
  python3 "$REPO/scripts/build/patch_lvgl_arabic.py"   --patch-file "$TP/lvgl/src/misc/lv_txt_ap.c"
  echo "$LVGL_TAG" > "$TP/lvgl/.wada-ref";;
esac
# miniz's CMake generates this export header; the host build only needs it empty.
[ -f "$TP/miniz/miniz_export.h" ] || printf '#pragma once\n#define MINIZ_EXPORT\n' > "$TP/miniz/miniz_export.h"
echo "done (core $CORE_TAG, lvgl $LVGL_TAG)."
