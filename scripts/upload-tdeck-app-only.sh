#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
pio_bin="${PIO_BIN:-/Users/tylersmith/.platformio/penv/bin/pio}"
python_bin="${PIO_PYTHON:-/Users/tylersmith/.platformio/penv/bin/python}"
esptool_py="${ESPTOOL_PY:-/Users/tylersmith/.platformio/packages/tool-esptoolpy/esptool.py}"
environment="LilyGo_TDeck_companion_radio_touch"
firmware="$repo_root/.pio/build/$environment/firmware.bin"

usage() {
  echo "Usage: $0 --port /dev/cu.usbmodemXXXX [--skip-build] --confirm-app-only"
  echo "Writes only the application partition at 0x10000."
}

port=""
build=1
confirmed=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      port="$2"
      shift 2
      ;;
    --skip-build)
      build=0
      shift
      ;;
    --confirm-app-only)
      confirmed=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -n "$port" && "$port" == /dev/cu.* ]] || {
  echo "Refusing upload: pass one explicit macOS /dev/cu.* port." >&2
  exit 2
}
[[ $confirmed -eq 1 ]] || {
  echo "Refusing upload without --confirm-app-only." >&2
  exit 2
}
[[ -x "$pio_bin" ]] || { echo "PlatformIO not executable: $pio_bin" >&2; exit 2; }
[[ -x "$python_bin" ]] || { echo "Python not executable: $python_bin" >&2; exit 2; }
[[ -f "$esptool_py" ]] || { echo "esptool not found: $esptool_py" >&2; exit 2; }
[[ -c "$port" ]] || { echo "Serial port is not present: $port" >&2; exit 2; }

if [[ $build -eq 1 ]]; then
  "$pio_bin" run --project-dir "$repo_root" -e "$environment"
fi
[[ -f "$firmware" ]] || { echo "Firmware image missing: $firmware" >&2; exit 2; }

echo "T-Deck application-only upload"
echo "  port:   $port"
echo "  image:  $firmware"
echo "  offset: 0x10000"
echo "Bootloader, partition table, boot_app0, NVS, and filesystems are not written."

"$python_bin" "$esptool_py" \
  --chip esp32s3 \
  --port "$port" \
  --baud 115200 \
  --before default_reset \
  --after hard_reset \
  --no-stub \
  write_flash 0x10000 "$firmware"
