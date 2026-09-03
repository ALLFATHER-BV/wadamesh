#!/usr/bin/env bash
# wadamesh release — two channels: TEST (beta, default) and STABLE (promote).
#
# Cut a TEST build (builds both boards, publishes to the beta channel — fast/frequent):
#   WADAMESH_VPS=user@host scripts/release.sh beta_21
#
# Promote an ALREADY-BUILT beta to STABLE (no rebuild — copies the tested bins):
#   WADAMESH_VPS=user@host scripts/release.sh beta_21 --promote
#
# Channels on the VPS firmware root (firmware.wadamesh.com, behind Cloudflare):
#   releases/BETA/<tag>/   immutable beta archives   <- test channel + new-firmware beta check
#   releases/TOUCH/<tag>/  immutable stable archives <- stable feed + legacy on-device check
#   latest-beta/           beta feed   (version.json + manifests + rolling bins)
#   latest/                stable feed (version.json + manifests + rolling bins)
#
# The VPS target comes from the environment — never commit it (Cloudflare fronts
# the origin, so the IP isn't needed in the firmware anyway).
set -euo pipefail

TAG="${1:?usage: release.sh <tag> [--promote]   e.g. beta_21  /  beta_21 --promote}"
MODE="beta"
[ "${2:-}" = "--promote" ] && MODE="stable"

PIO="${PIO:-$HOME/Library/Python/3.9/bin/pio}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out/firmware"                        # local mirror of the VPS firmware root
DEST="${WADAMESH_VPS:-}"; DEST_PATH="${WADAMESH_VPS_PATH:-/srv/wadamesh/firmware}"

# env:binname pairs — plain string form (works on macOS's bash 3.2; no associative arrays).
# All S3/PIO boards. The T-Display P4 is an ESP32-P4 IDF build (tdisplay_p4/) handled OUT of band
# — see the release skill / the P4 build+merge step — because this loop is PlatformIO-only.
ENVS="heltec_v4_tft_companion_radio_usb_tcp_touch:wadamesh-heltec-v4-tft LilyGo_TDeck_companion_radio_touch:wadamesh-tdeck ThinkNode_M9_companion_radio_touch:wadamesh-thinknode-m9 rak_tap_v2_companion_radio_touch:wadamesh-rak-tap-v2 heltec_v4_r8_tft_companion_radio_usb_tcp_touch:wadamesh-heltec-v4-r8-tft tlora_pager_lr1121_companion_radio_touch:wadamesh-tlora-pager-lr1121 tlora_pager_sx1262_companion_radio_touch:wadamesh-tlora-pager-sx1262 attaky_mesh_series_companion_radio_touch:wadamesh-attaky wio_tracker_l2_companion_radio_touch:wadamesh-wio-tracker-l2"

# Per-channel destination paths.
if [ "$MODE" = "stable" ]; then
  ARCH="$OUT/releases/TOUCH"; FEED="$OUT/latest"
else
  ARCH="$OUT/releases/BETA";  FEED="$OUT/latest-beta"
fi

# 1. Pull the current published tree so listings stay complete across tags.
if [ -n "$DEST" ]; then
  mkdir -p "$OUT"
  # --exclude apps/: the Lua app + language store lives under the same document
  # root but is NOT ours. It is published by scripts/deploy-apps.sh from
  # deploy/apps/, which is the canonical copy. Pulling it into out/ makes a local
  # mirror that the push below then sends back, overwriting a newer store with
  # whatever was current when it was pulled. That happened on 2026-08-23: the
  # beta_69 firmware push reverted the store to sdktest 1.2 and language v15 and
  # deleted two apps, minutes after deploy-apps.sh had published them.
  rsync -a --exclude 'apps/' "$DEST:$DEST_PATH/" "$OUT/" 2>/dev/null || echo "note: first publish (nothing to pull yet)"
fi
mkdir -p "$ARCH/$TAG" "$FEED"

if [ "$MODE" = "beta" ]; then
  # 2b. TEST build: compile both boards (tag + version embedded) into the beta archive + feed.
  # Firmware data (About screen + the app's device info) is derived here, never
  # hand-maintained: the tag, the core version actually pinned in platformio.ini,
  # and today's build date. Every staged binary is verified to carry them below —
  # beta_60 shipped with a stale in-tree default and About read "v1.16.0-touch".
  CORE_VER="$(grep -oE 'meshcomod\.git#core-v[0-9.]+' platformio.ini | head -1 | sed 's/.*#core-//')"
  [ -n "$CORE_VER" ] || { echo "ERROR: no core-v* pin found in platformio.ini"; exit 1; }
  # Hyphens, NOT spaces: PLATFORMIO_BUILD_FLAGS is whitespace-split, so a date
  # like "10 Aug 2026" tears the flag string apart and every -D after it is lost
  # — including the release tag, which is how a build silently ships untagged.
  BUILD_DATE="$(date '+%d-%b-%Y')"
  echo "firmware data: tag=$TAG core=$CORE_VER date=$BUILD_DATE"
  export PLATFORMIO_BUILD_FLAGS="-DFIRMWARE_RELEASE_TAG='\"${TAG}\"' -DFIRMWARE_VERSION='\"wadamesh ${TAG}\"' -DFIRMWARE_BUILD_DATE='\"${BUILD_DATE}\"' -DFIRMWARE_CORE_VERSION='\"${CORE_VER}\"'"
  # Keep the in-tree default (what DEV flashes show) in step with the pinned core,
  # so a bench build never reports a version the firmware no longer is.
  sed -i '' -E "s/^#define FIRMWARE_VERSION \"v[0-9.]+-touch\"/#define FIRMWARE_VERSION \"${CORE_VER}-touch\"/" src/MyMesh.h
  git diff --quiet src/MyMesh.h || echo "note: refreshed the dev FIRMWARE_VERSION default in src/MyMesh.h -> ${CORE_VER}-touch (commit it)"
  for pair in $ENVS; do
    env="${pair%%:*}"; name="${pair##*:}"
    "$PIO" run -t mergebin -e "$env"
    cp ".pio/build/$env/firmware.bin"        "$ARCH/$TAG/$name.bin"
    cp ".pio/build/$env/firmware-merged.bin" "$ARCH/$TAG/$name-merged.bin"
    cp ".pio/build/$env/firmware-merged.bin" "$FEED/$name-merged.bin"   # rolling -> flasher (standalone)
    cp ".pio/build/$env/firmware.bin"        "$FEED/$name.bin"          # rolling app image -> Launcher path
  done
else
  # 2s. STABLE promotion: NO rebuild — copy the already-tested beta bins so what
  #     ships to stable is byte-for-byte what the community tested.
  SRC="$OUT/releases/BETA/$TAG"
  if [ ! -d "$SRC" ] || ! ls "$SRC/"*.bin >/dev/null 2>&1; then
    # beta_20 + any pre-channel build already lives under releases/TOUCH/<tag>/.
    if ls "$ARCH/$TAG/"*.bin >/dev/null 2>&1; then
      SRC="$ARCH/$TAG"; echo "note: $TAG not in releases/BETA — promoting from existing $ARCH/$TAG"
    else
      echo "ERROR: no built bins for $TAG (expected $OUT/releases/BETA/$TAG). Cut the beta first."; exit 1
    fi
  fi
  for pair in $ENVS; do
    name="${pair##*:}"
    cp "$SRC/$name.bin"        "$ARCH/$TAG/$name.bin"
    cp "$SRC/$name-merged.bin" "$ARCH/$TAG/$name-merged.bin"
    cp "$SRC/$name-merged.bin" "$FEED/$name-merged.bin"
    cp "$SRC/$name.bin"        "$FEED/$name.bin"
  done
  echo "promoted $TAG bins  $SRC -> $ARCH/$TAG (+ $FEED)"
fi

# 3. Regenerate this channel's update-check listing (firmware scans the body for
#    the highest beta_<N>). JSON array of dir names — GitHub-contents shape.
python3 - "$ARCH" > "$ARCH/index.json" <<'PY'
import os, sys, json
rel = sys.argv[1]
betas = sorted(d for d in os.listdir(rel)
               if d.startswith("beta_") and os.path.isdir(os.path.join(rel, d)))
print(json.dumps([{"name": b, "type": "dir"} for b in betas], indent=2))
PY
echo "listing ($MODE): $(python3 -c 'import json,sys;print(", ".join(x["name"] for x in json.load(open(sys.argv[1]))))' "$ARCH/index.json")"

# 3a2. FIRMWARE DATA GATE — every staged app image must carry this tag, or the
#      release ships binaries whose About screen / update check disagree with the
#      release they are published under. Covers the out-of-band IDF boards
#      (T-Display P4) too: they are injected into $ARCH/$TAG before this runs, so
#      forgetting to rebuild them with WADA_FW_TAG fails here instead of shipping.
missing=""
for f in "$ARCH/$TAG"/*.bin; do
  case "$f" in *-merged.bin) continue;; esac      # merged images embed the same app
  # grep -c, not grep -q: this script runs under `set -o pipefail`, and grep -q
  # exits at the first match, which SIGPIPEs `strings` and makes the whole
  # pipeline "fail" — reporting every image as untagged even when it is fine.
  n="$(strings "$f" | grep -c "$TAG" || true)"
  [ "${n:-0}" -gt 0 ] || missing="$missing $(basename "$f")"
done
if [ -n "$missing" ]; then
  echo "ERROR: these staged images do not embed $TAG:$missing"
  echo "       rebuild them with the tag (S3: release.sh does it; P4: WADA_FW_TAG=$TAG ./build.sh build)"
  exit 1
fi
echo "firmware data verified: every staged image embeds $TAG"

# 3b. Web-flasher metadata for THIS channel (version.json + manifests -> the channel feed).
python3 "$ROOT/scripts/build/gen-flasher-meta.py" "$TAG" "$FEED" "$ROOT/release-notes/$TAG.txt" "$MODE"

# 3c. Mesh America Configurator provider catalog — STABLE only (it is a public
#     flasher catalog served live from raw.githubusercontent main; test builds
#     must not bump it). Points at the immutable releases/TOUCH/$TAG bins.
if [ "$MODE" = "stable" ]; then
  CATALOG="$ROOT/deploy/meshamerica-catalog.json"
  python3 "$ROOT/deploy/gen-meshamerica-catalog.py" "$TAG" > "$CATALOG.tmp" && mv "$CATALOG.tmp" "$CATALOG"
  mkdir -p "$OUT/meshamerica" && cp "$CATALOG" "$OUT/meshamerica/catalog.json"
  if git -C "$ROOT" diff --quiet -- "$CATALOG"; then
    echo "Mesh America catalog already current for $TAG"
  elif [ "$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null)" = "main" ]; then
    if git -C "$ROOT" commit -q -m "chore: refresh Mesh America catalog for $TAG" -- "$CATALOG" \
         && git -C "$ROOT" push origin HEAD:main; then
      echo "Mesh America catalog refreshed + pushed to main ($TAG)"
    else
      echo "WARN: Mesh America catalog commit/push failed — push deploy/meshamerica-catalog.json to main by hand"
    fi
  else
    echo "NOTE: regenerated $CATALOG for $TAG but not on 'main' — commit + push it to main."
  fi
fi

# 4. Publish to the VPS (Cloudflare caches at the edge).
if [ -n "$DEST" ]; then
  # Same exclusion on the way out, so a stale apps/ left in out/ by an older
  # checkout cannot clobber the store either. The firmware release never
  # publishes the store; deploy-apps.sh does, and only that.
  rsync -av --exclude 'apps/' "$OUT/" "$DEST:$DEST_PATH/"
  echo "published $TAG ($MODE) -> $DEST:$DEST_PATH"
else
  echo "WADAMESH_VPS not set — built + staged in $ARCH/$TAG + $FEED only (no publish)."
fi
