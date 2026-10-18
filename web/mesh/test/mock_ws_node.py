# A fake MeshCore WS companion server: answers DEVICE_QUERY and APP_START the way the
# device's WebSocketCompanionServer does (host sends '<'+len+payload in a binary
# message; the node replies with the raw payload per message).
import asyncio, base64, hashlib, struct, sys
PORT = int(sys.argv[1])
async def handle(r, w):
    hdr = b''
    while b'\r\n\r\n' not in hdr: hdr += await r.read(1024)
    key = [l.split(b':',1)[1].strip() for l in hdr.split(b'\r\n') if l.lower().startswith(b'sec-websocket-key')][0]
    acc = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
    w.write(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + acc + b'\r\n\r\n')
    def send(payload):
        n = len(payload)
        w.write(bytes([0x82, n]) + payload if n < 126 else bytes([0x82, 126]) + struct.pack('>H', n) + payload)
    while True:
        h = await r.readexactly(2)
        n = h[1] & 0x7f
        if n == 126: n = struct.unpack('>H', await r.readexactly(2))[0]
        mask = await r.readexactly(4)
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(await r.readexactly(n)))
        if h[0] & 0x0f == 8: break
        assert data[0] == 0x3c, data
        ln = data[1] | data[2] << 8
        frame = data[3:3+ln]
        print('mock got cmd', frame[0], 'len', ln, flush=True)
        if frame[0] == 22:
            send(bytes([13, 27, 200, 40]) + bytes(4) + b'7 Jun 2026'.ljust(12, b'\0') + b'Mock Board'.ljust(40, b'\0') + b'v-mock'.ljust(20, b'\0') + bytes([0, 1]))
        await w.drain()
    w.close()
async def main():
    s = await asyncio.start_server(handle, '127.0.0.1', PORT)
    async with s: await s.serve_forever()
asyncio.run(main())
