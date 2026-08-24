#!/usr/bin/env python3
"""Sideload a Lua Store app (or any /apps or /lang file) onto a device over its
serial console.

Boards whose storage is soldered on (the ThinkNode M9) cannot take the
"drop it on the SD card" route from the SDK page, and the firmware's Store can
only download from firmware.wadamesh.com. This pushes the files through the
serial CLI instead, using three commands the firmware offers for exactly this:

    fput /apps/<name>                 open (truncate) the file
    fadd <off> <len> <sum> <base64>   append a chunk; the device checks the
                                      offset, decoded length and byte sum and
                                      answers "Error: ..." so we can re-send
    fend                              close it

Lines are kept under 128 bytes on purpose: that is the ESP32's UART hardware
FIFO, and it is all that buffers the console while the firmware is inside a
flash-cache pause (the UART interrupt is not IRAM-resident). Longer lines
lose their middle there; shorter ones just wait.

Usage:
    scripts/sideload_app.py --port /dev/cu.wchusbserial10 deploy/apps/gpscompass/1.0
    scripts/sideload_app.py --port ... --reboot deploy/apps/gpscompass/1.0/gpscompass.lua
    scripts/sideload_app.py --port ... --dest /lang deploy/apps/lang/11/de.lang
    scripts/sideload_app.py --port ... --remote /apps/audio_test.d/test.mp3 ~/Music/test.mp3

Pass an app VERSION directory to send its <id>.lua + <id>.json, or individual
files. --reboot restarts the device afterwards so the drawer/Store rescan
picks the new app up (opening the Store page also rescans).

Needs pyserial:  python3 -m pip install pyserial
(or:  uvx --from platformio --with pyserial python scripts/sideload_app.py ...)
"""
import argparse
import base64
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("Missing dependency: pyserial (python3 -m pip install pyserial)")

CHUNK = 72           # bytes per fadd line -> 96 base64 chars; whole line < 128 bytes
RETRIES = 6          # per chunk, on an "Error:" reply or a silent (lost) line


def open_port(port, baud):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.02
    s.dtr = False
    s.rts = False        # try not to reset the board; some bridges do it anyway
    s.open()
    # The CH34x bridge on the M9 resets the board on open regardless of DTR/RTS.
    # If a boot log starts streaming, wait for the UI to come up ("[BOOT] ui
    # ready" — the console is not serviced before that, and the UI init has
    # multi-second quiet gaps that a plain silence wait mistakes for "done").
    # No output within 2 s = the board did not reset and is ready now.
    t0, seen = time.time(), b""
    while time.time() - t0 < 40:
        chunk = s.read(4096)
        if chunk:
            seen += chunk
            if b"[BOOT] ui ready" in seen:
                time.sleep(1.5)
                break
        elif not seen and time.time() - t0 > 2.0:
            break
    s.reset_input_buffer()
    return s


class DeviceError(RuntimeError):
    pass


def command(s, line, expect, timeout=4.0):   # the loop stalls for ~0.5 s on GPS work
    """Send one CLI line, return the reply line that starts with `expect`.
    The console echoes what it receives, so skip the echo and any log noise.
    Raises DeviceError on an "Error:" reply, TimeoutError when nothing came."""
    s.write((line + "\n").encode())
    s.flush()
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        buf += s.read(4096)
        for raw in buf.split(b"\n"):
            text = raw.decode("utf-8", "replace").strip("\r ")
            if text.startswith(expect):
                return text
            if text.startswith("Error:"):
                raise DeviceError(text)
    raise TimeoutError("no reply to: %s" % line[:40])


def command_retry(s, line, expect):
    """fput/fend: short lines, but the same byte loss can garble them — a
    mangled line answers "Error: unknown command" or nothing at all."""
    for attempt in range(RETRIES):
        try:
            return command(s, line, expect)
        except (DeviceError, TimeoutError) as e:
            if isinstance(e, DeviceError) and "unknown command" not in str(e):
                raise
            s.write(b"\n")
            time.sleep(0.2)
    raise RuntimeError("no usable reply to: " + line)


def push(s, local_path, remote_path):
    data = open(local_path, "rb").read()
    command_retry(s, "fput " + remote_path, "ok fput")
    off, retries = 0, 0
    while off < len(data):
        piece = data[off:off + CHUNK]
        line = "fadd %d %d %d %s" % (off, len(piece), sum(piece) & 0xFFFF, base64.b64encode(piece).decode())
        try:
            reply = command(s, line, "ok fadd")
        except DeviceError as e:
            msg = str(e)
            if msg.startswith("Error: offset "):
                off = int(msg.split()[-1])        # device tells us where it really is
            retries += 1
            if retries > RETRIES:
                raise RuntimeError("giving up at offset %d: %s" % (off, msg))
            continue
        except TimeoutError:
            # a lost line leaves the device waiting for its terminator; send one
            # (an empty line is ignored by the console) and ask where it is
            s.write(b"\n")
            retries += 1
            if retries > RETRIES:
                raise RuntimeError("no reply from the device at offset %d" % off)
            continue
        retries = 0
        off = int(reply.split()[-1])
        if sys.stdout.isatty():
            sys.stdout.write("\r  %s: %d / %d bytes" % (remote_path, off, len(data)))
            sys.stdout.flush()
    reply = command_retry(s, "fend", "ok fend")
    total = int(reply.split()[2])
    if sys.stdout.isatty():
        print()
    if total != len(data):
        raise RuntimeError("device wrote %d bytes, expected %d" % (total, len(data)))
    print("  done: %s (%d bytes)" % (remote_path, total))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="app version directory, or individual files")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--dest", default="/apps", help="/apps (default) or /lang")
    ap.add_argument("--remote", help="exact remote path for one file, including /apps/<id>.d/<name>")
    ap.add_argument("--reboot", action="store_true", help="reboot the device afterwards")
    args = ap.parse_args()

    files = []
    for p in args.paths:
        if os.path.isdir(p):
            app_id = os.path.basename(os.path.dirname(os.path.abspath(p).rstrip("/")))
            for ext in ("lua", "json"):
                f = os.path.join(p, "%s.%s" % (app_id, ext))
                if os.path.isfile(f):
                    files.append(f)
            if not files:
                sys.exit("no %s.lua in %s (pass deploy/apps/<id>/<ver>)" % (app_id, p))
        elif os.path.isfile(p):
            files.append(p)
        else:
            sys.exit("not found: " + p)

    if args.remote:
        if len(files) != 1:
            sys.exit("--remote requires exactly one input file")
        if not args.remote.startswith(("/apps/", "/lang/")):
            sys.exit("--remote must begin with /apps/ or /lang/")

    s = open_port(args.port, args.baud)
    try:
        if args.remote:
            push(s, files[0], args.remote)
        else:
            for f in files:
                push(s, f, "%s/%s" % (args.dest.rstrip("/"), os.path.basename(f)))
        if args.reboot:
            s.write(b"reboot\n")
            s.flush()
            print("rebooting the device")
    finally:
        s.close()


if __name__ == "__main__":
    main()
