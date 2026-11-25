#!/usr/bin/env bash
# Build the M5Stack Cardputer Zero release archive (wadamesh-zero.tar.gz) for a tag.
#
#   scripts/build/zero-release.sh beta_107      -> out/zero_beta_107/wadamesh-zero.tar.gz (+ .sha256)
#
# The Cardputer Zero is not an ESP32: WADAMESH runs there as an aarch64 Linux program
# built from web/ (CARDPUTER_ZERO.md, web/zero/). release.sh cannot build it, so this
# script does, and release.sh injects the archive afterwards (step 2d), the same way
# the ESP-IDF boards come in from p4-release.sh.
#
# On a Mac (Apple Silicon or Intel with arm64 emulation) it builds in a Debian trixie
# arm64 container: the Zero's image is Debian 13, and the UI needs glibc >= 2.38
# (strlcpy), so a bookworm build does not compile. On an arm64 Linux host with the
# packages below installed it builds natively (ZERO_NATIVE=1).
#
# The binary is verified to carry the tag before the archive is accepted.
set -euo pipefail
TAG="${1:?usage: zero-release.sh <tag>   e.g. beta_107}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/out/zero_$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

if [ -n "${ZERO_NATIVE:-}" ]; then
  WADA_FW_TAG="$TAG" "$ROOT/web/zero/build.sh"
  cp "$ROOT/web/zero/out/wadamesh-zero.tar.gz" "$OUT/"
else
  command -v docker >/dev/null || { echo "zero-release: docker not found (or run on arm64 Linux with ZERO_NATIVE=1)" >&2; exit 1; }
  # The tree is copied into the container (read-only mount), so the host's web/build-*
  # dirs and fetched deps never mix with a Linux build. third_party is cached in a volume.
  docker run --rm --platform linux/arm64 -e WADA_FW_TAG="$TAG" \
    -v "$ROOT":/src:ro -v "$OUT":/out -v wadamesh-zero-deps:/deps debian:trixie sh -c '
      set -e
      apt-get update -qq >/dev/null
      DEBIAN_FRONTEND=noninteractive apt-get install -y -qq cmake ninja-build g++ git ca-certificates libsdl2-dev python3 >/dev/null
      mkdir -p /w && cd /src
      tar cf - --exclude=./.pio --exclude="./web/build*" --exclude=./web/third_party --exclude=./web/node_modules \
               --exclude=./.git --exclude=./out --exclude=./web/zero/out --exclude=./web/desktop/sdl2-include . | (cd /w && tar xf -)
      cd /w && git init -q . && git config --global --add safe.directory "*"
      ln -s /deps web/third_party
      web/zero/build.sh
      cp web/zero/out/wadamesh-zero.tar.gz /out/
    '
fi

# Firmware data gate: the program inside must name this tag (About, test reports).
CHK="$(mktemp -d)"; trap 'rm -rf "$CHK"' EXIT
tar xzf "$OUT/wadamesh-zero.tar.gz" -C "$CHK"
n="$(strings "$CHK/wadamesh-zero/wadamesh-zero" | grep -c "$TAG" || true)"
[ "${n:-0}" -gt 0 ] || { echo "ERROR: wadamesh-zero does not embed $TAG" >&2; exit 1; }
(cd "$OUT" && shasum -a 256 wadamesh-zero.tar.gz > wadamesh-zero.tar.gz.sha256)
echo "-> ${OUT#$ROOT/}/wadamesh-zero.tar.gz ($(wc -c < "$OUT/wadamesh-zero.tar.gz" | tr -d ' ') B, embeds $TAG)"
