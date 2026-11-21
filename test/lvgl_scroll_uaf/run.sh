#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host regression test for the beta_102 T-Deck scroll panic (see repro.c).
# Builds LVGL 8.4 with AddressSanitizer twice, once with stock lv_obj_tree.c and
# once with scripts/build/patch_lvgl_scroll_uaf.py applied, and checks:
#   stock   + press / release -> heap-use-after-free (proves the test sees the bug)
#   patched + press / release -> clean, scroll_obj dropped with the page
#
# Usage: test/lvgl_scroll_uaf/run.sh [path/to/lvgl]
# Defaults to the LVGL that PlatformIO fetched for any env (run a pio build first).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lvgl="${1:-}"
if [ -z "$lvgl" ]; then
  for d in "$root"/.pio/libdeps/*/lvgl; do
    [ -f "$d/src/core/lv_obj_tree.c" ] && { lvgl="$d"; break; }
  done
fi
[ -n "$lvgl" ] && [ -f "$lvgl/src/core/lv_obj_tree.c" ] || {
  echo "LVGL sources not found; run a PlatformIO build first or pass the lvgl path" >&2
  exit 2
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-clang}"
flags=(-g -O1 -w -fsanitize=address -DLV_CONF_INCLUDE_SIMPLE -I"$here" -I"$lvgl")

python3 - "$root/scripts/build/patch_lvgl_scroll_uaf.py" "$lvgl/src/core/lv_obj_tree.c" "$work" <<'EOF'
import importlib.util, sys
script, source_path, work = sys.argv[1:]
spec = importlib.util.spec_from_file_location("patch_lvgl_scroll_uaf", script)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
source = open(source_path, encoding="utf-8").read()
patched, changed = patch.patch_source(source)
stock = source if changed else source.replace(patch.NEW, patch.OLD, 1)
open(work + "/lv_obj_tree_stock.c", "w", encoding="utf-8").write(stock)
open(work + "/lv_obj_tree_patched.c", "w", encoding="utf-8").write(patched)
EOF

# Every LVGL source except lv_obj_tree.c, compiled in parallel (the paths are
# passed as a flat string, so the checkout path must not contain spaces).
mkdir -p "$work/obj"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
export CC_CMD="$cc -c ${flags[*]}" OBJ_DIR="$work/obj"
find "$lvgl/src" -name '*.c' ! -path '*/core/lv_obj_tree.c' -print0 |
  xargs -0 -P "$jobs" -I{} sh -c \
    '$CC_CMD "$1" -o "$OBJ_DIR/$(printf %s "$1" | cksum | cut -d" " -f1).o"' _ {}
"$cc" -c "${flags[@]}" "$here/repro.c" -o "$work/repro.o"
for v in stock patched; do
  # The copies live outside LVGL, so resolve their relative includes from core/.
  "$cc" -c "${flags[@]}" -I"$lvgl/src/core" "$work/lv_obj_tree_$v.c" -o "$work/tree_$v.o"
  "$cc" -fsanitize=address -g "$work/repro.o" "$work/tree_$v.o" "$work"/obj/*.o -o "$work/repro_$v"
done

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
fail=0
for mode in press release; do
  case "$mode" in
    press)   where="_lv_indev_scroll_handler" ;;
    release) where="_lv_indev_scroll_throw_handler" ;;
  esac
  if "$work/repro_stock" "$mode" > "$work/stock_$mode.txt" 2>&1; then
    echo "FAIL: stock LVGL + $mode did not trip AddressSanitizer"; cat "$work/stock_$mode.txt"; fail=1
  elif grep -q "heap-use-after-free" "$work/stock_$mode.txt" && grep -q "in $where" "$work/stock_$mode.txt"; then
    echo "ok: stock LVGL + $mode -> heap-use-after-free in $where (the bug)"
  else
    echo "FAIL: stock LVGL + $mode failed for another reason"; cat "$work/stock_$mode.txt"; fail=1
  fi
  if out="$("$work/repro_patched" "$mode" 2>&1)"; then
    echo "ok: patched LVGL + $mode -> ${out#ok: }"
  else
    echo "FAIL: patched LVGL + $mode"; echo "$out"; fail=1
  fi
done
exit "$fail"
