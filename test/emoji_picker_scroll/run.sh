#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lvgl="${1:-$root/.pio/libdeps/LilyGo_TDeck_companion_radio_touch/lvgl}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-clang}"
flags=(-g -O1 -w -fsanitize=address -DLV_CONF_INCLUDE_SIMPLE -I"$root/test/lvgl_anim_uaf" -I"$lvgl")
mkdir -p "$work/obj"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
export CC_CMD="$cc -c ${flags[*]}" OBJ_DIR="$work/obj"
find "$lvgl/src" -name '*.c' -print0 |
  xargs -0 -P "$jobs" -I{} sh -c \
    '$CC_CMD "$1" -o "$OBJ_DIR/$(printf %s "$1" | cksum | cut -d" " -f1).o"' _ {}
"$cc" "${flags[@]}" "$here/repro.c" "$work"/obj/*.o -o "$work/repro"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
if "$work/repro" null > "$work/null.txt" 2>&1; then
  echo "FAIL: null transition did not reproduce the crash"
  exit 1
fi
if ! grep -q 'lv_obj_set_state' "$work/null.txt"; then
  cat "$work/null.txt"
  exit 1
fi
echo "ok: null transition reproduces the state-change crash"
"$work/repro"