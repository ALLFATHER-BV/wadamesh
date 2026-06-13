#!/usr/bin/env bash
# wadamesh release: build tagged bins for both touch boards, refresh the
# update-check listing, and publish to the firmware VPS (behind Cloudflare).
#
# Usage:
#   WADAMESH_VPS=user@your-vps WADAMESH_VPS_PATH=/srv/wadamesh/firmware \
#     scripts/release.sh beta_2
#
# The VPS target comes from the environment — never commit it (Cloudflare fronts
# the origin, so the IP isn't needed in the firmware anyway).
set -euo pipefail

TAG="${1:?usage: release.sh <tag>  e.g. beta_2}"
PIO="${PIO:-$HOME/Library/Python/3.9/bin/pio}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out/firmware"                       # local mirror of the VPS firmware root
REL="$OUT/releases/TOUCH"
DEST="${WADAMESH_VPS:-}"; DEST_PATH="${WADAMESH_VPS_PATH:-/srv/wadamesh/firmware}"

declare -A ENVS=(
  [heltec_v4_tft_companion_radio_usb_tcp_touch]=wadamesh-heltec-v4-tft
  [LilyGo_TDeck_companion_radio_touch]=wadamesh-tdeck
)

# 1. Pull the current published tree so the listing stays complete across tags.
if [ -n "$DEST" ]; then
  mkdir -p "$OUT"
  rsync -a "$DEST:$DEST_PATH/" "$OUT/" 2>/dev/null || echo "note: first publish (nothing to pull yet)"
fi
mkdir -p "$REL/$TAG"

# 2. Build both boards (app + merged), tag + version embedded.
export PLATFORMIO_BUILD_FLAGS="-DFIRMWARE_RELEASE_TAG='\"${TAG}\"' -DFIRMWARE_VERSION='\"wadamesh ${TAG}\"'"
for env in "${!ENVS[@]}"; do
  name="${ENVS[$env]}"
  "$PIO" run -t mergebin -e "$env"
  cp ".pio/build/$env/firmware.bin"        "$REL/$TAG/$name.bin"
  cp ".pio/build/$env/firmware-merged.bin" "$REL/$TAG/$name-merged.bin"
done

# 3. Regenerate the update-check listing (the firmware scans the body for the
#    highest beta_<N>). JSON array of dir names — GitHub-contents shape.
python3 - "$REL" > "$REL/index.json" <<'PY'
import os, sys, json
rel = sys.argv[1]
betas = sorted(d for d in os.listdir(rel)
               if d.startswith("beta_") and os.path.isdir(os.path.join(rel, d)))
print(json.dumps([{"name": b, "type": "dir"} for b in betas], indent=2))
PY
echo "listing: $(python3 -c 'import json,sys;print(", ".join(x["name"] for x in json.load(open(sys.argv[1]))))' "$REL/index.json")"

# 4. Publish to the VPS (Cloudflare caches at the edge).
if [ -n "$DEST" ]; then
  rsync -av "$OUT/" "$DEST:$DEST_PATH/"
  echo "published $TAG -> $DEST:$DEST_PATH"
else
  echo "WADAMESH_VPS not set — built + staged in $REL/$TAG only (no publish)."
fi
