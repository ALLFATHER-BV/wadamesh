#!/usr/bin/env python3
# web/build.sh serve: a local static server for web/dist with the headers the page
# needs. The build uses WebAssembly threads (SharedArrayBuffer), which browsers only
# enable on a cross-origin isolated page:
#   Cross-Origin-Opener-Policy: same-origin
#   Cross-Origin-Embedder-Policy: require-corp
# (web/deploy/nginx-snippet.conf is the same for the real host.)
#
# /tiles/... is proxied to the firmware's map tile server (DEFAULT_TILE_SERVER in
# src/helpers/esp32/TouchPrefsStore.cpp, or --tiles URL): the page fetches its map
# tiles same-origin (web/compat/wada_http.h), so the tile server needs no CORS or
# CORP headers, and the OSM-required identifying User-Agent is set here (a page
# cannot set one).
#   serve.py DIR [PORT] [--bind ADDR] [--tiles http://tiles.example]
import http.server
import os
import re
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
UA = 'wadamesh-web (https://github.com/ALLFATHER-BV/wadamesh)'
# z/x/y.jpg (+ the topo style's /opentopo prefix) and the line-of-sight elevation query
TILE_PATH = re.compile(r'^/tiles(/(?:[a-z]+/)?\d{1,2}/\d{1,7}/\d{1,7}\.(?:jpg|png)|/elev\?locations=[0-9.,|%C-]{1,1400})$')


def default_tile_server():
    src = os.path.join(HERE, '..', '..', 'src', 'helpers', 'esp32', 'TouchPrefsStore.cpp')
    try:
        m = re.search(r'DEFAULT_TILE_SERVER = "([^"]+)"', open(src, encoding='utf-8').read())
        if m:
            return m.group(1).rstrip('/')
    except OSError:
        pass
    return 'http://tiles.wadamesh.com'


TILE_UPSTREAM = default_tile_server()


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.js': 'text/javascript',
        '.mjs': 'text/javascript',
        '.svg': 'image/svg+xml',
    }

    def do_GET(self):
        if self.path.startswith('/tiles/'):
            return self.proxy_tile()
        return super().do_GET()

    def proxy_tile(self):
        if not TILE_PATH.match(self.path):
            self.send_error(404)
            return
        url = TILE_UPSTREAM + self.path[len('/tiles'):]
        req = urllib.request.Request(url, headers={'User-Agent': UA})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                body, status, ctype = r.read(), r.status, r.headers.get('Content-Type', 'application/octet-stream')
        except urllib.error.HTTPError as e:
            body, status, ctype = b'', e.code, 'text/plain'
        except (urllib.error.URLError, OSError):
            self.send_error(502, 'tile server unreachable')
            return
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        if status == 200:   # tiles change rarely: let the browser keep them (the upstream keeps 30 days)
            self.send_header('Cache-Control', 'public, max-age=1209600')
        self._tile = True
        self.end_headers()
        self.wfile.write(body)

    def send_head(self):
        # Serve the precompressed .br / .gz that build.sh writes, as nginx's
        # brotli_static / gzip_static would (so local load times are realistic).
        path = self.translate_path(self.path)
        accept = self.headers.get('Accept-Encoding', '')
        for enc, ext in (('br', '.br'), ('gzip', '.gz')):
            if enc in accept and os.path.isfile(path + ext):
                ctype = self.guess_type(path)
                f = open(path + ext, 'rb')
                size = os.fstat(f.fileno()).st_size
                self.send_response(200)
                self.send_header('Content-Type', ctype)
                self.send_header('Content-Encoding', enc)
                self.send_header('Content-Length', str(size))
                self.send_header('Vary', 'Accept-Encoding')
                self.end_headers()
                return f
        return super().send_head()

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        if not getattr(self, '_tile', False):
            self.send_header('Cache-Control', 'no-cache')
        self._tile = False
        super().end_headers()

    def log_message(self, fmt, *args):
        if os.environ.get('WADA_SERVE_QUIET'):
            return
        super().log_message(fmt, *args)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    bind = '127.0.0.1'
    if '--bind' in sys.argv:
        bind = sys.argv[sys.argv.index('--bind') + 1]
        args = [a for a in args if a != bind]
    if '--tiles' in sys.argv:
        global TILE_UPSTREAM
        TILE_UPSTREAM = sys.argv[sys.argv.index('--tiles') + 1].rstrip('/')
        args = [a for a in args if a.rstrip('/') != TILE_UPSTREAM]
    root = args[0] if args else '.'
    port = int(args[1]) if len(args) > 1 else 8080
    os.chdir(root)
    srv = http.server.ThreadingHTTPServer((bind, port), Handler)
    print(f'wadamesh web: http://{"localhost" if bind == "127.0.0.1" else bind}:{port}/  (serving {os.getcwd()}, map tiles from {TILE_UPSTREAM}, Ctrl-C stops)', flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
