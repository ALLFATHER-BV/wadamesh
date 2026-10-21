# A fake MeshCore node behind the WebSocket companion server (the device's port 8765):
# the host sends '<' + len16 + payload inside one binary message, the node answers with
# the raw payload, one message per frame. Same node as mock_companion_node.py.
#   python3 web/mesh/test/mock_ws_node.py 18765
#   then connect the page (Connect > Wi-Fi) to ws://localhost:18765/
import asyncio, base64, hashlib, json, os, struct, sys, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18765
sys.argv = sys.argv[:1]
import mock_companion_node as node

async def handle(r, w):
    hdr = b''
    while b'\r\n\r\n' not in hdr:
        d = await r.read(1024)
        if not d: return
        hdr += d
    key = [l.split(b':', 1)[1].strip() for l in hdr.split(b'\r\n') if l.lower().startswith(b'sec-websocket-key')]
    if not key:   # a plain GET, as the device serves an info page; /latlon and /share/N are for the tests
        path = hdr.split(b' ', 2)[1] if hdr.count(b' ') >= 2 else b'/'
        if path.startswith(b'/share/'): node.STATE['share'] = int(path[7:8] or b'0')
        body = json.dumps(node.STATE).encode() if path != b'/' else b'mock node\n'
        w.write(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: '
                + str(len(body)).encode() + b'\r\n\r\n' + body); await w.drain(); w.close(); return
    acc = base64.b64encode(hashlib.sha1(key[0] + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
    w.write(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + acc + b'\r\n\r\n')
    loop = asyncio.get_running_loop()
    lock = threading.Lock()
    def send_now(payload):
        n = len(payload)
        hdr = bytes([0x82, n]) if n < 126 else bytes([0x82, 126]) + struct.pack('>H', n)
        w.write(hdr + payload)
    def send(payload):   # callable from any thread (the handler's timers)
        loop.call_soon_threadsafe(send_now, payload)
    def later(delay, payload):
        loop.call_later(delay, send_now, payload)
    print('mock ws: client', flush=True)
    node.on_connect(send, later)
    try:
        while True:
            h = await r.readexactly(2)
            n = h[1] & 0x7f
            if n == 126: n = struct.unpack('>H', await r.readexactly(2))[0]
            elif n == 127: n = struct.unpack('>Q', await r.readexactly(8))[0]
            mask = await r.readexactly(4) if h[1] & 0x80 else b'\0\0\0\0'
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(await r.readexactly(n)))
            op = h[0] & 0x0f
            if op == 8: break
            if op == 9: w.write(bytes([0x8A, 0])); continue
            if op != 2 or len(data) < 3 or data[0] != 0x3c: continue
            ln = data[1] | data[2] << 8
            node.handle(data[3:3 + ln], send_now, later)
            await w.drain()
    except (asyncio.IncompleteReadError, ConnectionResetError):
        pass
    print('mock ws: client gone', flush=True)
    w.close()

async def main():
    s = await asyncio.start_server(handle, '127.0.0.1', PORT)
    print('mock ws node on ws://localhost:%d/' % PORT, flush=True)
    async with s: await s.serve_forever()

asyncio.run(main())
