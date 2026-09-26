#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TDECK_ENV="LilyGo_TDeck_companion_radio_touch"
TDECK_PRO_V1_0_ENV="LilyGo_TDeck_Pro_v1_0_companion_radio_touch"
TDECK_PRO_V1_1_ENV="LilyGo_TDeck_Pro_v1_1_companion_radio_touch"
TDECK_MAX_ENV="LilyGo_TDeck_Max_companion_radio_touch"
HELTEC_ENV="heltec_v4_tft_companion_radio_usb_tcp_touch"
HELTEC_R8_ENV="heltec_v4_r8_tft_companion_radio_usb_tcp_touch"
ATTAKY_ENV="attaky_mesh_series_companion_radio_touch"
WIO_ENV="wio_tracker_l2_companion_radio_touch"
PAGER_LR1121_ENV="tlora_pager_lr1121_companion_radio_touch"
PAGER_SX1262_ENV="tlora_pager_sx1262_companion_radio_touch"
M9_ENV="ThinkNode_M9_companion_radio_touch"
RAK_ENV="rak_tap_v2_companion_radio_touch"
# The T-Display P4 is not a PlatformIO env: it is a standalone ESP-IDF app built by
# tdisplay_p4/build.sh (which ends in `exec idf.py ...`). These two names are the
# script's own handles for its two panel SKUs; they never reach `pio`.
P4_ENV="tdisplay_p4"
P4_LCD_ENV="tdisplay_p4_lcd"
P4_BUILD="$ROOT/tdisplay_p4/build.sh"

PIO="${PIO:-$(command -v pio || true)}"
ENV_NAME=""
ENV_EXPLICIT=false
ERASE_FIRST=false
FULLCLEAN=false
JUST_BUILD=false

has_env() {
  is_p4_env "$1" && return 0
  grep -q "^\[env:$1\]$" platformio.ini
}

is_p4_env() {
  [ "$1" = "$P4_ENV" ] || [ "$1" = "$P4_LCD_ENV" ]
}

# The P4 reuses the Tanmatsu's project-local ESP-IDF (see tdisplay_p4/build.sh).
p4_toolchain_ready() {
  [ -f "$ROOT/tanmatsu/esp-idf/export.sh" ]
}

# run_p4 <env> <idf.py actions...>
# Runs tdisplay_p4/build.sh for the chosen SKU. `reconfigure` goes first because
# both SKUs share build/tdisplay_p4 and WADA_P4_LCD is only read at CMake configure
# time — without it, switching SKU would silently rebuild the previous panel.
# PORT=/dev/cu.usbmodemXXXX pins the serial port; otherwise idf.py auto-detects.
run_p4() {
  local env_name="$1"; shift
  local port_args=()
  [ -n "${PORT:-}" ] && port_args=(-p "$PORT")
  # build.sh handles a lone `fullclean` itself (removes only the generated tree): idf.py's
  # stock fullclean rejects the intentionally patched managed components and aborts.
  if [ "${1:-}" = "fullclean" ]; then
    "$P4_BUILD" fullclean || return 1
    shift
    [ $# -eq 0 ] && return 0
  fi
  echo "[IDF] $(env_label "$env_name"): $*"
  if [ "$env_name" = "$P4_LCD_ENV" ]; then
    WADA_P4_LCD=1 "$P4_BUILD" ${port_args+"${port_args[@]}"} reconfigure "$@"
  else
    env -u WADA_P4_LCD "$P4_BUILD" ${port_args+"${port_args[@]}"} reconfigure "$@"
  fi
}

all_envs() {
  sed -n 's/^\[env:\(.*\)\]$/\1/p' platformio.ini |
    grep -v '^LilyGo_TDeck_Pro_companion_radio_touch$'
}

env_label() {
  case "$1" in
    "$TDECK_ENV")        echo "LilyGo T-Deck" ;;
    "$TDECK_PRO_V1_0_ENV") echo "LilyGo T-Deck Pro V1.0" ;;
    "$TDECK_PRO_V1_1_ENV") echo "LilyGo T-Deck Pro V1.1" ;;
    "$TDECK_MAX_ENV")    echo "LilyGo T-Deck Max" ;;
    "$HELTEC_ENV")       echo "Heltec V4" ;;
    "$HELTEC_R8_ENV")    echo "Heltec V4-R8" ;;
    "$ATTAKY_ENV")       echo "Attaky Mesh Series" ;;
    "$WIO_ENV")          echo "Seeed Wio Tracker L2" ;;
    "$PAGER_LR1121_ENV") echo "LilyGo T-LoRa Pager LR1121" ;;
    "$PAGER_SX1262_ENV") echo "LilyGo T-LoRa Pager SX1262" ;;
    "$M9_ENV")           echo "ThinkNode M9" ;;
    "$RAK_ENV")          echo "RAK TAP V2" ;;
    "$P4_ENV")           echo "LilyGo T-Display P4 (AMOLED)" ;;
    "$P4_LCD_ENV")       echo "LilyGo T-Display P4 (LCD)" ;;
    *)                     echo "$1" ;;
  esac
}

show_usage() {
  cat <<EOF
Usage: $0 [device] [options]

Devices:
  --tdeck                 LilyGo T-Deck
  --tdeck-pro-v1-0        LilyGo T-Deck Pro V1.0
  --tdeck-pro-v1-1        LilyGo T-Deck Pro V1.1
  --tdeck-max             LilyGo T-Deck Max
  --heltec                Heltec V4
  --heltec-r8             Heltec V4-R8
  --attaky                Attaky Mesh Series
  --wio                   Seeed Wio Tracker L2
  --pager-lr1121          LilyGo T-LoRa Pager LR1121
  --pager-sx1262          LilyGo T-LoRa Pager SX1262
  --m9                    ThinkNode M9
  --rak                   RAK TAP V2
  --tdisplay-p4           LilyGo T-Display P4, AMOLED (ESP-IDF: tdisplay_p4/build.sh)
  --tdisplay-p4-lcd       LilyGo T-Display P4, HI8561 LCD SKU (ESP-IDF)

Options:
  --erase, -E             Erase flash before upload
  --fullclean, -F         Run PlatformIO fullclean first
  --just-build, -B        Build only; do not upload or monitor
                          With no device, build every PlatformIO environment
                          plus both T-Display P4 SKUs (skipped if ESP-IDF is
                          not installed)
  --help, -h              Show this help

With no device flag, an interactive shell prompts for a target. A non-interactive
upload defaults to --tdeck. Set PIO=/path/to/pio to override the CLI executable.
For the T-Display P4, set PORT=/dev/cu.usbmodemXXXX to pick the serial port
(idf.py auto-detects otherwise).
EOF
}

select_env() {
  local env_name="$1"
  if [ "$ENV_EXPLICIT" = true ]; then
    echo "Choose only one device flag." >&2
    exit 2
  fi
  if ! has_env "$env_name"; then
    echo "PlatformIO environment not found: $env_name" >&2
    exit 1
  fi
  ENV_NAME="$env_name"
  ENV_EXPLICIT=true
}

prompt_for_device() {
  local options=(
    "$TDECK_ENV"
    "$TDECK_PRO_V1_0_ENV"
    "$TDECK_PRO_V1_1_ENV"
    "$TDECK_MAX_ENV"
    "$M9_ENV"
    "$HELTEC_ENV"
    "$HELTEC_R8_ENV"
    "$ATTAKY_ENV"
    "$WIO_ENV"
    "$PAGER_LR1121_ENV"
    "$PAGER_SX1262_ENV"
    "$RAK_ENV"
    "$P4_ENV"
    "$P4_LCD_ENV"
  )
  local available=()
  local env_name
  for env_name in "${options[@]}"; do
    has_env "$env_name" && available+=("$env_name")
  done

  if [ "${#available[@]}" -eq 0 ]; then
    echo "No supported PlatformIO environments found." >&2
    exit 1
  fi
  if [ ! -t 0 ]; then
    ENV_NAME="$TDECK_ENV"
    has_env "$ENV_NAME" || ENV_NAME="${available[0]}"
    echo "[PIO] Non-interactive shell; using $(env_label "$ENV_NAME") ($ENV_NAME)."
    return
  fi

  echo "Select a device:"
  local i choice
  for i in "${!available[@]}"; do
    printf '  %d) %s (%s)\n' "$((i + 1))" "$(env_label "${available[$i]}")" "${available[$i]}"
  done
  while true; do
    read -r -p "Enter choice [1-${#available[@]}]: " choice
    if [[ "$choice" =~ ^[0-9]+$ ]] &&
       [ "$choice" -ge 1 ] && [ "$choice" -le "${#available[@]}" ]; then
      ENV_NAME="${available[$((choice - 1))]}"
      return
    fi
    echo "Invalid selection."
  done
}

format_duration() {
  local total="$1"
  local hours=$((total / 3600))
  local minutes=$(((total % 3600) / 60))
  local seconds=$((total % 60))
  if [ "$hours" -gt 0 ]; then
    printf '%dh %02dm %02ds' "$hours" "$minutes" "$seconds"
  else
    printf '%dm %02ds' "$minutes" "$seconds"
  fi
}

run_target() {
  local target="$1"
  local label="$2"
  echo "[PIO] $label: $(env_label "$ENV_NAME") ($ENV_NAME)"
  "$PIO" run -e "$ENV_NAME" -t "$target"
}

sha256_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --tdeck)        select_env "$TDECK_ENV" ;;
    --tdeck-pro-v1-0) select_env "$TDECK_PRO_V1_0_ENV" ;;
    --tdeck-pro-v1-1) select_env "$TDECK_PRO_V1_1_ENV" ;;
    --tdeck-max)    select_env "$TDECK_MAX_ENV" ;;
    --heltec)       select_env "$HELTEC_ENV" ;;
    --heltec-r8)    select_env "$HELTEC_R8_ENV" ;;
    --attaky)       select_env "$ATTAKY_ENV" ;;
    --wio)          select_env "$WIO_ENV" ;;
    --pager-lr1121) select_env "$PAGER_LR1121_ENV" ;;
    --pager-sx1262) select_env "$PAGER_SX1262_ENV" ;;
    --m9)           select_env "$M9_ENV" ;;
    --rak)          select_env "$RAK_ENV" ;;
    --tdisplay-p4)  select_env "$P4_ENV" ;;
    --tdisplay-p4-lcd) select_env "$P4_LCD_ENV" ;;
    --erase|-E)     ERASE_FIRST=true ;;
    --fullclean|-F) FULLCLEAN=true ;;
    --just-build|-B) JUST_BUILD=true ;;
    --help|-h)      show_usage; exit 0 ;;
    *)
      echo "Unknown argument: $1" >&2
      show_usage >&2
      exit 2
      ;;
  esac
  shift
done

need_pio() {
  if [ -z "$PIO" ]; then
    echo "PlatformIO CLI not found. Install it or set PIO=/path/to/pio." >&2
    exit 1
  fi
}
need_p4_toolchain() {
  if ! p4_toolchain_ready; then
    echo "ESP-IDF for the T-Display P4 is not installed: run make -C tanmatsu sdk," >&2
    echo "then tanmatsu/fetch-deps.sh and tdisplay_p4/fetch-deps.sh." >&2
    exit 1
  fi
}

if [ "$JUST_BUILD" = true ]; then
  if [ "$ERASE_FIRST" = true ]; then
    echo "--erase cannot be combined with --just-build." >&2
    exit 2
  fi

  build_envs=()
  if [ "$ENV_EXPLICIT" = true ]; then
    build_envs+=("$ENV_NAME")
  else
    while IFS= read -r env_name; do
      [ -n "$env_name" ] && build_envs+=("$env_name")
    done < <(all_envs)
    build_envs+=("$P4_ENV" "$P4_LCD_ENV")
  fi
  for env_name in "${build_envs[@]}"; do
    is_p4_env "$env_name" || { need_pio; break; }
  done
  if [ "$ENV_EXPLICIT" = true ] && is_p4_env "$ENV_NAME"; then
    need_p4_toolchain
  fi
  if [ "${#build_envs[@]}" -eq 0 ]; then
    echo "No PlatformIO environments found." >&2
    exit 1
  fi

  echo "[PIO] Build-only sweep over ${#build_envs[@]} environment(s)."
  sweep_start="$(date +%s)"
  results=()
  failed=0
  for ENV_NAME in "${build_envs[@]}"; do
    echo
    echo "===================================================================="
    echo "[PIO] Building $(env_label "$ENV_NAME") ($ENV_NAME)"
    echo "===================================================================="
    env_start="$(date +%s)"
    if is_p4_env "$ENV_NAME"; then
      if ! p4_toolchain_ready; then
        results+=("skip  $ENV_NAME  (ESP-IDF not installed)")
        continue
      fi
      p4_actions=(build)
      [ "$FULLCLEAN" = true ] && p4_actions=(fullclean build)
      if run_p4 "$ENV_NAME" "${p4_actions[@]}"; then
        env_end="$(date +%s)"
        size_note=""
        bin_path="tdisplay_p4/build/tdisplay_p4/application.bin"
        [ -f "$bin_path" ] && size_note="  $(( $(wc -c < "$bin_path") / 1024 )) KB"
        results+=("ok    $ENV_NAME  $(format_duration "$((env_end - env_start))")$size_note")
      else
        env_end="$(date +%s)"
        results+=("FAIL  $ENV_NAME  $(format_duration "$((env_end - env_start))")")
        failed=$((failed + 1))
      fi
      continue
    fi
    if [ "$FULLCLEAN" = true ] && ! "$PIO" run -e "$ENV_NAME" -t fullclean; then
      env_end="$(date +%s)"
      results+=("FAIL  $ENV_NAME  (fullclean)  $(format_duration "$((env_end - env_start))")")
      failed=$((failed + 1))
      continue
    fi
    if "$PIO" run -e "$ENV_NAME"; then
      env_end="$(date +%s)"
      size_note=""
      bin_path=".pio/build/$ENV_NAME/firmware.bin"
      if [ -f "$bin_path" ]; then
        size_note="  $(( $(wc -c < "$bin_path") / 1024 )) KB"
      fi
      results+=("ok    $ENV_NAME  $(format_duration "$((env_end - env_start))")$size_note")
    else
      env_end="$(date +%s)"
      results+=("FAIL  $ENV_NAME  $(format_duration "$((env_end - env_start))")")
      failed=$((failed + 1))
    fi
  done

  echo
  echo "===================================================================="
  echo "[PIO] Build summary"
  echo "===================================================================="
  printf '  %s\n' "${results[@]}"
  sweep_end="$(date +%s)"
  echo "[PIO] ${#build_envs[@]} environment(s), $failed failed, total $(format_duration "$((sweep_end - sweep_start))")."
  if [ "$failed" -gt 0 ]; then
    exit 1
  fi
  exit 0
fi

if [ "$ENV_EXPLICIT" = false ]; then
  prompt_for_device
fi

if is_p4_env "$ENV_NAME"; then
  need_p4_toolchain
  p4_actions=()
  [ "$FULLCLEAN" = true ]   && p4_actions+=(fullclean)
  [ "$ERASE_FIRST" = true ] && p4_actions+=(erase-flash)
  p4_actions+=(build flash)
  start="$(date +%s)"
  run_p4 "$ENV_NAME" "${p4_actions[@]}"
  end="$(date +%s)"
  echo "[IDF] Build/flash completed in $(format_duration "$((end - start))")."
  run_p4 "$ENV_NAME" monitor
  exit $?
fi

need_pio

if [ "$ERASE_FIRST" = true ]; then
  run_target erase "Erase flash"
fi

start="$(date +%s)"
if [ "$FULLCLEAN" = true ]; then
  run_target fullclean "Full clean"
fi
run_target upload "Build and upload"
end="$(date +%s)"
echo "[PIO] Build/upload completed in $(format_duration "$((end - start))")."

elf_path=".pio/build/$ENV_NAME/firmware.elf"
bin_path=".pio/build/$ENV_NAME/firmware.bin"
if [ -f "$elf_path" ]; then
  elf_sha="$(sha256_file "$elf_path")"
  echo "[PIO] ELF SHA256: $elf_sha"
  echo "[PIO] Runtime monitor should show: ELF file SHA256: ${elf_sha:0:16}"
fi
if [ -f "$bin_path" ]; then
  echo "[PIO] BIN SHA256: $(sha256_file "$bin_path")"
fi

run_target monitor "Monitor"