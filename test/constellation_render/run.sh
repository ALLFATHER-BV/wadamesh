#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Renders the Constellation screensaver at every board's logical screen size
# (both orientations where the UI can rotate) and writes PNGs for review.
#
# Usage: test/constellation_render/run.sh [outdir] [path/to/lvgl]
# Defaults to the LVGL that PlatformIO fetched for any env (run a pio build first).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/doc-shots/constellation}"
lvgl="${2:-}"
if [ -z "$lvgl" ]; then
  for d in "$root"/.pio/libdeps/*/lvgl; do
    [ -f "$d/lvgl.h" ] && { lvgl="$d"; break; }
  done
fi
[ -n "$lvgl" ] && [ -f "$lvgl/lvgl.h" ] || {
  echo "LVGL sources not found; run a PlatformIO build first or pass the lvgl path" >&2
  exit 2
}
mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
flags=(-O1 -w -DLV_CONF_INCLUDE_SIMPLE -I"$here" -I"$lvgl")

mkdir -p "$work/obj"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
export CC_CMD="${CC:-clang} -c ${flags[*]}" OBJ_DIR="$work/obj"
find "$lvgl/src" -name '*.c' -print0 |
  xargs -0 -P "$jobs" -I{} sh -c \
    '$CC_CMD "$1" -o "$OBJ_DIR/$(printf %s "$1" | cksum | cut -d" " -f1).o"' _ {}
cxx="${CXX:-clang++}"
"$cxx" -std=c++17 -c "${flags[@]}" "$root/src/ui-touch/Constellation.cpp" -o "$work/cst.o"
"${CC:-clang}" -c "${flags[@]}" "$root/src/ui-touch/clock_font_40.c" -o "$work/f40.o"
"${CC:-clang}" -c "${flags[@]}" "$root/src/ui-touch/ui_semibold_12.c" -o "$work/semi12.o"
"${CC:-clang}" -c "${flags[@]}" -DHAS_TANMATSU "$root/src/ui-touch/clock_font_96.c" -o "$work/f96.o"
"$cxx" -std=c++17 -c "${flags[@]}" "$here/render.cpp" -o "$work/render.o"
"$cxx" "$work/render.o" "$work/cst.o" "$work/f40.o" "$work/semi12.o" "$work/obj"/*.o -o "$work/render"
# The Tanmatsu build swaps in the 96 px clock face.
"$cxx" -std=c++17 -c "${flags[@]}" -DHAS_TANMATSU "$root/src/ui-touch/Constellation.cpp" -o "$work/cst96.o"
"$cxx" "$work/render.o" "$work/cst96.o" "$work/f96.o" "$work/semi12.o" "$work/obj"/*.o -o "$work/render96"

# name width height [binary]
while read -r name w h bin; do
  [ -n "$name" ] || continue
  exe="$work/${bin:-render}"
  for mode in still ping; do
    arg=""; [ "$mode" = ping ] && arg=ping
    "$exe" "$w" "$h" "$work/$name-$mode.bmp" $arg >/dev/null
    sips -s format png "$work/$name-$mode.bmp" --out "$out/$name-$mode.png" >/dev/null
  done
  echo "ok: $name ${w}x${h}"
done <<'SIZES'
tdeck-v4-m9      320 240
tdeck-portrait   240 320
crowpanel-rak    480 320
crowpanel-port   320 480
pager            480 222
tdisplay-p4      284 616
tanmatsu         800 480 render96
SIZES
echo "PNGs in $out"
