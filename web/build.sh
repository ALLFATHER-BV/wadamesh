#!/usr/bin/env bash
# web/build.sh host   build the native SDL2 app   -> web/build-host/wadamesh-host
# web/build.sh run    build, then start it (extra args are passed to the app)
# web/build.sh shots  run the acceptance tour headless -> web/docs/*.png
# web/build.sh smoke  the same tour, screenshots discarded (exit status only)
# web/build.sh web    phase 2: the Emscripten build (not yet)
set -euo pipefail
cd "$(dirname "$0")"
WEB="$(pwd)"
target="${1:-host}"; shift || true
[ -f third_party/meshcore/src/Mesh.h ] || ./fetch-deps.sh
case "$target" in
  host|run)
    cmake -S "$WEB" -B "$WEB/build-host" -G Ninja -DCMAKE_BUILD_TYPE="${WADA_BUILD_TYPE:-RelWithDebInfo}" >/dev/null
    cmake --build "$WEB/build-host" --target wadamesh-host
    if [ "$target" = run ]; then
      mkdir -p "$WEB/data"
      WADA_DATA_DIR="${WADA_DATA_DIR:-$WEB/data}" exec "$WEB/build-host/wadamesh-host" "$@"
    fi
    ;;
  shots|smoke)
    # The acceptance tour on a fresh data dir, headless: web/docs/*.png ("shots"), or a
    # scratch dir that is thrown away ("smoke": exit status only, for CI / the release gate).
    "$0" host
    tmp="$(mktemp -d)"
    out="$WEB/docs"; [ "$target" = smoke ] && out="$tmp/shots"
    WADA_DATA_DIR="$tmp/data" "$WEB/build-host/wadamesh-host" --headless \
      --script "$WEB/scripts/tour.wada" --shots "$out"
    rm -rf "$tmp"
    ;;
  web)
    echo "web/build.sh web: the Emscripten target is phase 2 (see WEB_BUILD.md)." >&2
    exit 2
    ;;
  *) echo "usage: web/build.sh host|run|shots|smoke|web" >&2; exit 2;;
esac
