#!/usr/bin/env bash
# One-time: dump YOUR Tanmatsu's AppFS partition as the "pristine" baseline that
# tan_deploy.py diffs against.
#
# Do this while the launcher's own apps are installed and wadamesh is NOT, so the
# baseline represents the device as it ships. Using someone else's dump risks writing
# over apps you actually have: the deploy writes only the sectors that differ from
# this file, so the file has to describe YOUR device.
#
# Usage: dump-pristine.sh /dev/cu.usbmodemXXXX
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:?usage: dump-pristine.sh /dev/cu.usbmodemXXXX}"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"
python3 "$ESPTOOL" --chip esp32p4 -p "$PORT" read_flash 0x420000 0x800000 "$HERE/dev_appfs.bin"
ls -la "$HERE/dev_appfs.bin"
echo "baseline saved. Keep it: every later sideload diffs against it."
