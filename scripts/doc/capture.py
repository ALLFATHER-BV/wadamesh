#!/usr/bin/env python3
"""
Documentation screenshot capture for the DOC_CAPTURE firmware build.

Build the firmware with -DDOC_CAPTURE, flash it to the T-Deck, then run:

    python3 scripts/doc/capture.py /dev/cu.usbmodemXXXX [outdir]

The device waits for a 'G' byte, then walks every screen and streams each
framebuffer over USB as a 16-bit BMP wrapped in <<<WMSHOT ...>>> / <<<WMEND>>>
markers. This script sends 'G', reads each frame, and writes a PNG per screen
(via macOS `sips`; falls back to leaving the .bmp if sips is unavailable).
"""
import sys, os, re, time, subprocess

try:
    import serial
except ImportError:
    sys.exit("pyserial not found. Try: /Library/Developer/CommandLineTools/usr/bin/python3 -m pip install pyserial")

if len(sys.argv) < 2:
    sys.exit("usage: capture.py <serial-port> [outdir]")
port = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else "doc-shots"
os.makedirs(outdir, exist_ok=True)

ser = serial.Serial(port, 115200, timeout=2)
print(f"listening on {port} -> {outdir}/")

hdr_re = re.compile(rb"<<<WMSHOT name=(\S+) w=(\d+) h=(\d+) bytes=(\d+)>>>")

def read_exact(n):
    data = bytearray()
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            break
        data += chunk
    return bytes(data)

count = 0
started = False
last_go = 0.0
deadline = time.time() + 90        # give the device up to 90s to start
while time.time() < deadline:
    if not started and time.time() - last_go > 0.4:
        ser.write(b"G")            # nudge the device to begin the tour
        ser.flush()
        last_go = time.time()
    line = ser.readline()
    if not line:
        continue
    if b"WMTOUR START" in line:
        started = True
        print("tour started")
        continue
    if b"WMTOUR END" in line:
        print("tour ended")
        break
    m = hdr_re.search(line)
    if not m:
        continue
    name = m.group(1).decode()
    nbytes = int(m.group(4))
    data = read_exact(nbytes)
    if len(data) != nbytes:
        print(f"  ! {name}: short read {len(data)}/{nbytes}")
        continue
    bmp = os.path.join(outdir, name + ".bmp")
    png = os.path.join(outdir, name + ".png")
    with open(bmp, "wb") as f:
        f.write(data)
    try:
        subprocess.run(["sips", "-s", "format", "png", bmp, "--out", png],
                       check=True, capture_output=True)
        os.remove(bmp)
        count += 1
        print(f"  [{count:2d}] {name}.png  ({nbytes} bytes)")
    except Exception as e:
        count += 1
        print(f"  [{count:2d}] {name}.bmp  ({nbytes} bytes; sips convert failed: {e})")

ser.close()
print(f"done: {count} screenshots in {outdir}/")
