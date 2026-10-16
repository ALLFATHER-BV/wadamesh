#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LVGL="${LVGL_SRC:-$ROOT/.pio/libdeps/LilyGo_TDeck_companion_radio_touch/lvgl}"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/wada-link-highlight.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
FLAGS=(-DLV_CONF_SKIP -DLV_USE_BIDI=1 -DLV_USE_ARABIC_PERSIAN_CHARS=1 -DLV_MEM_CUSTOM=1 -DLV_COLOR_DEPTH=32 -I "$LVGL")
awk '
  /^static bool chatCoordDigit/ {coord=1}
  /^static bool chatFirstCoord/ {coord=0}
  /^static bool urlCiHas/ {url=1}
  /^static bool chatFirstUrl/ {url=0}
  /^static bool chatHashtagSpan/ {tag=1}
  /^\/\/ ---- QR popup/ {tag=0}
  /^enum ChatLinkKind/ {print}
  /^struct ChatLinkChoice/ {render=1}
  /^static constexpr int CHAT_LINK_CHOICE_MAX/ {render=0}
  coord || url || tag || render {print}
' "$ROOT/src/ui-touch/UITask.cpp" > "$OUT/implementation.inc"
cd "$OUT"
find "$LVGL/src" -name '*.c' -print0 | xargs -0 -n 40 "${CC:-cc}" -w -c "${FLAGS[@]}"
ar rcs liblvgl.a ./*.o
"${CXX:-c++}" -std=c++17 "${FLAGS[@]}" -I "$ROOT/src" -I "$OUT" \
  "$ROOT/test/chat_link_highlight/test.cpp" liblvgl.a -o test-links
./test-links