#!/usr/bin/env python3
# web/build.sh serve: a local static server for web/dist with the headers the page
# needs. The build uses WebAssembly threads (SharedArrayBuffer), which browsers only
# enable on a cross-origin isolated page:
#   Cross-Origin-Opener-Policy: same-origin
#   Cross-Origin-Embedder-Policy: require-corp
# (web/deploy/nginx-snippet.conf is the same for the real host.)
#
# Two same-origin proxies for the firmware's own servers (web/compat/wada_http.h), so
# they need no CORS or CORP headers, and the identifying User-Agent OSM asks for is
# set here (a page cannot set one). Each only passes the paths the firmware asks for:
#   /tiles/...   -> the map tile server (DEFAULT_TILE_SERVER in
#                   src/helpers/esp32/TouchPrefsStore.cpp, or --tiles URL)
#   /fw/apps/... -> the Store, http://firmware.wadamesh.com/apps (or --store URL)
#   serve.py DIR [PORT] [--bind ADDR] [--tiles http://tiles.example] [--store http://x/apps] [--nt DIR]
# /nt/ serves the no-threads bundle (web/dist-nothreads, or --nt DIR) the page loads when
# it is not cross-origin isolated (any http:// origin other than localhost).
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
NT_DIR = None   # the no-threads bundle served at /nt/ (main(): --nt DIR, else ../dist-nothreads)
STORE_UPSTREAM = 'http://firmware.wadamesh.com/apps'
# catalogs, <id>/<ver>/<file> (code, manifest, the app's own files), language files
STORE_PATH = re.compile(r'^/fw/apps/(apps\.json|langs\.json|[A-Za-z0-9_-]{1,20}/[0-9][0-9.]{0,7}/[A-Za-z0-9_-][A-Za-z0-9._-]{0,39}'
                        r'|lang/(?:[0-9][0-9.]{0,7}/)?[a-z]{2}(?:-[a-z]{2})?\.lang)$')


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.js': 'text/javascript',
        '.mjs': 'text/javascript',
        '.svg': 'image/svg+xml',
        '.webmanifest': 'application/manifest+json',
    }

    def translate_path(self, path):
        # nt/: the no-threads bundle (web/dist-nothreads), as the site serves it at /nt/
        if NT_DIR and path.split('?', 1)[0].startswith('/nt/'):
            rel = path.split('?', 1)[0][len('/nt/'):]
            if '..' in rel or rel.startswith('/'):
                return os.path.join(NT_DIR, '__nope__')
            return os.path.join(NT_DIR, rel)
        return super().translate_path(path)

    def do_GET(self):
        if self.path.startswith('/tiles/'):
            return self.proxy(TILE_PATH, TILE_UPSTREAM, '/tiles', 'public, max-age=1209600')
        if self.path.startswith('/fw/apps/'):
            # catalogs change with every app release; a version folder never changes
            catalog = self.path.endswith('.json') and self.path.count('/') == 3
            return self.proxy(STORE_PATH, STORE_UPSTREAM, '/fw/apps',
                              'no-cache' if catalog else 'public, max-age=604800')
        return super().do_GET()

    def proxy(self, allowed, upstream, prefix, cache):
        if not allowed.match(self.path):
            self.send_error(404)
            return
        url = upstream + self.path[len(prefix):]
        req = urllib.request.Request(url, headers={'User-Agent': UA})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                body, status, ctype = r.read(), r.status, r.headers.get('Content-Type', 'application/octet-stream')
        except urllib.error.HTTPError as e:
            body, status, ctype = b'', e.code, 'text/plain'
        except (urllib.error.URLError, OSError):
            self.send_error(502, 'upstream unreachable')
            return
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        if status == 200:
            self.send_header('Cache-Control', cache)
        self._proxied = True
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
        if not getattr(self, '_proxied', False):
            self.send_header('Cache-Control', 'no-cache')
        self._proxied = False
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
    global TILE_UPSTREAM, STORE_UPSTREAM
    if '--tiles' in sys.argv:
        TILE_UPSTREAM = sys.argv[sys.argv.index('--tiles') + 1].rstrip('/')
        args = [a for a in args if a.rstrip('/') != TILE_UPSTREAM]
    if '--store' in sys.argv:
        STORE_UPSTREAM = sys.argv[sys.argv.index('--store') + 1].rstrip('/')
        args = [a for a in args if a.rstrip('/') != STORE_UPSTREAM]
    root = args[0] if args else '.'
    global NT_DIR
    nt = sys.argv[sys.argv.index('--nt') + 1] if '--nt' in sys.argv else os.path.join(root, '..', 'dist-nothreads')
    args = [a for a in args if a != nt]
    NT_DIR = os.path.abspath(nt) if os.path.isdir(nt) else None
    port = int(args[1]) if len(args) > 1 else 8080
    os.chdir(root)
    srv = http.server.ThreadingHTTPServer((bind, port), Handler)
    print(f'wadamesh web: http://{"localhost" if bind == "127.0.0.1" else bind}:{port}/  (serving {os.getcwd()}, map tiles from {TILE_UPSTREAM}, Store from {STORE_UPSTREAM}, Ctrl-C stops)', flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
