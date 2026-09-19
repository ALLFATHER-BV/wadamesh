#!/usr/bin/env python3
"""USB Files from the command line: the same protocol files.wadamesh.com speaks.

Open USB Files on the device first (app drawer), then:

    scripts/usbfiles.py info
    scripts/usbfiles.py ls /sd/tiles
    scripts/usbfiles.py get /internal/meshcore-backup.json backup.json
    scripts/usbfiles.py put notes.txt /sd/transfer/notes.txt [--overwrite]
    scripts/usbfiles.py rm /sd/transfer/notes.txt
    scripts/usbfiles.py mkdir /sd/transfer/new
    scripts/usbfiles.py mv /sd/transfer/a.txt /sd/transfer/b.txt
    scripts/usbfiles.py df /internal
    scripts/usbfiles.py selftest            # round trips, access rules, throughput

Protocol: src/helpers/esp32/UsbFilesProtocol.h. The port is found automatically
(the first /dev/cu.usbmodem*, /dev/ttyACM* or COM port) unless --port is given.
"""
import argparse
import binascii
import glob
import json
import os
import random
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("Missing dependency: pyserial (python3 -m pip install pyserial)", file=sys.stderr)
    sys.exit(2)

MAGIC = b"\xE7\x5A"
T_HELLO, T_LIST, T_STAT, T_READ = 0x01, 0x02, 0x03, 0x04
T_WRITE_BEGIN, T_WRITE_DATA, T_WRITE_END = 0x05, 0x06, 0x07
T_REMOVE, T_MKDIR, T_RENAME, T_SPACE, T_ABORT, T_PING = 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D
T_BEACON, T_BYE, T_REPLY = 0x40, 0x41, 0x80
STATUS = {
    0: "ok", 1: "bad request", 2: "not found", 3: "exists", 4: "protected",
    5: "no space", 6: "i/o error", 7: "bad path", 8: "busy", 9: "too big",
    10: "bad checksum", 11: "bad offset", 12: "unsupported", 13: "no media",
    14: "not empty",
}
S_OK, S_NOT_FOUND, S_EXISTS, S_PROTECTED, S_BAD_PATH, S_BAD_OFFSET = 0, 2, 3, 4, 7, 11
MAX_PAYLOAD = 4096 + 256


class DeviceError(Exception):
    def __init__(self, status, raw):
        self.status = status
        self.raw = raw   # the reply body after the status byte
        message = "" if status == S_BAD_OFFSET else raw.decode("utf-8", "replace")
        super().__init__("%s: %s" % (STATUS.get(status, "status %d" % status), message))


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def encode(ftype, seq, payload=b""):
    head = struct.pack("<BHH", ftype, seq, len(payload))
    return MAGIC + head + payload + struct.pack("<I", crc32(head + payload))


class Link:
    """Frames over a serial port, with the device's log lines skipped."""

    def __init__(self, port, verbose=False):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.buf = bytearray()
        self.verbose = verbose
        self.seq = random.randint(1, 60000)
        self.hello = None

    def close(self):
        self.ser.close()

    def _frames(self):
        """Yield complete, CRC-valid frames from the buffer (the device's parser, in Python)."""
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                keep = 1 if self.buf[-1:] == b"\xE7" else 0
                self._noise(self.buf[: len(self.buf) - keep])
                del self.buf[: len(self.buf) - keep]
                return
            if i:
                self._noise(self.buf[:i])
                del self.buf[:i]
            if len(self.buf) < 7:
                return
            ftype, seq, length = struct.unpack_from("<BHH", self.buf, 2)
            if length > MAX_PAYLOAD:
                del self.buf[:1]
                continue
            total = 7 + length + 4
            if len(self.buf) < total:
                return
            body = bytes(self.buf[2:7 + length])
            (crc,) = struct.unpack_from("<I", self.buf, 7 + length)
            if crc != crc32(body):
                del self.buf[:1]
                continue
            payload = bytes(self.buf[7:7 + length])
            del self.buf[:total]
            yield ftype, seq, payload

    def _noise(self, data):
        if self.verbose and data:
            sys.stderr.write("[device] " + data.decode("utf-8", "replace"))

    def poll(self, timeout):
        """All frames that arrive within timeout seconds (returns early on the first)."""
        end = time.time() + timeout
        out = []
        while time.time() < end and not out:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf.extend(chunk)
                out.extend(self._frames())
        return out

    def wait_beacon(self, timeout=15.0):
        timeout = getattr(self, "beacon_wait", timeout)
        end = time.time() + timeout
        while time.time() < end:
            for ftype, _seq, payload in self.poll(0.5):
                if ftype == T_BEACON:
                    return json.loads(payload.decode())
        raise SystemExit("No beacon: open USB Files on the device (app drawer), then try again.")

    def request(self, ftype, payload=b"", timeout=3.0, retries=3):
        self.seq = self.seq % 65535 + 1
        frame = encode(ftype, self.seq, payload)
        for _attempt in range(retries + 1):
            self.ser.write(frame)
            end = time.time() + timeout
            while time.time() < end:
                for rtype, rseq, body in self.poll(end - time.time()):
                    if rtype == T_BYE:
                        raise SystemExit("The device closed USB Files.")
                    if rtype == (ftype | T_REPLY) and rseq == self.seq:
                        status = body[0] if body else 6
                        if status != S_OK:
                            raise DeviceError(status, body[1:])
                        return body[1:]
        raise SystemExit("No reply from the device (request 0x%02x)." % ftype)

    def connect(self):
        beacon = self.wait_beacon()
        self.hello = json.loads(self.request(T_HELLO, bytes([1])).decode())
        return beacon, self.hello

    # ---- operations ----
    def ls(self, path):
        out, start = [], 0
        while True:
            reply = json.loads(self.request(T_LIST, struct.pack("<IH", start, 64) + path.encode()).decode())
            out.extend(reply["e"])
            if reply["n"] < 0:
                break
            start = reply["n"]
        merged, seen = [], set()
        for entry in out:
            name, is_dir, size, writable = entry[:4]
            threat = entry[4] if len(entry) > 4 else 0
            key = (name, is_dir)
            if key in seen:
                continue
            seen.add(key)
            merged.append((name, bool(is_dir), size, bool(writable), threat))
        return merged

    def stat(self, path):
        return json.loads(self.request(T_STAT, path.encode()).decode())

    def get(self, path):
        size = self.stat(path)["s"]
        data = bytearray()
        chunk = int(self.hello.get("chunk", 4096))
        while len(data) < size:
            body = self.request(T_READ, struct.pack("<IH", len(data), chunk) + path.encode())
            (offset,) = struct.unpack_from("<I", body, 0)
            if offset != len(data):
                raise SystemExit("device returned offset %d, expected %d" % (offset, len(data)))
            data.extend(body[4:])
            if len(body) == 4:
                break
        return bytes(data)

    def put(self, path, data, overwrite=False):
        self.request(T_WRITE_BEGIN, struct.pack("<IB", len(data), 1 if overwrite else 0) + path.encode())
        chunk = int(self.hello.get("chunk", 4096))
        offset = 0
        while offset < len(data):
            piece = data[offset:offset + chunk]
            try:
                body = self.request(T_WRITE_DATA, struct.pack("<I", offset) + piece)
            except DeviceError as e:
                if e.status != S_BAD_OFFSET or len(e.raw) < 4:
                    raise
                (offset,) = struct.unpack_from("<I", e.raw, 0)   # where the device wants us
                continue
            (offset,) = struct.unpack_from("<I", body, 0)
        self.request(T_WRITE_END, struct.pack("<I", crc32(data)), timeout=10.0)

    def rm(self, path):
        self.request(T_REMOVE, path.encode())

    def mkdir(self, path):
        self.request(T_MKDIR, path.encode())

    def mv(self, src, dst):
        s = src.encode()
        self.request(T_RENAME, struct.pack("<H", len(s)) + s + dst.encode())

    def df(self, root):
        return json.loads(self.request(T_SPACE, root.encode()).decode())


def find_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/cu.wchusbserial*", "/dev/ttyUSB*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return ("%d %s" % (n, unit)) if unit == "B" else ("%.1f %s" % (n, unit))
        n /= 1024.0


def expect_error(link, status, fn, *args):
    try:
        fn(*args)
    except DeviceError as e:
        if e.status == status:
            return
        raise SystemExit("FAIL: expected %s, got %s" % (STATUS[status], e))
    raise SystemExit("FAIL: expected %s, the device accepted it" % STATUS[status])


def selftest(link):
    roots = {r["id"]: r for r in link.hello["roots"]}
    print("roots:", ", ".join("%s (%s)" % (k, "ok" if v["ok"] else "unavailable") for k, v in roots.items()))
    targets = []
    if roots.get("sd", {}).get("ok"):
        targets.append("/sd/transfer")
    if roots.get("internal", {}).get("ok"):
        targets.append("/internal/transfer")
    if roots.get("tiles", {}).get("ok"):
        targets.append("/tiles/usbfiles-test")
    for folder in targets:
        link.mkdir(folder)
        for size in (0, 1, 4095, 4096, 4097, 50000):
            data = os.urandom(size)
            path = "%s/t%d.bin" % (folder, size)
            try:
                link.rm(path)
            except DeviceError:
                pass
            link.put(path, data)
            back = link.get(path)
            assert back == data, "round trip mismatch for %s" % path
            assert link.stat(path)["s"] == size
            expect_error(link, S_EXISTS, link.put, path, b"x")
            link.put(path, b"replaced", overwrite=True)
            assert link.get(path) == b"replaced"
            link.mv(path, path + ".moved")
            expect_error(link, S_NOT_FOUND, link.stat, path)
            link.rm(path + ".moved")
        names = [e[0] for e in link.ls(folder)]
        assert not any(n.startswith("t") and n.endswith(".bin") for n in names), names
        print("  %-22s round trips ok" % folder)
    if "tiles" in roots and roots["tiles"]["ok"]:
        link.rm("/tiles/usbfiles-test")
    # Access rules: live data is download-only, paths cannot escape.
    if roots.get("internal", {}).get("ok"):
        expect_error(link, S_PROTECTED, link.put, "/internal/contacts3", b"x", True)
        expect_error(link, S_PROTECTED, link.rm, "/internal/new_prefs")
        expect_error(link, S_PROTECTED, link.put, "/internal/prefs/x.kv", b"x")
    if roots.get("sd", {}).get("ok"):
        expect_error(link, S_PROTECTED, link.put, "/sd/meshcomod/x", b"x")
        expect_error(link, S_PROTECTED, link.put, "/sd/MESHCOMOD/x", b"x")
        expect_error(link, S_PROTECTED, link.put, "/sd/MESHCO~1/x", b"x")
        expect_error(link, S_PROTECTED, link.rm, "/sd/meshcomod")
    expect_error(link, S_BAD_PATH, link.stat, "/sd/../internal/contacts3")
    expect_error(link, S_BAD_PATH, link.stat, "/nope/x")
    for rid, r in roots.items():
        if r["ok"]:
            expect_error(link, S_PROTECTED, link.rm, "/" + rid)   # a storage itself
    print("  access rules ok")
    # Malware flags: SD Scan's rules, by name in listings and by content in stat.
    for folder in targets[:1]:
        pe = bytearray(128)
        pe[0:2] = b"MZ"
        pe[0x3C:0x40] = struct.pack("<I", 64)   # e_lfanew -> the PE signature
        pe[64:68] = b"PE\0\0"
        samples = {"autorun.inf": b"[autorun]\r\n", "photo.jpg": bytes(pe), "notes.txt": b"hello"}
        for name, data in samples.items():
            link.put(folder + "/" + name, data, overwrite=True)
        flags = {e[0]: e[4] for e in link.ls(folder)}
        assert flags.get("autorun.inf") == 1 and flags.get("notes.txt") == 0, flags
        assert flags.get("photo.jpg") == 0, flags            # a harmless name...
        assert link.stat(folder + "/photo.jpg")["t"] == 5    # ...caught by its content
        assert link.stat(folder + "/notes.txt")["t"] == 0
        for name in samples:
            link.rm(folder + "/" + name)
        print("  malware flags ok")
    # Throughput, on the first writable target.
    if targets:
        data = os.urandom(512 * 1024)
        path = targets[0] + "/speed.bin"
        try:
            link.rm(path)
        except DeviceError:
            pass
        t0 = time.time()
        link.put(path, data)
        up = time.time() - t0
        t0 = time.time()
        back = link.get(path)
        down = time.time() - t0
        link.rm(path)
        assert back == data
        print("  512 KB to %s: up %.0f KB/s, down %.0f KB/s" % (targets[0], 512 / up, 512 / down))
    print("selftest passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first USB serial port found)")
    ap.add_argument("-v", "--verbose", action="store_true", help="show the device's log lines")
    ap.add_argument("command", choices=["info", "ls", "stat", "get", "put", "rm", "mkdir", "mv", "df", "selftest"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--wait", type=float, default=15.0, help="seconds to wait for USB Files to open")
    a = ap.parse_args()

    port = a.port or find_port()
    if not port:
        raise SystemExit("No USB serial port found. Is the device plugged in?")
    link = Link(port, a.verbose)
    link.beacon_wait = a.wait
    try:
        beacon, hello = link.connect()
        if a.command == "info":
            print(json.dumps(hello, indent=2))
        elif a.command == "ls":
            path = a.args[0] if a.args else "/"
            risk = {1: "autorun file", 2: "Windows program", 3: "Windows script",
                    4: "Windows shortcut", 5: "renamed Windows program"}
            for name, is_dir, size, writable, threat in link.ls(path):
                print("%s %10s  %s%s%s" % ("rw" if writable else "r-", "<dir>" if is_dir else human(size),
                                           name, "/" if is_dir else "",
                                           ("   !! MALWARE RISK: " + risk.get(threat, "?")) if threat else ""))
        elif a.command == "stat":
            print(json.dumps(link.stat(a.args[0])))
        elif a.command == "get":
            data = link.get(a.args[0])
            dest = a.args[1] if len(a.args) > 1 else os.path.basename(a.args[0])
            with open(dest, "wb") as f:
                f.write(data)
            print("%s -> %s (%s)" % (a.args[0], dest, human(len(data))))
        elif a.command == "put":
            with open(a.args[0], "rb") as f:
                data = f.read()
            link.put(a.args[1], data, a.overwrite)
            print("%s -> %s (%s)" % (a.args[0], a.args[1], human(len(data))))
        elif a.command == "rm":
            link.rm(a.args[0])
        elif a.command == "mkdir":
            link.mkdir(a.args[0])
        elif a.command == "mv":
            link.mv(a.args[0], a.args[1])
        elif a.command == "df":
            d = link.df(a.args[0])
            print("%s used of %s, %s kept free for the device" % (human(d["u"]), human(d["t"]), human(d["r"])))
        elif a.command == "selftest":
            selftest(link)
    except DeviceError as e:
        raise SystemExit("device: %s" % e)
    finally:
        link.close()


if __name__ == "__main__":
    main()
