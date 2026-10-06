#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 [--check]" >&2
}

mode=write
if [[ ${1:-} == "--check" ]]; then
  mode=check
  shift
fi
if [[ $# -ne 0 ]]; then
  usage
  exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/wadamesh-touch-fonts.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

download() {
  local url=$1
  local output=$2
  local expected=$3

  curl -fsSL --retry 3 "$url" -o "$output"
  verify_sha256 "$output" "$expected"
}

verify_sha256() {
  local path=$1
  local expected=$2
  local actual

  if command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$path" | awk '{print $1}')
  else
    actual=$(sha256sum "$path" | awk '{print $1}')
  fi

  if [[ $actual != "$expected" ]]; then
    echo "SHA-256 mismatch for $path" >&2
    echo "expected: $expected" >&2
    echo "actual:   $actual" >&2
    exit 1
  fi
}

montserrat="$work_dir/Montserrat-Medium.ttf"
# Light weight, for the large screensaver / lock-screen clock only. LVGL ships just
# Medium, so this one comes from the Montserrat project itself, pinned to a TAG
# (not master) so the bytes and therefore the generated font cannot drift.
montserrat_light="$work_dir/Montserrat-Light.ttf"
# SemiBold for names, titles and figures, Bold for the small count badges: the
# weight steps the redesigned UI leans on, from the same pinned release.
montserrat_semibold="$work_dir/Montserrat-SemiBold.ttf"
montserrat_bold="$work_dir/Montserrat-Bold.ttf"
# Lucide line icons (ISC, plus MIT for the icons it took from Feather). They
# replace the FontAwesome symbols LVGL bakes into its Montserrat, see
# generate_ui below and scripts/build/lucide_font.py.
lucide_tgz="$work_dir/lucide-static-1.52.0.tgz"
lucide="$work_dir/lucide.ttf"
lucide_cp="$work_dir/lucide-codepoints.json"
lucide_py="$repo_root/scripts/build/lucide_font.py"
noto_sans_zip="$work_dir/NotoSans-v2.015.zip"
noto_sans="$work_dir/NotoSans-Regular.ttf"
noto_arabic_zip="$work_dir/NotoSansArabicUI-v2.011.zip"
noto_arabic="$work_dir/NotoSansArabicUI-Regular.ttf"
noto_symbols_zip="$work_dir/NotoSansSymbols2-v2.008.zip"
noto_symbols="$work_dir/NotoSansSymbols2-Regular.ttf"

download \
  "https://raw.githubusercontent.com/lvgl/lvgl/v8.4.0/scripts/built_in_font/Montserrat-Medium.ttf" \
  "$montserrat" \
  "421f26b23e2be6b98373d32acd3cb2897b154d4bf0a77d26534ce476e4cbed53"
download \
  "https://raw.githubusercontent.com/JulietaUla/Montserrat/v7.222/fonts/ttf/Montserrat-Light.ttf" \
  "$montserrat_light" \
  "558059a690df85c773135e5f58582897238d595cdec7f86765e6db10df368937"
download \
  "https://raw.githubusercontent.com/JulietaUla/Montserrat/v7.222/fonts/ttf/Montserrat-SemiBold.ttf" \
  "$montserrat_semibold" \
  "49fbfce003ad1692d7c9a6502791577088c12c50088d4caa27dbbfe540ad9d13"
download \
  "https://raw.githubusercontent.com/JulietaUla/Montserrat/v7.222/fonts/ttf/Montserrat-Bold.ttf" \
  "$montserrat_bold" \
  "4e6d93bc38122c371acb8dc0dbefbf2649c235191e1be136bb5720546d719808"
download \
  "https://registry.npmjs.org/lucide-static/-/lucide-static-1.52.0.tgz" \
  "$lucide_tgz" \
  "4d2aa079171e4eba4c704844b8b0c9edb6e4cfb2fb8bd86f7b14353994f04a87"
tar -xzf "$lucide_tgz" -C "$work_dir" package/font/lucide.ttf package/font/codepoints.json
mv "$work_dir/package/font/lucide.ttf" "$lucide"
mv "$work_dir/package/font/codepoints.json" "$lucide_cp"
verify_sha256 "$lucide" \
  "124ecc64b91a158fc519eefdaaead89a2d9e2c39b6f9c8429c5584c93a0be451"
download \
  "https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSans-v2.015/NotoSans-v2.015.zip" \
  "$noto_sans_zip" \
  "0c34df072a3fa7efbb7cbf34950e1f971a4447cffe365d3a359e2d4089b958f5"
download \
  "https://github.com/notofonts/arabic/releases/download/NotoSansArabicUI-v2.011/NotoSansArabicUI-v2.011.zip" \
  "$noto_arabic_zip" \
  "f775ee259557195721466e99ec0316bbcd897f0429e25955f5d9582ca1202ce7"
download \
  "https://github.com/notofonts/symbols/releases/download/NotoSansSymbols2-v2.008/NotoSansSymbols2-v2.008.zip" \
  "$noto_symbols_zip" \
  "346c930bbe8eb946701a05c54e9c11a2094dee1d93c387bf1771c0a3e335688f"

unzip -p "$noto_sans_zip" \
  "NotoSans/hinted/ttf/NotoSans-Regular.ttf" > "$noto_sans"
unzip -p "$noto_arabic_zip" \
  "NotoSansArabicUI/hinted/ttf/NotoSansArabicUI-Regular.ttf" > "$noto_arabic"
unzip -p "$noto_symbols_zip" \
  "NotoSansSymbols2/hinted/ttf/NotoSansSymbols2-Regular.ttf" > "$noto_symbols"

verify_sha256 "$noto_sans" \
  "478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823"
verify_sha256 "$noto_arabic" \
  "c56275c744ded6ff6df13de04963e6174632f0405a54a83f44d0fe5395f45ae6"
verify_sha256 "$noto_symbols" \
  "c4a0a80f0041ce4be81e2478faad22776d23edb98ae3f0d19bd37044820ecf9d"

font_conv=(npx --yes lv_font_conv@1.5.3)
if [[ $("${font_conv[@]}" --version) != "1.5.3" ]]; then
  echo "lv_font_conv 1.5.3 is required" >&2
  exit 1
fi

symbols='•·–—‘’„“”…°±×÷€£¥§©®™½¼¾℃℉'
# Arrows (U+2190-2193) and the comparison operators (U+2260/2264/2265) are NOT in
# Noto Sans. Asking $noto_sans for them, as this script did until #261, silently
# produced nothing — lv_font_conv omits a glyph the source font lacks instead of
# failing — so "Settings → Quick replies" drew a tofu box in EVERY language, at
# 137 sites in the UI and in all 13 .lang files. (Noto Sans Symbols 2 does not
# have them either; verified, it errors outright when asked.)
#
# Montserrat DOES have all seven, and is already the primary UI face and the first
# --font here, so cutting them from it costs no new dependency or licence line and
# makes the arrow match the text around it. lv_font_conv takes the FIRST font that
# supplies a codepoint, so listing these on Montserrat wins while every other
# symbol still comes from Noto Sans below, unchanged.
symbols_fallback='→←↑↓≠≤≥'
stage="$work_dir/stage"
mkdir -p "$stage/src/ui-touch"

normalise() {
  local raw=$1
  local output=$2
  local sources=$3
  local guard_expr=${4:-}
  local guard_label=${5:-$guard_expr}
  local include_caps=${6:-0}
  local note=${7:-}

  awk -v sources="$sources" -v guard_expr="$guard_expr" \
      -v guard_label="$guard_label" -v include_caps="$include_caps" -v note="$note" '
    BEGIN {
      if (include_caps == "1") {
        print "#include \"device_caps.h\""
      }
      if (note != "") {
        count = split(note, lines, "|")
        for (i = 1; i <= count; ++i) print lines[i]
      }
      if (guard_expr != "") {
        print "#if " guard_expr
        print ""
      } else if (include_caps == "1" || note != "") {
        print ""
      }
    }
    /^ \* Opts:/ {
      print " * Generated by scripts/build/gen-touch-fonts.sh with lv_font_conv 1.5.3"
      print " * Sources: " sources
      next
    }
    /^#ifdef LV_LVGL_H_INCLUDE_SIMPLE$/ {
      print "#if 1"
      next
    }
    { print }
    END {
      if (guard_expr != "") {
        print ""
        print "#endif /* " guard_label " */"
      }
    }
  ' "$raw" > "$output"
  perl -0pi -e 's/\s+\z/\n/' "$output"
}

generate_extras() {
  local size=$1
  local guard_expr=${2:-}
  local guard_label=${3:-$guard_expr}
  local raw="$work_dir/extras_font_${size}.c"
  local output="$stage/src/ui-touch/extras_font_${size}.c"

  "${font_conv[@]}" \
    --size "$size" \
    --bpp 4 \
    --format lvgl \
    --font "$montserrat" \
    -r 0x00C0-0x00FF \
    -r 0x0100-0x017F \
    -r 0x0400-0x04FF \
    --symbols "$symbols_fallback" \
    --font "$noto_sans" \
    -r 0x0370-0x03FF \
    --symbols "$symbols" \
    --font "$noto_arabic" \
    -r 0x0600-0x06FF \
    -r 0xFE70-0xFEFF \
    --lv-font-name "extras_${size}" \
    -o "$raw"

  normalise "$raw" "$output" \
    "Montserrat Medium (LVGL v8.4.0), Noto Sans 2.015, Noto Sans Symbols 2.008, Noto Sans Arabic UI 2.011" \
    "$guard_expr" "$guard_label" "$([[ -n $guard_expr ]] && echo 1 || echo 0)"
}

generate_latin_extras() {
  local size=$1
  local guard_expr=${2:-}
  local guard_label=${3:-$guard_expr}
  local include_caps=${4:-0}
  local note=${5:-}
  local raw="$work_dir/extras_lat_${size}.c"
  local output="$stage/src/ui-touch/extras_lat_${size}.c"

  "${font_conv[@]}" \
    --size "$size" \
    --bpp 4 \
    --format lvgl \
    --font "$montserrat" \
    -r 0x00C0-0x00FF \
    -r 0x0100-0x017F \
    --symbols "„" \
    --lv-font-name "extras_lat_${size}" \
    -o "$raw"

  normalise "$raw" "$output" \
    "Montserrat Medium (LVGL v8.4.0)" \
    "$guard_expr" "$guard_label" "$include_caps" "$note"
}

generate_star() {
  local size=$1
  local raw="$work_dir/star_font_${size}.c"
  local output="$stage/src/ui-touch/star_font_${size}.c"

  "${font_conv[@]}" \
    --size "$size" \
    --bpp 4 \
    --no-compress \
    --format lvgl \
    --font "$noto_symbols" \
    -r 0x2605 \
    --lv-font-name "star_font_${size}" \
    -o "$raw"

  normalise "$raw" "$output" \
    "Noto Sans Symbols 2.008"
}

# The ambient clock: digits and the colon only, in Montserrat Light. Eleven glyphs
# is what keeps a 48 px face to a few KB -- the screensaver and lock screen need a
# large, quiet clock, and the biggest face every board already carries is 28 px
# Medium, which reads as a heading rather than a clock.
generate_clock() {
  local size=$1
  local guard_expr=${2:-}
  local guard_label=${3:-$guard_expr}
  local raw="$work_dir/clock_font_${size}.c"
  local output="$stage/src/ui-touch/clock_font_${size}.c"

  "${font_conv[@]}" \
    --size "$size" \
    --bpp 4 \
    --no-compress \
    --format lvgl \
    --font "$montserrat_light" \
    -r 0x30-0x3A \
    --lv-font-name "clock_font_${size}" \
    -o "$raw"

  normalise "$raw" "$output" \
    "Montserrat Light v7.222 (digits and colon)" \
    "$guard_expr" "$guard_label"
}

# The UI faces, under LVGL's own font names so the theme, every widget and the
# firmware pick them up unchanged: Montserrat Medium text, exactly as LVGL builds
# its built-in fonts, with Lucide line icons on the LV_SYMBOL_* codepoints where
# LVGL puts FontAwesome. include/lv_conf.h switches LVGL's copies off and says
# which sizes a board carries (WADA_UI_FONT_NN).
generate_ui() {
  local size=$1
  local guard_expr=${2:-}
  local guard_label=${3:-$guard_expr}
  local text_raw="$work_dir/ui_text_${size}.c"
  local raw="$work_dir/ui_font_${size}.c"
  local output="$stage/src/ui-touch/ui_font_${size}.c"
  local metrics icons arg
  local -a ranges

  # Text alone first, for the line metrics the icons must not change.
  "${font_conv[@]}" --no-compress --no-prefilter --bpp 4 --size "$size" --format lvgl \
    --font "$montserrat" -r 0x20-0x7F,0xB0,0x2022 \
    --lv-font-name "lv_font_montserrat_${size}" -o "$text_raw"
  metrics=$(python3 "$lucide_py" metrics "$text_raw")
  ranges=()
  while IFS= read -r arg; do ranges+=("$arg"); done < <(python3 "$lucide_py" ranges "$lucide_cp" lv_symbols)
  icons=$(python3 "$lucide_py" count "$lucide_cp" lv_symbols)
  "${font_conv[@]}" --no-compress --no-prefilter --bpp 4 --size "$size" --format lvgl \
    --font "$montserrat" -r 0x20-0x7F,0xB0,0x2022 \
    --font "$lucide" "${ranges[@]}" \
    --lv-font-name "lv_font_montserrat_${size}" -o "$raw"
  # shellcheck disable=SC2086
  python3 "$lucide_py" patch "$raw" "$icons" $metrics
  # lv_font_conv guards the body with the font's own macro, LV_FONT_MONTSERRAT_NN,
  # which is LVGL's switch for its own copy and stays off, so the body would
  # compile to nothing. Which boards carry a size is the board guard below.
  perl -pi -e "s/\\bLV_FONT_MONTSERRAT_${size}\\b/WADA_UI_FONT_${size}_BODY/g" "$raw"

  normalise "$raw" "$output" \
    "Montserrat Medium (LVGL v8.4.0), Lucide 1.52.0 on the LV_SYMBOL codepoints" \
    "$guard_expr" "$guard_label"
}

# A heavier cut for names, titles and figures. Latin with the accents European
# names use; anything else falls through to the Medium chain at run time.
generate_semibold() {
  local size=$1
  local raw="$work_dir/ui_semibold_${size}.c"
  local output="$stage/src/ui-touch/ui_semibold_${size}.c"

  "${font_conv[@]}" --bpp 4 --size "$size" --format lvgl \
    --font "$montserrat_semibold" -r 0x20-0x7E,0xA0-0x17F,0x2022,0x2026 \
    --lv-font-name "ui_semibold_${size}" -o "$raw"
  normalise "$raw" "$output" "Montserrat SemiBold v7.222"
}

# Count badges: digits, "+" and "!" in Bold, at the one size the pills use.
generate_badge() {
  local size=$1
  local raw="$work_dir/ui_badge_${size}.c"
  local output="$stage/src/ui-touch/ui_badge_${size}.c"

  "${font_conv[@]}" --no-compress --bpp 4 --size "$size" --format lvgl \
    --font "$montserrat_bold" -r 0x20-0x21,0x2B,0x30-0x39 \
    --lv-font-name "ui_badge_${size}" -o "$raw"
  normalise "$raw" "$output" "Montserrat Bold v7.222 (digits, + and !)"
}

# App and launcher icons on Lucide's own codepoints (names in ui_icons.h).
generate_icons() {
  local size=$1
  local guard_expr=${2:-}
  local guard_label=${3:-$guard_expr}
  local raw="$work_dir/ui_icons_${size}.c"
  local output="$stage/src/ui-touch/ui_icons_${size}.c"
  local text_raw="$work_dir/ui_icons_${size}_text.c"
  local metrics icons arg
  local -a ranges

  # The text these icons sit beside or in place of: its line box, so an icon
  # label lines up with a text label, and its capitals, to centre the icons on.
  "${font_conv[@]}" --no-compress --no-prefilter --bpp 4 --size "$size" --format lvgl \
    --font "$montserrat" -r 0x20-0x7F --lv-font-name "ui_icons_${size}_text" -o "$text_raw"
  metrics=$(python3 "$lucide_py" metrics "$text_raw")
  ranges=()
  while IFS= read -r arg; do ranges+=("$arg"); done < <(python3 "$lucide_py" ranges "$lucide_cp" app)
  icons=$(python3 "$lucide_py" count "$lucide_cp" app)
  "${font_conv[@]}" --bpp 4 --size "$size" --format lvgl \
    --font "$lucide" "${ranges[@]}" \
    --lv-font-name "ui_icons_${size}" -o "$raw"
  # shellcheck disable=SC2086
  python3 "$lucide_py" patch "$raw" "$icons" $metrics
  normalise "$raw" "$output" "Lucide 1.52.0 (app icons)" "$guard_expr" "$guard_label"
}

# The small fallback icon fonts the UI chains behind the text (TOUCH_SYM_* in
# UITask.cpp): same names and codepoints as the FontAwesome cuts they replace.
generate_fallback_icons() {
  local name=$1
  local size=$2
  local set=$3
  local raw="$work_dir/${name}.c"
  local output="$stage/src/ui-touch/${name}.c"
  local text_raw="$work_dir/${name}_text.c"
  local metrics icons arg
  local -a ranges

  "${font_conv[@]}" --no-compress --no-prefilter --bpp 4 --size "$size" --format lvgl \
    --font "$montserrat" -r 0x41 --lv-font-name "${name}_text" -o "$text_raw"
  metrics=$(python3 "$lucide_py" metrics "$text_raw")
  ranges=()
  while IFS= read -r arg; do ranges+=("$arg"); done < <(python3 "$lucide_py" ranges "$lucide_cp" "$set")
  icons=$(python3 "$lucide_py" count "$lucide_cp" "$set")
  "${font_conv[@]}" --no-compress --bpp 4 --size "$size" --format lvgl \
    --font "$lucide" "${ranges[@]}" \
    --lv-font-name "$name" -o "$raw"
  # shellcheck disable=SC2086
  python3 "$lucide_py" patch "$raw" "$icons" $metrics
  normalise "$raw" "$output" "Lucide 1.52.0 (${set} icons)"
}

# Which boards carry which size is decided here, on board macros, and not in
# lv_conf.h: src/ compiles resolve "lv_conf.h" to a stale copy vendored with the
# core (see the T-Deck env in platformio.ini), so a switch there would be seen by
# some translation units and not others. 18/20/24 are the bigger UI-size presets
# (every board with a text-size setting; the V4 has none, its flash is full).
# DOC_CAPTURE builds carry them all: they render other boards' sizes and scales for review.
ui_big="defined(HAS_TANMATSU) || defined(HAS_TDISPLAY_P4) || defined(HELTEC_LORA_V4_R8) || defined(TLORA_PAGER) || defined(HAS_THINKNODE_M9) || defined(HAS_TDECK_GT911) || defined(DOC_CAPTURE)"
for size in 12 14 16 28; do
  generate_ui "$size"
done
generate_ui 18 "$ui_big"
generate_ui 20 "$ui_big"
generate_ui 24 "$ui_big"
for size in 12 14 16; do
  generate_semibold "$size"
done
generate_badge 11
generate_icons 16
generate_icons 20
python3 "$lucide_py" header "$lucide_cp" > "$stage/src/ui-touch/ui_icons.h"
generate_fallback_icons person_font 16 person
generate_fallback_icons person_font14 14 person
generate_fallback_icons zoom_font 16 zoom
generate_fallback_icons sleepicons_font 16 sleep
generate_fallback_icons cc_icons_16 16 cc

for size in 12 14 16; do
  generate_extras "$size"
done
for size in 20 24; do
  generate_extras "$size" "defined(TLORA_PAGER)" "TLORA_PAGER"
done
# At-a-glance uses 20 px on T-Deck/M9 and 28 px across boards; the 24 px face
# remains the Tanmatsu Large/Huge UI fallback.
generate_latin_extras 20 \
  "defined(HAS_TANMATSU) || defined(HAS_TDECK_GT911) || defined(HAS_THINKNODE_M9)" \
  "HAS_TANMATSU || HAS_TDECK_GT911 || HAS_THINKNODE_M9" 1 \
  "// Was HAS_TANMATSU-only (Large/Huge UI-scale accented-Latin fallback); the T-Deck|// now also builds this for the \"at a glance\" notification's 20 px message body|// (see atGlanceEnsureFont() in UITask.cpp) -- an experiment to see whether a|// smaller-than-28px glance body is still legible on that panel."
generate_latin_extras 24 "defined(HAS_TANMATSU)" "HAS_TANMATSU" 1
generate_latin_extras 28 "" "" 1 \
  "// Was HAS_TANMATSU-only (only consumer used to be the Tanmatsu's Large/Huge UI-scale|// accented-Latin fallback); now compiled on every board too for the \"at a glance\"|// notification's 28 px message body (see atGlanceEnsureFont() in UITask.cpp), which|// needs accented Latin / em-dash / ellipsis glyph coverage at that size on any board."
for size in 14 28; do
  generate_star "$size"
done
# 40 px for the screensaver and 48 px for the lock screen, everywhere (the P4
# renders at half its panel resolution, so these are twice that in physical
# pixels there); the Tanmatsu's 800x480 logical screen needs the 96 px face.
generate_clock 40
generate_clock 48
generate_clock 96 "defined(HAS_TANMATSU)" "HAS_TANMATSU"

files=(
  src/ui-touch/ui_font_12.c
  src/ui-touch/ui_font_14.c
  src/ui-touch/ui_font_16.c
  src/ui-touch/ui_font_18.c
  src/ui-touch/ui_font_20.c
  src/ui-touch/ui_font_24.c
  src/ui-touch/ui_font_28.c
  src/ui-touch/ui_semibold_12.c
  src/ui-touch/ui_semibold_14.c
  src/ui-touch/ui_semibold_16.c
  src/ui-touch/ui_badge_11.c
  src/ui-touch/ui_icons_16.c
  src/ui-touch/ui_icons_20.c
  src/ui-touch/ui_icons.h
  src/ui-touch/person_font.c
  src/ui-touch/person_font14.c
  src/ui-touch/zoom_font.c
  src/ui-touch/sleepicons_font.c
  src/ui-touch/cc_icons_16.c
  src/ui-touch/extras_font_12.c
  src/ui-touch/extras_font_14.c
  src/ui-touch/extras_font_16.c
  src/ui-touch/extras_font_20.c
  src/ui-touch/extras_font_24.c
  src/ui-touch/extras_lat_20.c
  src/ui-touch/extras_lat_24.c
  src/ui-touch/extras_lat_28.c
  src/ui-touch/star_font_14.c
  src/ui-touch/star_font_28.c
  src/ui-touch/clock_font_40.c
  src/ui-touch/clock_font_48.c
  src/ui-touch/clock_font_96.c
)

if [[ $mode == check ]]; then
  stale=0
  for file in "${files[@]}"; do
    if ! cmp -s "$stage/$file" "$repo_root/$file"; then
      echo "stale generated font: $file" >&2
      stale=1
    fi
  done
  exit "$stale"
fi

for file in "${files[@]}"; do
  cp "$stage/$file" "$repo_root/$file"
done

echo "Regenerated ${#files[@]} touch font assets."
