#!/bin/sh
# Builds wadamesh for the M5Stack Cardputer Zero on a 64-bit Linux PC (Debian/Ubuntu):
#   sudo apt install cmake ninja-build g++-aarch64-linux-gnu libsdl2-dev
#   web/zero/build.sh            -> web/zero/out/wadamesh-zero.tar.gz (copy it to the Zero)
# WADA_FW_TAG=beta_N bakes a release tag in (scripts/build/zero-release.sh does this in Docker).
set -e
ZERO="$(cd "$(dirname "$0")" && pwd)"; WEB="$(dirname "$ZERO")"
[ -f "$WEB/third_party/meshcore/src/Mesh.h" ] || "$WEB/fetch-deps.sh"
[ -f "$WEB/desktop/sdl2-include/SDL.h" ] || "$WEB/desktop/vendor-sdl2-headers.sh"
TC="$ZERO/aarch64-toolchain.cmake"
cmake -S "$WEB" -B "$WEB/build-zero-arm64" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWADA_ZERO=ON -DWADA_FW_TAG="${WADA_FW_TAG:-}" -DCMAKE_TOOLCHAIN_FILE="$TC" >/dev/null
cmake --build "$WEB/build-zero-arm64" --target wadamesh-host
cmake -S "$WEB" -B "$WEB/build-desktop-arm64" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWADA_DESKTOP_DLSDL=ON \
      -DWADA_SDL2_INCLUDE="$WEB/desktop/sdl2-include" -DWADA_FW_TAG="${WADA_FW_TAG:-}" -DCMAKE_TOOLCHAIN_FILE="$TC" >/dev/null
cmake --build "$WEB/build-desktop-arm64" --target wadamesh-host
OUT="$ZERO/out/wadamesh-zero"; rm -rf "$ZERO/out"; mkdir -p "$OUT"
aarch64-linux-gnu-strip -o "$OUT/wadamesh-zero" "$WEB/build-zero-arm64/wadamesh-host"
aarch64-linux-gnu-strip -o "$OUT/wadamesh-desktop" "$WEB/build-desktop-arm64/wadamesh-host"
cp -r "$ZERO/dist/." "$OUT/"
cp "$WEB/../scripts/build/make_cities.py" "$OUT/"
(cd "$ZERO/out" && tar czf wadamesh-zero.tar.gz wadamesh-zero)
echo "-> $ZERO/out/wadamesh-zero.tar.gz"
