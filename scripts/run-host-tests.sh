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
cd "$ROOT"
CXX="${CXX:-g++}"
STD="-std=c++17"
OUT="${TMPDIR:-/tmp}/wada-host-tests"
mkdir -p "$OUT"

# Tests needing more than `-I src`. Anything not listed builds with the default.
extra_flags() {
  case "$1" in
    # Its subject is a board variant header, not shared src/.
    test_crowpanel_sd_codec) echo "-I variants/crowpanel_35" ;;
    *) echo "" ;;
  esac
}

# Not unit tests, with the reason. Kept visible rather than deleted: a silent
# skip list is how a suite quietly stops covering things.
skip_reason() {
  case "$1" in
    test_minimp3_decoder)
      echo "takes an .mp3 path on argv; it is a decoder harness, not a unit test" ;;
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
  if ! $CXX $STD -I src $(extra_flags "$n") -o "$OUT/$n" "$t" > "$OUT/$n.build" 2>&1; then
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
