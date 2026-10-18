#!/usr/bin/env python3
# web/build.sh serve: a local static server for web/dist with the headers the page
# needs. The build uses WebAssembly threads (SharedArrayBuffer), which browsers only
# enable on a cross-origin isolated page:
#   Cross-Origin-Opener-Policy: same-origin
#   Cross-Origin-Embedder-Policy: require-corp
# (web/deploy/nginx-snippet.conf is the same for the real host.)
#   serve.py DIR [PORT] [--bind ADDR]
import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.js': 'text/javascript',
        '.mjs': 'text/javascript',
        '.svg': 'image/svg+xml',
    }

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
        self.send_header('Cache-Control', 'no-cache')
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
    root = args[0] if args else '.'
    port = int(args[1]) if len(args) > 1 else 8080
    os.chdir(root)
    srv = http.server.ThreadingHTTPServer((bind, port), Handler)
    print(f'wadamesh web: http://{"localhost" if bind == "127.0.0.1" else bind}:{port}/  (serving {os.getcwd()}, Ctrl-C stops)', flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
