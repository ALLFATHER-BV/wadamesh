# A fake MeshCore companion node for testing the companion backend without hardware:
#   python3 web/mesh/test/mock_companion_node.py 15000 &
#   WADA_DATA_DIR=/tmp/wm web/build-host/wadamesh-host --tcp 127.0.0.1:15000
# Contacts (Alice, Bob, a repeater, a room), Public + #wadatest, two waiting messages;
# DMs are ACKed and answered, status / telemetry / trace / login / CLI answer, a channel
# post is echoed by two 'repeaters' as raw RX (needs the openssl CLI).
import socket, struct, sys, time, threading, hashlib, os
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 15000

def key(name): return hashlib.sha256(name.encode()).digest()
SELF = key('mock-self')
CONTACTS = [('Alice', 1, 0, 0xFF), ('Bob', 1, 1, 0), ('Mock Repeater', 2, 0, 0), ('Mock Room', 3, 0, 0xFF)]
def contact_frame(code, name, typ, flags, opl):
    now = int(time.time())
    return (bytes([code]) + key(name) + bytes([typ, flags, opl]) + bytes(64) + name.encode().ljust(32, b'\0')
            + struct.pack('<IiiI', now - 100, 52370000, 4890000, now))
PUBLIC = bytes.fromhex('8b3387e9c5cdea6ac9e5edbaa115cd72')
TEST = hashlib.sha256(b'#wadatest').digest()[:16]
CHANNELS = [('Public', PUBLIC), ('#wadatest', TEST)]
WAITING = [
    bytes([16, 28, 0, 0]) + key('Alice')[:6] + bytes([2, 0]) + struct.pack('<I', int(time.time()) - 600) + b'hello from the mock (waiting)',
    bytes([17, 20, 0, 0, 1, 1, 0]) + struct.pack('<I', int(time.time()) - 300) + b'Bob: channel hello (waiting)',
]

waiting = list(WAITING)   # like a node's sync cursor: delivered once, not per connection
# The node's "share position in adverts" (SELF_INFO advert_loc_policy) and every
# CMD_SET_ADVERT_LATLON it was sent: the location privacy test (node-tour.mjs) reads
# and sets these through the WebSocket mock's plain-HTTP side.
STATE = {'share': 0, 'latlon': []}

def on_connect(send, later):
    # a live channel post a few seconds after a client connects
    later(8.0, bytes([17, 22, 0, 0, 1, 1, 0]) + struct.pack('<I', int(time.time())) + b'Bob: live hello from the mock')
    later(8.05, bytes([0x83]))

def handle(f, send, later):
    cmd = f[0]
    print('mock cmd', cmd, f[1:12].hex(), flush=True)
    if cmd == 22:
        send(bytes([13, 10, 200, 8]) + bytes(4) + b'7 Jun 2026'.ljust(12, b'\0') + b'Mock Board'.ljust(40, b'\0') + b'v-mock'.ljust(20, b'\0') + bytes([0, 0]))
    elif cmd == 1:
        send(bytes([5, 1, 22, 22]) + SELF + struct.pack('<ii', 52370000, 4890000) + bytes([0, STATE['share'], 0, 0])
             + struct.pack('<II', 869618, 62500) + bytes([8, 5]) + b'Mock Node')
    elif cmd == 5:
        send(bytes([9]) + struct.pack('<I', int(time.time())))
    elif cmd == 4:
        send(bytes([2]) + struct.pack('<I', len(CONTACTS)))
        for ct in CONTACTS: send(contact_frame(3, *ct))
        send(bytes([4]) + struct.pack('<I', int(time.time())))
    elif cmd == 31:
        i = f[1]
        if i < len(CHANNELS): send(bytes([18, i]) + CHANNELS[i][0].encode().ljust(32, b'\0') + CHANNELS[i][1])
        else: send(bytes([18, i]) + bytes(48))
    elif cmd == 20:
        send(bytes([12]) + struct.pack('<HII', 4100, 10, 100))
    elif cmd == 10:
        send(waiting.pop(0) if waiting else bytes([10]))
    elif cmd == 2:   # DM: SENT, then the ACK, then a reply
        send(bytes([6, 1]) + struct.pack('<II', 0xA1B2C3D4, 5000))
        later(1.5, bytes([0x82]) + struct.pack('<II', 0xA1B2C3D4, 1500))
        if f[1] == 0:
            later(3.0, bytes([16, 30, 0, 0]) + f[7:13] + bytes([0xFF, 0]) + struct.pack('<I', int(time.time())) + b'mock reply: got it')
            later(3.1, bytes([0x83]))
        else:
            later(2.0, bytes([16, 30, 0, 0]) + f[7:13] + bytes([0xFF, 1]) + struct.pack('<I', int(time.time())) + b'> cli output from mock')
    elif cmd == 3:
        send(bytes([0]))
        idx, ts, text = f[2], f[3:7], f[7:]
        secret = CHANNELS[idx][1]
        data = ts + bytes([0]) + b'Mock Node: ' + text
        data += bytes((16 - len(data) % 16) % 16)
        import subprocess, hmac
        enc = subprocess.run(['openssl', 'enc', '-aes-128-ecb', '-nopad', '-K', secret.hex()], input=data, capture_output=True).stdout
        mac = hmac.new(secret + bytes(16), enc, hashlib.sha256).digest()[:2]
        payload = hashlib.sha256(secret).digest()[:1] + mac + enc
        for i, hop in enumerate((0xAB, 0xCD)):
            later(0.6 + i * 0.5, bytes([0x88, 24, 0xB0, 0x15, 0x01, hop]) + payload)
    elif cmd in (27, 39):   # status / telemetry
        send(bytes([6, 0]) + struct.pack('<II', 0x1234, 3000))
        pk = f[1:7] if cmd == 27 else f[4:10]
        if cmd == 27:
            later(1.0, bytes([0x87, 0]) + pk + struct.pack('<HHhhIII', 4012, 0, -118, -87, 18234, 9120, 6 * 86400) + bytes(28))
        else:
            later(1.0, bytes([0x8B, 0]) + pk + bytes([1, 116, 0x01, 0x90]))   # LPP ch1 voltage 4.00 V
    elif cmd == 36:
        tag = f[1:5]
        send(bytes([6, 0]) + tag + struct.pack('<I', 2000))
        later(0.8, bytes([0x89, 0, 1, 0]) + tag + bytes(4) + f[10:11] + bytes([24]) + bytes([20]))
    elif cmd == 26:
        send(bytes([6, 0]) + f[1:5] + struct.pack('<I', 3000))
        later(1.0, bytes([0x85, 1]) + f[1:7] + struct.pack('<I', int(time.time())) + bytes([1, 1]))
    elif cmd == 50:   # CMD_SEND_BINARY_REQ (the repeater admin app): neighbours / owner info
        tag = b'\x51\x52\x53\x54'
        send(bytes([6, 0]) + tag + struct.pack('<I', 3000))
        req = f[33:]
        if req[:1] == b'\x06':
            pl = req[6] if len(req) > 6 else 4
            body = struct.pack('<HH', 2, 2)
            for name, ago, snr in (('Alice', 60, 30), ('Bob', 900, -8)):
                body += key(name)[:pl] + struct.pack('<Ib', ago, snr)
        elif req[:1] == b'\x07':
            body = b'v-mock\nMock Repeater\nthe mock owner'
        else:
            body = b''
        if body: later(0.8, bytes([0x8C, 0]) + tag + body)
    elif cmd == 66:
        send(bytes([29]) + b'mock cli: ' + f[1:] + b'\nsecond line')
    elif cmd == 7:
        send(bytes([0]))
    elif cmd == 14:   # CMD_SET_ADVERT_LATLON
        lat, lon = struct.unpack('<ii', f[1:9])
        STATE['latlon'].append([lat / 1e6, lon / 1e6])
        send(bytes([0]))
    else:
        print('mock: unhandled', cmd, flush=True)
        send(bytes([1, 1]))


def serve_tcp(c):
    lock = threading.Lock()
    def send(p):
        with lock: c.sendall(b'>' + struct.pack('<H', len(p)) + p)
    def later(delay, p):
        threading.Timer(delay, lambda: send(p)).start()
    on_connect(send, later)
    buf = b''
    while True:
        d = c.recv(4096)
        if not d: return
        buf += d
        while len(buf) >= 3 and buf[0] == 0x3c:
            n = buf[1] | buf[2] << 8
            if len(buf) < 3 + n: break
            f, buf = buf[3:3+n], buf[3+n:]
            handle(f, send, later)

if __name__ == '__main__':
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', PORT)); s.listen(4)
    while True:
        c, _ = s.accept()
        print('mock: client', flush=True)
        threading.Thread(target=serve_tcp, args=(c,), daemon=True).start()
