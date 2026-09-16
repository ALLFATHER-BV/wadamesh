#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host regression test for the #428/#475 chat-scroll panic (see repro.c).
# Builds LVGL 8.4 with AddressSanitizer twice, once with stock lv_anim.c and
# once with scripts/build/patch_lvgl_anim_uaf.py applied, and checks:
#   stock   + plain handler  -> heap-use-after-free (proves the test sees the bug)
#   stock   + guard handler  -> clean (the UITask.cpp fix on its own)
#   patched + either handler -> clean (the LVGL fix on its own)
#
# Usage: test/lvgl_anim_uaf/run.sh [path/to/lvgl]
# Defaults to the LVGL that PlatformIO fetched for any env (run a pio build first).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lvgl="${1:-}"
if [ -z "$lvgl" ]; then
  for d in "$root"/.pio/libdeps/*/lvgl; do
    [ -f "$d/src/misc/lv_anim.c" ] && { lvgl="$d"; break; }
  done
fi
[ -n "$lvgl" ] && [ -f "$lvgl/src/misc/lv_anim.c" ] || {
  echo "LVGL sources not found; run a PlatformIO build first or pass the lvgl path" >&2
  exit 2
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-clang}"
flags=(-g -O1 -w -fsanitize=address -DLV_CONF_INCLUDE_SIMPLE -I"$here" -I"$lvgl")

python3 - "$root/scripts/build/patch_lvgl_anim_uaf.py" "$lvgl/src/misc/lv_anim.c" "$work" <<'EOF'
import importlib.util, sys
script, source_path, work = sys.argv[1:]
spec = importlib.util.spec_from_file_location("patch_lvgl_anim_uaf", script)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
source = open(source_path, encoding="utf-8").read()
patched, changed = patch.patch_source(source)
stock = source if changed else source.replace(patch.NEW, patch.OLD, 1)
open(work + "/lv_anim_stock.c", "w", encoding="utf-8").write(stock)
open(work + "/lv_anim_patched.c", "w", encoding="utf-8").write(patched)
EOF

# Every LVGL source except lv_anim.c, compiled in parallel (the paths are passed
# as a flat string, so the checkout path must not contain spaces).
mkdir -p "$work/obj"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
export CC_CMD="$cc -c ${flags[*]}" OBJ_DIR="$work/obj"
find "$lvgl/src" -name '*.c' ! -path '*/misc/lv_anim.c' -print0 |
  xargs -0 -P "$jobs" -I{} sh -c \
    '$CC_CMD "$1" -o "$OBJ_DIR/$(printf %s "$1" | cksum | cut -d" " -f1).o"' _ {}
"$cc" -c "${flags[@]}" "$here/repro.c" -o "$work/repro.o"
for v in stock patched; do
  # The copies live outside LVGL, so resolve their relative includes from misc/.
  "$cc" -c "${flags[@]}" -I"$lvgl/src/misc" "$work/lv_anim_$v.c" -o "$work/anim_$v.o"
  "$cc" -fsanitize=address -g "$work/repro.o" "$work/anim_$v.o" "$work"/obj/*.o -o "$work/repro_$v"
done

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
fail=0
if "$work/repro_stock" plain > "$work/stock_plain.txt" 2>&1; then
  echo "FAIL: stock LVGL + plain handler did not trip AddressSanitizer"; fail=1
elif grep -q "heap-use-after-free" "$work/stock_plain.txt" && grep -q "in anim_timer" "$work/stock_plain.txt"; then
  echo "ok: stock LVGL + plain handler -> heap-use-after-free in anim_timer (the bug)"
else
  echo "FAIL: stock LVGL + plain handler failed for another reason"; cat "$work/stock_plain.txt"; fail=1
fi
for run in "stock guard" "patched plain" "patched guard"; do
  set -- $run
  if out="$("$work/repro_$1" "$2" 2>&1)"; then
    echo "ok: $1 LVGL + $2 handler -> ${out#ok: }"
  else
    echo "FAIL: $1 LVGL + $2 handler"; echo "$out"; fail=1
  fi
done
exit "$fail"
