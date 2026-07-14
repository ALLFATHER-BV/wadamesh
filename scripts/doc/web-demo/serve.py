#!/usr/bin/env python3
"""
Offline demo server for the wadamesh web control panel.

Serves the extracted index.html at GET / and speaks the device's /term
WebSocket protocol with hard-coded demo data, so the real SPA renders exactly
as it does on the device — just with clean documentation data. No firmware, no
device, no third-party packages (WebSocket handshake + framing done by hand).

    python3 scripts/doc/web-demo/serve.py [port]     # default 8791

Then open http://127.0.0.1:<port>/ and screenshot. Runs in the foreground.

Protocol (mirrors WebSocketCompanionServer.cpp + handleWebDataCmd in UITask.cpp):
  * Browser opens ws://host/term, sends short text commands.
  * '@'-prefixed commands are the data API; the reply is ONE binary frame whose
    payload is 0x01 followed by JSON.
  * Any other command is a terminal CLI line; the reply is a binary frame whose
    payload is 0x00 followed by text.
"""
import base64
import hashlib
import json
import os
import re
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import demo  # noqa: E402  (local module)

INDEX = os.path.join(HERE, "index.html")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"   # RFC 6455 magic


# --------------------------------------------------------------------------- #
# Command -> demo payload dispatch. Mirrors handleWebDataCmd() exactly.
# Returns (kind, data): kind 'json' -> data is a dict (framed 0x01),
#                       kind 'text' -> data is a str  (framed 0x00),
#                       kind None   -> no reply (matches the firmware swallowing
#                                      mutation @-commands that only refresh).
# --------------------------------------------------------------------------- #
def dispatch(cmd):
    now = int(time.time())
    if not cmd:
        return (None, None)

    # Terminal CLI line (not '@') -> text reply.
    if cmd[0] != "@":
        return ("text", demo.terminal_reply(now, cmd))

    a = cmd[1:]

    def is_word(prefix):
        # matches handleWebDataCmd's "a[0]==.. && (next==0 || ' ')" tests
        return a == prefix or a.startswith(prefix + " ")

    def int_arg(after):
        # atoi of the tail starting at index `after`
        m = re.match(r"\s*(-?\d+)", a[after:])
        return int(m.group(1)) if m else 0

    if is_word("c"):
        return ("json", demo.contacts_payload(now))
    if is_word("t"):
        return ("json", demo.threads_payload(now))
    if is_word("dc"):
        return ("json", demo.discovered_payload(now))
    if is_word("sg"):
        return ("json", demo.settings_payload(now))
    if is_word("st"):
        return ("json", demo.status_payload(now))
    if a.startswith("m "):
        return ("json", demo.messages_payload(now, int_arg(2)))
    if a.startswith("oc "):
        return ("json", demo.messages_payload(now, _thread_for_contact(int_arg(3))))
    if a.startswith("oh "):
        # open channel by slot: demo channels have no thread -> empty chat keyed to slot
        slot = int_arg(3)
        return ("json", _empty_channel_chat(slot))
    if a.startswith("ic "):
        return ("json", demo.contact_info_payload(now, int_arg(3)))
    if a.startswith("sr "):
        # apply radio -> echo settings back (firmware calls webPushSettings)
        return ("json", demo.settings_payload(now))
    # Send / mutate commands: the firmware pushes a refresh (messages/threads/
    # contacts). For a static demo we re-emit the relevant view so the UI stays
    # coherent if a screenshot script drives a send, but the canned data itself
    # doesn't change.
    if a.startswith("s "):
        return ("json", demo.messages_payload(now, int_arg(2)))
    if a.startswith("sc "):
        return ("json", demo.messages_payload(now, _thread_for_contact(int_arg(3))))
    if a.startswith("sh "):
        return (None, None)
    if a.startswith("ad "):
        return ("json", demo.discovered_payload(now))          # refresh discovered
    if a[:3] in ("fv ", "bk ", "pr ", "xc "):
        return ("json", demo.contacts_payload(now))            # refresh contacts
    if a.startswith("xt ") or a.startswith("d ") or a.startswith("r "):
        return ("json", demo.threads_payload(now))             # refresh threads
    # unknown '@' -> swallow (firmware returns true without replying)
    return (None, None)


def _thread_for_contact(cidx):
    # map a contact index to its thread index using demo data (or -1)
    try:
        return demo._CONTACTS[cidx]["t"]
    except (IndexError, KeyError):
        return -1


def _empty_channel_chat(slot):
    name = "Channel"
    for ch in demo._CHANNELS:
        if ch["slot"] == slot:
            name = ch["n"]
            break
    return {"t": "m", "i": -1, "h": slot, "ch": 1, "n": name, "msgs": []}


# --------------------------------------------------------------------------- #
# Minimal WebSocket framing (server -> client is never masked; client -> server
# is always masked per RFC 6455).
# --------------------------------------------------------------------------- #
def ws_encode(payload_bytes, opcode=0x2):
    """Build a single unmasked frame. opcode 0x2 = binary (what the SPA expects)."""
    fin_op = 0x80 | opcode
    n = len(payload_bytes)
    if n < 126:
        header = struct.pack("!BB", fin_op, n)
    elif n < 65536:
        header = struct.pack("!BBH", fin_op, 126, n)
    else:
        header = struct.pack("!BBQ", fin_op, 127, n)
    return header + payload_bytes


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def ws_read_frame(sock):
    """Read one client frame. Returns (opcode, payload_bytes) or (None, None) on close."""
    hdr = _recv_exact(sock, 2)
    if not hdr:
        return (None, None)
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0f
    masked = b1 & 0x80
    length = b1 & 0x7f
    if length == 126:
        ext = _recv_exact(sock, 2)
        if ext is None:
            return (None, None)
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = _recv_exact(sock, 8)
        if ext is None:
            return (None, None)
        length = struct.unpack("!Q", ext)[0]
    mask = b"\x00\x00\x00\x00"
    if masked:
        mask = _recv_exact(sock, 4)
        if mask is None:
            return (None, None)
    payload = _recv_exact(sock, length) if length else b""
    if payload is None:
        return (None, None)
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    return (opcode, payload)


def frame_reply(kind, data):
    if kind == "json":
        return ws_encode(b"\x01" + json.dumps(data, ensure_ascii=False).encode("utf-8"))
    if kind == "text":
        return ws_encode(b"\x00" + data.encode("utf-8"))
    return None


# --------------------------------------------------------------------------- #
# HTTP / WebSocket handling on a raw socket.
# --------------------------------------------------------------------------- #
def read_http_request(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            return None, None, None
        data += chunk
        if len(data) > 65536:
            break
    head, _, _rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    if not lines:
        return None, None, None
    request_line = lines[0]
    parts = request_line.split(" ")
    method = parts[0] if parts else ""
    path = parts[1] if len(parts) > 1 else "/"
    headers = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return method, path, headers


def serve_http_index(sock):
    with open(INDEX, "rb") as f:
        body = f.read()
    resp = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/html; charset=utf-8\r\n"
        b"Content-Length: " + str(len(body)).encode() + b"\r\n"
        b"Connection: close\r\n"
        b"Cache-Control: no-store\r\n"
        b"\r\n" + body
    )
    sock.sendall(resp)


def serve_http_404(sock):
    body = b"not found"
    sock.sendall(
        b"HTTP/1.1 404 Not Found\r\nContent-Length: " + str(len(body)).encode()
        + b"\r\nConnection: close\r\n\r\n" + body
    )


def do_ws_handshake(sock, headers):
    key = headers.get("sec-websocket-key")
    if not key:
        serve_http_404(sock)
        return False
    accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
    resp = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n"
    )
    sock.sendall(resp.encode())
    return True


def handle_ws(sock):
    # Periodic status pushes, like the device's 3 s refresh, on a background thread.
    alive = {"v": True}
    lock = threading.Lock()

    def push(frame):
        if frame is None:
            return
        with lock:
            try:
                sock.sendall(frame)
            except OSError:
                alive["v"] = False

    def status_loop():
        while alive["v"]:
            time.sleep(3.0)
            if not alive["v"]:
                break
            push(frame_reply("json", demo.status_payload(int(time.time()))))

    t = threading.Thread(target=status_loop, daemon=True)
    t.start()
    try:
        while alive["v"]:
            opcode, payload = ws_read_frame(sock)
            if opcode is None:
                break
            if opcode == 0x8:          # close
                break
            if opcode == 0x9:          # ping -> pong
                push(ws_encode(payload, opcode=0xA))
                continue
            if opcode == 0xA:          # pong
                continue
            if opcode not in (0x1, 0x2):
                continue
            # strip trailing CR/LF like the firmware does
            text = payload.decode("utf-8", "replace").rstrip("\r\n")
            if not text:
                continue
            kind, data = dispatch(text)
            push(frame_reply(kind, data))
    finally:
        alive["v"] = False
        try:
            sock.close()
        except OSError:
            pass


def handle_client(sock, addr):
    try:
        method, path, headers = read_http_request(sock)
        if method is None:
            sock.close()
            return
        upgrade = headers.get("upgrade", "").lower() == "websocket"
        # the SPA connects to /term; accept any path for the WS upgrade to be lenient
        if upgrade:
            if not do_ws_handshake(sock, headers):
                return
            handle_ws(sock)
            return
        if method == "GET" and path.split("?")[0] in ("/", "/index.html"):
            serve_http_index(sock)
        else:
            serve_http_404(sock)
        sock.close()
    except (OSError, UnicodeDecodeError):
        try:
            sock.close()
        except OSError:
            pass


def main():
    port = 8791
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
        except ValueError:
            print("usage: serve.py [port]", file=sys.stderr)
            sys.exit(2)
    if not os.path.exists(INDEX):
        print("index.html missing — run: python3 %s/extract.py" % HERE, file=sys.stderr)
        sys.exit(1)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    url = "http://127.0.0.1:%d/" % port
    print("wadamesh web-demo serving at %s" % url)
    print("  (Ctrl-C to stop)")
    try:
        while True:
            client, addr = srv.accept()
            client.settimeout(30)
            threading.Thread(target=handle_client, args=(client, addr), daemon=True).start()
    except KeyboardInterrupt:
        print("\nstopping.")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
