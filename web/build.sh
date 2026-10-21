#!/usr/bin/env bash
# web/build.sh host   build the native SDL2 app   -> web/build-host/wadamesh-host
# web/build.sh run    build, then start it (extra args are passed to the app)
# web/build.sh shots  run the acceptance tour headless -> web/docs/*.png
# web/build.sh smoke  the same tour, screenshots discarded (exit status only)
# web/build.sh web    the browser build (Emscripten) -> web/dist/ (static; needs COOP/COEP)
# web/build.sh web-debug  the same with -g + runtime assertions -> web/dist-debug/
# web/build.sh web-nothreads  the bundle for a page without cross-origin isolation (the
#                     http copy: no pthreads) -> web/dist-nothreads/ (web builds it too)
# web/build.sh serve  build web/dist if missing, then serve it on http://localhost:8080
#                     with the cross-origin isolation headers (PORT=... to change)
# web/build.sh webtest  the acceptance tour in headless browsers against web/dist
#                     (Playwright: npm install --prefix web playwright && npx playwright install)
#                     BROWSERS=chromium,firefox,webkit; screenshots -> web/docs/web-*.png
# web/build.sh nodetest the page connected to a mock node over WebSocket -> web/docs/node-*.png
# web/build.sh mobiletest phones and tablets (iPhone 14, Pixel 7, iPad: portrait fill, phone
#                     keyboard, turning) -> web/docs/mobile-*.png
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
  web-nothreads)
    # The bundle for a page that is not cross-origin isolated (the http copy, which reaches
    # a node's ws:// over Wi-Fi): no pthreads, the FreeRTOS tasks as cooperative fibers
    # (WADA_NO_THREADS, Asyncify). Only wadamesh.js/.wasm -> web/dist-nothreads/, served at
    # nt/ next to the page (app.js loads it when the page is not isolated).
    command -v emcmake >/dev/null || { echo "web/build.sh web-nothreads: emcmake not found (brew install emscripten)" >&2; exit 1; }
    bdir="$WEB/build-web-nothreads"; dist="$WEB/dist-nothreads"
    emcmake cmake -S "$WEB" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWADA_NO_THREADS=ON \
      -DWADA_WEB_OPT="${WADA_WEB_OPT:--O2}" >/dev/null
    cmake --build "$bdir" --target wadamesh-host
    mkdir -p "$dist"; find "$dist" -mindepth 1 -delete
    cp "$bdir/wadamesh.js" "$bdir/wadamesh.wasm" "$dist/"
    for f in wadamesh.wasm wadamesh.js; do
      gzip -9 -k -n -f "$dist/$f"
      if command -v brotli >/dev/null; then brotli -q 11 -f -k "$dist/$f"; fi
    done
    sz() { stat -f %z "$1" 2>/dev/null || stat -c %s "$1"; }
    printf 'no-threads bundle -> %s\n' "$dist"
    for f in wadamesh.wasm wadamesh.js; do printf '  %-14s %9s B  gzip %9s B\n' "$f" "$(sz "$dist/$f")" "$(sz "$dist/$f.gz")"; done
    ;;
  web|web-debug)
    # Emscripten 3.1.50+ (brew install emscripten). Same CMakeLists.txt as the host build.
    command -v emcmake >/dev/null || { echo "web/build.sh web: emcmake not found (brew install emscripten)" >&2; exit 1; }
    if [ "$target" = web ]; then bt=Release; bdir="$WEB/build-web"; dist="$WEB/dist"
    else bt=RelWithDebInfo; bdir="$WEB/build-web-debug"; dist="$WEB/dist-debug"; fi
    # The page loads the no-threads bundle (nt/) where it is not cross-origin isolated:
    # build it too, so the two always come from the same source (WADA_NO_NT=1 skips it).
    if [ "$target" = web ] && [ -z "${WADA_NO_NT:-}" ]; then "$SELF" web-nothreads; fi
    emcmake cmake -S "$WEB" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE="$bt" \
      -DWADA_WEB_OPT="${WADA_WEB_OPT:--O2}" >/dev/null
    cmake --build "$bdir" --target wadamesh-host
    mkdir -p "$dist"; find "$dist" -mindepth 1 -delete   # keep the folder: a running serve keeps serving it
    cp "$bdir/wadamesh.js" "$bdir/wadamesh.wasm" "$dist/"
    cp "$WEB/shell/app.js" "$WEB/shell/app.css" "$dist/"
    cp "$WEB/../deploy/site/wadamesh-mark.svg" "$WEB/../deploy/site/favicon.ico" \
       "$WEB/shell/manifest.webmanifest" "$dist/"
    mkdir -p "$dist/icons"   # the home-screen icons (web/shell/icons/render.mjs)
    cp "$WEB/shell/icons/"*.png "$WEB/shell/icons/app-icon.svg" "$dist/icons/"
    # Cache key: the page asks for every file with ?v=<hash>, so a CDN can keep them for good.
    nt=""; [ -f "$WEB/dist-nothreads/wadamesh.wasm" ] && nt="$WEB/dist-nothreads/wadamesh.wasm $WEB/dist-nothreads/wadamesh.js"
    # shellcheck disable=SC2086
    build="$(cat "$dist/wadamesh.wasm" "$dist/wadamesh.js" "$dist/app.js" "$dist/app.css" $nt | shasum -a 256 | cut -c1-12)"
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
  mobiletest)
    # Phones and tablets (Playwright device emulation): the canvas fills the screen at the
    # device's own size, the phone keyboard path, turning the device. web/docs/mobile-*.png
    [ -f "$WEB/dist/index.html" ] || "$SELF" web
    [ -d "$WEB/node_modules/playwright" ] || { echo "mobiletest: npm install --prefix web playwright && (cd web && npx playwright install)" >&2; exit 1; }
    port="${PORT:-8189}"
    WADA_SERVE_QUIET=1 python3 "$WEB/scripts/serve.py" "$WEB/dist" "$port" & srv=$!
    trap 'kill $srv 2>/dev/null' EXIT
    sleep 1
    node "$WEB/scripts/mobile-tour.mjs" --url "http://localhost:$port/" "$@"
    ;;
  nodetest)
    # The page connected to a node: a mock node's WebSocket server (web/mesh/test/
    # mock_ws_node.py) in place of a device, Connect > Wi-Fi, DM, live message, reload,
    # Disconnect. Screenshots -> web/docs/node-*.png
    [ -f "$WEB/dist/index.html" ] || "$SELF" web
    [ -d "$WEB/node_modules/playwright" ] || { echo "nodetest: npm install --prefix web playwright && (cd web && npx playwright install)" >&2; exit 1; }
    port="${PORT:-8188}"; wsport="${WS_PORT:-18766}"
    WADA_SERVE_QUIET=1 python3 "$WEB/scripts/serve.py" "$WEB/dist" "$port" & srv=$!
    python3 "$WEB/mesh/test/mock_ws_node.py" "$wsport" >/dev/null 2>&1 & mock=$!
    trap 'kill $srv $mock 2>/dev/null' EXIT
    sleep 1
    node "$WEB/scripts/node-tour.mjs" --url "http://localhost:$port/" --ws "localhost:$wsport" "$@"
    ;;
  *) echo "usage: web/build.sh host|run|shots|smoke|web|web-debug|web-nothreads|serve|webtest|nodetest|mobiletest" >&2; exit 2;;
esac
