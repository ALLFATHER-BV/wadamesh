#!/usr/bin/env bash
# web/build.sh host   build the native SDL2 app   -> web/build-host/wadamesh-host
# web/build.sh run    build, then start it (extra args are passed to the app)
# web/build.sh shots  run the acceptance tour headless -> web/docs/*.png
# web/build.sh smoke  the same tour, screenshots discarded (exit status only)
# web/build.sh web    the browser build (Emscripten) -> web/dist/ (static; needs COOP/COEP)
# web/build.sh web-debug  the same with -g + runtime assertions -> web/dist-debug/
# web/build.sh serve  build web/dist if missing, then serve it on http://localhost:8080
#                     with the cross-origin isolation headers (PORT=... to change)
# web/build.sh webtest  the acceptance tour in headless browsers against web/dist
#                     (Playwright: npm install --prefix web playwright && npx playwright install)
#                     BROWSERS=chromium,firefox,webkit; screenshots -> web/docs/web-*.png
set -euo pipefail
cd "$(dirname "$0")"
WEB="$(pwd)"
SELF="$WEB/$(basename "$0")"   # "$0" may be relative to where we were called from
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
    "$SELF" host
    tmp="$(mktemp -d)"
    out="$WEB/docs"; [ "$target" = smoke ] && out="$tmp/shots"
    WADA_DATA_DIR="$tmp/data" "$WEB/build-host/wadamesh-host" --headless \
      --script "$WEB/scripts/tour.wada" --shots "$out"
    rm -rf "$tmp"
    ;;
  web|web-debug)
    # Emscripten 3.1.50+ (brew install emscripten). Same CMakeLists.txt as the host build.
    command -v emcmake >/dev/null || { echo "web/build.sh web: emcmake not found (brew install emscripten)" >&2; exit 1; }
    if [ "$target" = web ]; then bt=Release; bdir="$WEB/build-web"; dist="$WEB/dist"
    else bt=RelWithDebInfo; bdir="$WEB/build-web-debug"; dist="$WEB/dist-debug"; fi
    emcmake cmake -S "$WEB" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE="$bt" \
      -DWADA_WEB_OPT="${WADA_WEB_OPT:--O2}" >/dev/null
    cmake --build "$bdir" --target wadamesh-host
    mkdir -p "$dist"; find "$dist" -mindepth 1 -delete   # keep the folder: a running serve keeps serving it
    cp "$bdir/wadamesh.js" "$bdir/wadamesh.wasm" "$dist/"
    cp "$WEB/shell/app.js" "$WEB/shell/app.css" "$dist/"
    cp "$WEB/../deploy/site/wadamesh-mark.svg" "$WEB/../deploy/site/favicon.ico" "$dist/"
    # Cache key: the page asks for every file with ?v=<hash>, so a CDN can keep them for good.
    build="$(cat "$dist/wadamesh.wasm" "$dist/wadamesh.js" "$dist/app.js" "$dist/app.css" | shasum -a 256 | cut -c1-12)"
    sed "s/__WADA_BUILD__/$build/g" "$WEB/shell/index.html" > "$dist/index.html"
    sed -i.bak "s/__WADA_BUILD__/$build/g" "$dist/app.js" && rm -f "$dist/app.js.bak"
    for f in wadamesh.wasm wadamesh.js app.js app.css index.html; do
      gzip -9 -k -n -f "$dist/$f"
      if command -v brotli >/dev/null; then brotli -q 11 -f -k "$dist/$f"; fi
    done
    sz() { stat -f %z "$1" 2>/dev/null || stat -c %s "$1"; }
    printf 'web build %s (%s, %s) -> %s\n' "$build" "$bt" "${WADA_WEB_OPT:--O2}" "$dist"
    for f in wadamesh.wasm wadamesh.js; do
      printf '  %-14s %9s B  gzip %9s B' "$f" "$(sz "$dist/$f")" "$(sz "$dist/$f.gz")"
      [ -f "$dist/$f.br" ] && printf '  brotli %9s B' "$(sz "$dist/$f.br")"
      printf '\n'
    done
    ;;
  serve)
    [ -f "$WEB/dist/index.html" ] || "$SELF" web
    exec python3 "$WEB/scripts/serve.py" "$WEB/dist" "${PORT:-8080}" "$@"
    ;;
  webtest)
    [ -f "$WEB/dist/index.html" ] || "$SELF" web
    [ -d "$WEB/node_modules/playwright" ] || { echo "webtest: npm install --prefix web playwright && (cd web && npx playwright install)" >&2; exit 1; }
    port="${PORT:-8187}"
    WADA_SERVE_QUIET=1 python3 "$WEB/scripts/serve.py" "$WEB/dist" "$port" & srv=$!
    trap 'kill $srv 2>/dev/null' EXIT
    sleep 1
    node "$WEB/scripts/web-tour.mjs" --url "http://localhost:$port/" --browsers "${BROWSERS:-chromium}" "$@"
    ;;
  *) echo "usage: web/build.sh host|run|shots|smoke|web|web-debug|serve|webtest" >&2; exit 2;;
esac
