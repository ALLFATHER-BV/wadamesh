#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host regression test for pending lv_obj_del_async() calls (see repro.c).
# Builds LVGL 8.4 with AddressSanitizer three times - stock, with
# scripts/build/patch_lvgl_async_del.py applied, and with only its lv_obj_tree.c
# half - and checks:
#   stock    + parent / twice -> heap-use-after-free (proves the test sees the bug)
#   stock    + plain          -> clean (the normal case works today)
#   patched  + all three      -> clean, every queued object deleted exactly once
#   treeonly + plain          -> double free (why the lv_async.c half exists)
#
# Usage: test/lvgl_async_del_uaf/run.sh [path/to/lvgl]
# Defaults to the LVGL that PlatformIO fetched for any env (run a pio build first).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lvgl="${1:-}"
if [ -z "$lvgl" ]; then
  for d in "$root"/.pio/libdeps/*/lvgl; do
    [ -f "$d/src/misc/lv_async.c" ] && { lvgl="$d"; break; }
  done
fi
[ -n "$lvgl" ] && [ -f "$lvgl/src/misc/lv_async.c" ] && [ -f "$lvgl/src/core/lv_obj_tree.c" ] || {
  echo "LVGL sources not found; run a PlatformIO build first or pass the lvgl path" >&2
  exit 2
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-clang}"
flags=(-g -O1 -w -fsanitize=address -DLV_CONF_INCLUDE_SIMPLE -I"$here" -I"$lvgl")

# Stock and patched copies of both halves (the checkout may already be patched).
python3 - "$root/scripts/build/patch_lvgl_async_del.py" "$lvgl" "$work" <<'EOF'
import importlib.util, os, sys
script, lvgl, work = sys.argv[1:]
spec = importlib.util.spec_from_file_location("patch_lvgl_async_del", script)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
for half, stem in ((patch.ASYNC, "async"), (patch.OBJ_TREE, "tree")):
    source = open(os.path.join(lvgl, half["path"]), encoding="utf-8").read()
    patched, changed = patch.patch_source(half, source)
    stock = source if changed else source.replace(half["new"], half["old"], 1)
    open(work + "/%s_stock.c" % stem, "w", encoding="utf-8").write(stock)
    open(work + "/%s_patched.c" % stem, "w", encoding="utf-8").write(patched)
EOF

# Every other LVGL source, compiled in parallel (the paths are passed as a flat
# string, so the checkout path must not contain spaces).
mkdir -p "$work/obj"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
export CC_CMD="$cc -c ${flags[*]}" OBJ_DIR="$work/obj"
find "$lvgl/src" -name '*.c' ! -path '*/misc/lv_async.c' ! -path '*/core/lv_obj_tree.c' -print0 |
  xargs -0 -P "$jobs" -I{} sh -c \
    '$CC_CMD "$1" -o "$OBJ_DIR/$(printf %s "$1" | cksum | cut -d" " -f1).o"' _ {}
"$cc" -c "${flags[@]}" "$here/repro.c" -o "$work/repro.o"
for v in stock patched; do
  # The copies live outside LVGL, so resolve their relative includes from there.
  "$cc" -c "${flags[@]}" -I"$lvgl/src/misc" "$work/async_$v.c" -o "$work/async_$v.o"
  "$cc" -c "${flags[@]}" -I"$lvgl/src/core" "$work/tree_$v.c" -o "$work/tree_$v.o"
done
link() { "$cc" -fsanitize=address -g "$work/repro.o" "$work/async_$2.o" "$work/tree_$3.o" "$work"/obj/*.o -o "$work/repro_$1"; }
link stock stock stock
link patched patched patched
link treeonly stock patched

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
fail=0
# expect <variant> <mode> <asan error or "clean"> [frame the report must name]
expect() {
  local out="$work/$1_$2.txt"
  if "$work/repro_$1" "$2" > "$out" 2>&1; then
    if [ "$3" = clean ]; then echo "ok: $1 LVGL + $2 -> $(sed -n 's/^ok: //p' "$out")"
    else echo "FAIL: $1 LVGL + $2 did not trip AddressSanitizer"; cat "$out"; fail=1; fi
  elif [ "$3" != clean ] && grep -q "AddressSanitizer: $3" "$out" && grep -q "in $4" "$out"; then
    echo "ok: $1 LVGL + $2 -> $3 in $4"
  else
    echo "FAIL: $1 LVGL + $2"; cat "$out"; fail=1
  fi
}
expect stock    parent heap-use-after-free lv_async_timer_cb
expect stock    twice  heap-use-after-free lv_async_timer_cb
expect stock    plain  clean
expect patched  parent clean
expect patched  twice  clean
expect patched  plain  clean
expect treeonly plain  attempting\ double-free lv_async_timer_cb
exit "$fail"
