#!/usr/bin/env bash
# Build every T-Display P4 release image and verify each one is what it claims.
#
# The P4 is not a PlatformIO env, so release.sh cannot build it: its bins are
# produced here and injected into the release tree afterwards. That used to be a
# handful of ad-hoc commands, which stopped being safe at EIGHT variants:
#
#   panel      AMOLED (RM69A10, default)  or  LCD      (HI8561, WADA_P4_LCD=1)
#   radio      SX1262 (default)           or  LR2021   (WADA_P4_LR2021=1)
#   C6 stack   esp-hosted (default)       or  ESP-AT   (WADA_P4_LEGACY_AT=1)
#
# The C6 axis is the dangerous one. Current V1 units ship the C6 with esp-hosted
# firmware; older units still carry the factory ESP-AT. Flash the wrong one and
# the C6 is unreachable -- which before b52c71f boot-looped the board, and still
# means no Wi-Fi and no BLE. The two builds are otherwise identical, so NOTHING
# about the binary tells you which it is except the artifact name it carries.
#
# TWO traps this script exists to close, both hit on 2026-10-05:
#
#  1. The CMake cache. Each variant has its own build dir, but a dir reused with
#     different env vars keeps the OLD selection: a build asked for as hosted came
#     out as ESP-AT, flag silently ignored. The cache is cleared per variant here.
#  2. Collecting from the wrong directory. build/tdisplay_p4 is only the AMOLED
#     SX1262 variant; the others live in build/tdisplay_p4_lcd and friends.
#     Copying the first one four times produces four identical "variants" that
#     all look fine until someone flashes one.
#
# So every image is verified against the OTA name baked into it before it is
# accepted. That is the same string the device builds its self-update URL from,
# so if it disagrees with the filename the artifact is wrong in the way that
# matters -- a device would fetch the other variant's firmware. The T-Deck Max
# shipped exactly that bug (a Max self-updating to Pro firmware) and this table
# has been wrong three times. The check is cheap; a mismatch aborts the release.
#
# Usage:
#   scripts/build/p4-release.sh beta_88 [outdir]
set -euo pipefail

TAG="${1:?usage: p4-release.sh <tag> [outdir]}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${2:-$ROOT/out/p4_$TAG}"
P4="$ROOT/tdisplay_p4"
# esptool with ESP32-P4 support: the IDF env's, not whatever is on PATH.
IDFPY="$ROOT/tanmatsu/esp-idf-tools/python_env/idf5.5_py3.9_env/bin/python"

[ -x "$IDFPY" ] || { echo "ABORT: IDF python not found at $IDFPY" >&2; exit 1; }
mkdir -p "$OUT"

# name | build dir | env assignments ("-" = none)
VARIANTS=(
  "wadamesh-tdisplay-p4|tdisplay_p4|-"
  "wadamesh-tdisplay-p4-lr2021|tdisplay_p4_lr2021|WADA_P4_LR2021=1"
  "wadamesh-tdisplay-p4-lcd|tdisplay_p4_lcd|WADA_P4_LCD=1"
  "wadamesh-tdisplay-p4-lcd-lr2021|tdisplay_p4_lcd_lr2021|WADA_P4_LCD=1 WADA_P4_LR2021=1"
  "wadamesh-tdisplay-p4-at|tdisplay_p4|WADA_P4_LEGACY_AT=1"
  "wadamesh-tdisplay-p4-lr2021-at|tdisplay_p4_lr2021|WADA_P4_LR2021=1 WADA_P4_LEGACY_AT=1"
  "wadamesh-tdisplay-p4-lcd-at|tdisplay_p4_lcd|WADA_P4_LCD=1 WADA_P4_LEGACY_AT=1"
  "wadamesh-tdisplay-p4-lcd-lr2021-at|tdisplay_p4_lcd_lr2021|WADA_P4_LCD=1 WADA_P4_LR2021=1 WADA_P4_LEGACY_AT=1"
)

cd "$P4"
fail=0
for v in "${VARIANTS[@]}"; do
  name="${v%%|*}"; rest="${v#*|}"
  dir="${rest%%|*}"; envs="${rest#*|}"
  echo "=== $name  (build/$dir)"

  # Trap 1: never inherit another variant's selection. Two dirs are shared by an
  # esp-hosted and an ESP-AT build (the C6 axis does not change the dir name), so
  # the previous occupant has to be cleared -- but ONLY by a real clean. Deleting
  # CMakeCache.txt alone leaves CMakeFiles/ behind and the next configure dies in
  # CMakeDetermineCCompiler with "No such file or directory". build.sh fullclean
  # removes the tree properly and keeps the patched managed_components.
  # A marker records what this dir last built, so re-running the same variant
  # stays incremental instead of paying for a full rebuild every time.
  # Two of these dirs are shared by an esp-hosted and an ESP-AT build, because the
  # C6 axis does not change the dir name. The backend is read from the
  # ENVIRONMENT at CMake CONFIGURE time (if(DEFINED ENV{WADA_P4_LEGACY_AT}) in
  # main/CMakeLists.txt), and CMake does not reconfigure just because an env var
  # changed -- so an incremental build in a dir that was configured the other way
  # silently produces the OTHER variant, with no configure output to show for it.
  # That is what made the first two runs of this script ship mislabelled images.
  #
  # So: always start these from nothing, and VERIFY the dir is gone rather than
  # assuming the clean worked. The first version suppressed fullclean's output
  # and trusted it; it had not run, and the only evidence was a missing configure
  # line buried in a build log. A full rebuild per variant is the right price for
  # a script that runs once per release.
  if [ -d "build/$dir" ]; then
    if [ "$envs" = "-" ]; then
      env -u WADA_P4_LCD -u WADA_P4_LR2021 -u WADA_P4_LEGACY_AT ./build.sh fullclean >> "/tmp/p4rel_$name.clean.log" 2>&1 || true
    else
      env -u WADA_P4_LCD -u WADA_P4_LR2021 -u WADA_P4_LEGACY_AT $envs ./build.sh fullclean >> "/tmp/p4rel_$name.clean.log" 2>&1 || true
    fi
  fi
  if [ -d "build/$dir" ]; then
    echo "   FAIL: build/$dir still present after fullclean; refusing to build on a stale tree"
    echo "         (see /tmp/p4rel_$name.clean.log)"
    fail=$((fail+1)); continue
  fi

  # `env -u` every switch, then set only this variant's: an exported var in the
  # caller's shell would otherwise leak in and silently change the build.
  rc=0
  if [ "$envs" = "-" ]; then
    env -u WADA_P4_LCD -u WADA_P4_LR2021 -u WADA_P4_LEGACY_AT \
        WADA_FW_TAG="$TAG" ./build.sh build > "/tmp/p4rel_$name.log" 2>&1 || rc=$?
  else
    env -u WADA_P4_LCD -u WADA_P4_LR2021 -u WADA_P4_LEGACY_AT \
        $envs WADA_FW_TAG="$TAG" ./build.sh build > "/tmp/p4rel_$name.log" 2>&1 || rc=$?
  fi

  B="build/$dir"
  app="$B/application.bin"           # trap 2: this variant's dir, not the default one
  # Check the EXIT CODE, not the log text. A failed CMake configure prints
  # "Configuring incomplete, errors occurred!" and no "error:" line at all, so a
  # grep for compiler errors waved it through -- and because the previous build's
  # application.bin was still sitting in the dir, the next step happily read that
  # instead. Only the id check downstream caught it.
  if [ "$rc" -ne 0 ]; then
    echo "   FAIL: build exited $rc (see /tmp/p4rel_$name.log)"; fail=$((fail+1)); continue
  fi
  if [ ! -f "$app" ]; then
    echo "   FAIL: no application.bin (see /tmp/p4rel_$name.log)"; fail=$((fail+1)); continue
  fi

  # The verification that matters: what does the image say it is?
  # NOTE: no `grep -q` and no `head` in these pipelines. Both exit as soon as
  # they have what they need, which SIGPIPEs `strings`, and under `set -o
  # pipefail` that non-zero status reads as "check failed" on a perfectly good
  # image. Read the whole stream and decide afterwards.
  ids="$(strings -a "$app" | grep -oE 'wadamesh-tdisplay-p4[a-z0-9-]*' | sort -u || true)"
  got="$(printf '%s\n' "$ids" | sed -n '1p')"
  if [ "$got" != "$name" ]; then
    echo "   FAIL: image identifies as '$got', expected '$name'"
    echo "         A device built from this would self-update to the WRONG variant."
    fail=$((fail+1)); continue
  fi
  # And that it carries this release's tag, not a stale cached one.
  tagged="$(strings -a "$app" | grep -cE "(^|[^0-9a-z])${TAG}([^0-9]|$)" || true)"
  if [ "${tagged:-0}" = "0" ]; then
    echo "   FAIL: $TAG is not embedded (stale build dir?)"; fail=$((fail+1)); continue
  fi

  cp "$app" "$OUT/$name.bin"
  "$IDFPY" -m esptool --chip esp32p4 merge_bin --flash_mode dio --flash_size 16MB --flash_freq 80m \
    -o "$OUT/$name-merged.bin" \
    0x2000 "$B/bootloader/bootloader.bin" \
    0x8000 "$B/partition_table/partition-table.bin" \
    0xf000 "$B/ota_data_initial.bin" \
    0x20000 "$app" > /dev/null
  echo "   ok  $(stat -f%z "$app") bytes  id=$got"
done

echo
if [ "$fail" -ne 0 ]; then
  echo "ABORT: $fail of ${#VARIANTS[@]} P4 variants failed. Nothing is safe to ship." >&2
  exit 1
fi
echo "all ${#VARIANTS[@]} P4 variants built and verified -> $OUT"
echo "Copy both .bin and -merged.bin for each into releases/<CHANNEL>/$TAG/ AND latest[-beta]/."
