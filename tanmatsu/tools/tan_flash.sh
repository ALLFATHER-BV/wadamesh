#!/usr/bin/env bash
# Sideload the freshly-built wadamesh onto a Tanmatsu, as an AppFS app under the
# badge.team launcher. Run AFTER `tanmatsu/build.sh build`.
#
#   tanmatsu/tools/tan_flash.sh [/dev/cu.usbmodemXXXX]
#
# With no port given it probes each USB serial device and picks the one that answers
# as an ESP32-P4. The Tanmatsu presents TWO ports (the P4 and the C6 coprocessor) and
# which name each gets changes between replugs, so detecting by chip beats guessing.
#
# NEVER use `idf.py flash` on this board: that writes bootloader, partition table and
# OTA slots, which replaces the launcher OS itself. This writes two AppFS sectors and
# nothing else.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"
OUT="${OUT:-/tmp}"

# ONE invocation: it derives the new version from the metadata of the previous
# deploy, so running it twice would bump the version an extra time and, worse, diff
# against a file it had just rewritten.
DEPLOY_OUT=$(python3 "$HERE/tan_deploy.py" --out "$OUT") || { echo "$DEPLOY_OUT"; echo "image build FAILED"; exit 1; }
echo "$DEPLOY_OUT"
APP_OFF=$(printf '%s\n' "$DEPLOY_OUT" | grep '^APP_OFFSET=' | cut -d= -f2)
PART_OFF=0x420000
[ -z "$APP_OFF" ] && { echo "could not determine the app offset"; exit 1; }

PORT="${1:-}"
if [ -z "$PORT" ]; then
  for P in $(/bin/ls -1 /dev/cu.usbmodem* 2>/dev/null); do
    CHIP=$(python3 "$ESPTOOL" --port "$P" --before default_reset --after no_reset \
             --connect-attempts 2 chip_id 2>&1 | grep -m1 -i "^Chip is" || true)
    echo "  $P -> ${CHIP:-no answer}"
    case "$CHIP" in *ESP32-P4*) PORT="$P";; esac
  done
fi
[ -z "$PORT" ] && {
  echo "!! No ESP32-P4 found. If the device has no serial ports at all, the C6 has wedged:"
  echo "   FULLY power-cycle the Tanmatsu (off then on, not just reset) and retry."
  exit 1; }

echo "=== P4 = $PORT — app @ $APP_OFF first, metadata @ $PART_OFF last ==="
python3 "$ESPTOOL" --chip esp32p4 -p "$PORT" --before default_reset --after no_reset \
  --connect-attempts 3 write_flash "$APP_OFF" "$OUT/appfs_app.bin"  2>&1 | grep -iE "Hash of data verified|fatal|error" | tail -1
python3 "$ESPTOOL" --chip esp32p4 -p "$PORT" --before default_reset --after hard_reset \
  --connect-attempts 3 write_flash "$PART_OFF" "$OUT/appfs_meta.bin" 2>&1 | grep -iE "Hash of data verified|fatal|error" | tail -1

# Read the metadata back and compare. The second write is the one that commits the
# install, so if it silently dropped, the launcher would show the OLD app with no
# indication anything failed.
python3 "$ESPTOOL" --chip esp32p4 -p "$PORT" --before default_reset --after hard_reset \
  --connect-attempts 3 read_flash "$PART_OFF" 0x10000 "$OUT/appfs_readback.bin" 2>&1 | grep -iE "Read [0-9]|fatal" | tail -1
if cmp -s "$OUT/appfs_readback.bin" "$OUT/appfs_meta.bin"; then
  echo "metadata verified — installed. Launch 'WadaMesh' from the launcher menu."
else
  echo "metadata DIFFERS — the commit write did not land. Re-run this script."
  exit 1
fi
