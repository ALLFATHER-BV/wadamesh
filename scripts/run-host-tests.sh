#!/usr/bin/env bash
# Build and run every host test in test/.
#
# These are plain g++ binaries, no framework: each file documents its own build
# line in a comment and is compiled standalone. That is cheap and fast, but it
# also means nothing ever ran them all together -- so test_touch_prefs_schema
# sat broken for two schema versions (v64 appended two fields and its sums were
# not updated) without anyone noticing. A static_assert that does not compile is
# not a guard, and that particular one exists to catch the mid-struct insert
# that poisoned beta_44 and beta_57. One command, run it before a release.
#
# Usage: scripts/run-host-tests.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
CXX="${CXX:-g++}"
STD="-std=c++17"
OUT="${TMPDIR:-/tmp}/wada-host-tests"
mkdir -p "$OUT"

# Tests needing more than `-I src`. Anything not listed builds with the default.
extra_flags() {
  case "$1" in
    # Its subject is a board variant header, not shared src/.
    test_crowpanel_sd_codec) echo "-I variants/crowpanel_35" ;;
    test_m9_keyboard) echo "-I test/m9_keyboard_fakes" ;;
    test_time_zones) echo "-I test/clock_floor_fakes" ;;
    # Its subject lives in the T-Display P4 IDF component, not in src/.
    test_c6_link_health) echo "-I tdisplay_p4/components/c6_at" ;;
    # The T-LoRa Pager chip drivers, compiled from variants/ against fake chips.
    test_pager_power|test_pager_haptics|test_pager_imu) echo "-I test/pager_hw_fakes" ;;
    test_indicator_expander|test_indicator_dio1) echo "-I variants/sensecap_indicator" ;;
    # The reader is a .cpp of its own; tinfl comes from the web build's miniz.
    test_pmtiles) echo "-I src/ui-touch -I web/third_party/miniz src/ui-touch/PmTiles.cpp web/third_party/miniz/miniz_tinfl.c" ;;
    test_emoji_deflated) echo "-I test/emoji_fakes -I $OUT/gen -I src/ui-touch" ;;
    test_tdeck_keyboard_detect) echo "-I test/tdeck_keyboard_fakes" ;;
    test_keyboard_layouts_hw) echo "-I test/kb_layouts_fakes" ;;   # LVGL names only
    test_clock_floor) echo "-I test/clock_hw_fakes" ;;   # a settable hardware clock
    test_p4_haptic) echo "-I test/p4_haptic_fakes" ;;       # an AW86224 register model
    # The real WadaNmeaLocationProvider.h over a fake UART / MicroNMEA; the prefs
    # header it includes only declares its functions under ESP32.
    test_gps_baud_probe) echo "-DESP32 -I test/gps_probe_fakes" ;;
    *) echo "" ;;
  esac
}

# Libraries a test links, placed after its source so GNU ld resolves them too.
link_flags() {
  case "$1" in
    test_emoji_deflated) echo "-lz" ;;
    *) echo "" ;;
  esac
}

# Generated inputs a test compiles against (written under $OUT, never into the tree).
prep() {
  case "$1" in
    test_emoji_deflated)
      mkdir -p "$OUT/gen" &&
        python3 scripts/build/gen-emoji-deflated.py src/ui-touch/emoji_data.c "$OUT/gen/emoji_deflated.h" ;;
    *) true ;;
  esac
}

# Not unit tests, with the reason. Kept visible rather than deleted: a silent
# skip list is how a suite quietly stops covering things.
skip_reason() {
  case "$1" in
    test_minimp3_decoder)
      echo "takes an .mp3 path on argv; it is a decoder harness, not a unit test" ;;
    test_pmtiles)
      # Only a fresh checkout lacks it; web/build.sh (or web/fetch-deps.sh) fetches it.
      [ -f web/third_party/miniz/miniz_tinfl.c ] || echo "needs web/third_party/miniz: run web/fetch-deps.sh" ;;
    test_reader_content)
      # Fixing this means lifting ReaderContent out of UITask.cpp, which is a
      # refactor of a 60k-line file, not a test change. Left failing-by-design
      # and named, so it is a known gap rather than a mystery.
      echo "subject (ReaderContent::htmlToText) is defined inside UITask.cpp, which cannot build on the host" ;;
    *) echo "" ;;
  esac
}

pass=0; fail=0; skip=0; failed=""
for t in test/test_*.cpp; do
  n="$(basename "$t" .cpp)"
  reason="$(skip_reason "$n")"
  if [ -n "$reason" ]; then
    printf 'SKIP  %-34s %s\n' "$n" "$reason"; skip=$((skip+1)); continue
  fi
  # shellcheck disable=SC2046
  if ! { prep "$n" && $CXX $STD -I src $(extra_flags "$n") -o "$OUT/$n" "$t" $(link_flags "$n"); } \
       > "$OUT/$n.build" 2>&1; then
    printf 'BUILD %-34s %s\n' "$n" "see $OUT/$n.build"
    grep -m2 -E 'error:|Undefined' "$OUT/$n.build" | sed 's/^/        /'
    failed="$failed $n"; fail=$((fail+1)); continue
  fi
  if ! "$OUT/$n" > "$OUT/$n.run" 2>&1; then
    printf 'FAIL  %-34s %s\n' "$n" "see $OUT/$n.run"
    tail -3 "$OUT/$n.run" | sed 's/^/        /'
    failed="$failed $n"; fail=$((fail+1)); continue
  fi
  printf 'ok    %s\n' "$n"
  pass=$((pass+1))
done

echo
echo "pass=$pass fail=$fail skip=$skip"
[ "$fail" -eq 0 ] || { echo "failing:$failed"; exit 1; }
