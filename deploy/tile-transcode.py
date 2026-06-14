#!/usr/bin/env python3
"""
wadamesh tile transcode service.

Why this exists
---------------
The touch firmware fetches map tiles over plain HTTP (on-device HTTPS is not
workable — mbedTLS needs ~30 KB of internal heap for a handshake and only
~5 KB survives Wi-Fi association). It also can only decode JPEG cheaply: the
device's SJPG/TJpgDec path decodes straight to RGB565 in small stripes, while
PNG decode (lodepng) needs a full 256 KB ARGB8888 buffer per tile and bogged
the UI down / rendered as noise.

OpenStreetMap only serves PNG. So this service sits between nginx and OSM:
it fetches the PNG from OSM (HTTPS, with the policy-required identifying
User-Agent) and re-encodes it as JPEG, which is what the device asks for.

nginx (see tiles.wadamesh.com.conf) reverse-proxies tiles.wadamesh.com
to this service on 127.0.0.1:5005 and caches the JPEG result on disk, so OSM
is only hit on a cache miss and transcoding only happens once per tile.

Run
---
    pip install flask pillow requests
    python3 tile-transcode.py          # listens on 127.0.0.1:5005

Production: use the systemd unit (wadamesh-tile-transcode.service).
"""

import io
import sys
import time

try:
    import requests
    from flask import Flask, Response, abort, request
    from PIL import Image
except ImportError:
    sys.exit("ERROR: pip install flask pillow requests")

OSM_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
# OSM tile policy: identify yourself with a contactable UA. This is what OSM
# sees — the device never talks to OSM directly.
OSM_UA = "wadamesh-tile-proxy/1.0 (+https://wadamesh.com)"
JPEG_QUALITY = 80          # slippy tiles compress well; 80 ≈ visually lossless
MAX_TILE_BYTES = 256 * 1024
REQUEST_TIMEOUT = 12       # seconds for the OSM fetch

# Elevation backend for the line-of-sight analyzer. opentopodata's public
# SRTM 30 m dataset: max 100 locations/request, ~1 req/sec, 1000/day. Fine
# for a personal device. Self-host opentopodata if you outgrow the cap.
ELEV_URL = "https://api.opentopodata.org/v1/srtm30m"
ELEV_MAX_POINTS = 100

app = Flask(__name__)
_session = requests.Session()
_session.headers.update({"User-Agent": OSM_UA, "Accept": "image/png,image/*"})


@app.get("/<int:z>/<int:x>/<int:y>.jpg")
def tile(z: int, x: int, y: int):
    # Range-check before touching OSM so we can't be turned into an open proxy.
    if not (0 <= z <= 19):
        abort(404)
    n = 1 << z
    if not (0 <= x < n and 0 <= y < n):
        abort(404)

    try:
        r = _session.get(OSM_URL.format(z=z, x=x, y=y), timeout=REQUEST_TIMEOUT,
                         stream=True)
    except requests.RequestException:
        abort(502)

    if r.status_code != 200:
        # Pass OSM's status through (404 = empty/sea tile — nginx caches it).
        abort(r.status_code if r.status_code in (404, 429) else 502)

    raw = r.raw.read(MAX_TILE_BYTES + 1, decode_content=True)
    if len(raw) > MAX_TILE_BYTES:
        abort(502)

    try:
        img = Image.open(io.BytesIO(raw)).convert("RGB")
        out = io.BytesIO()
        img.save(out, "JPEG", quality=JPEG_QUALITY, optimize=True)
    except Exception:
        abort(502)

    return Response(out.getvalue(), mimetype="image/jpeg",
                    headers={"Cache-Control": "public, max-age=2592000"})


@app.get("/elev")
def elev():
    """Elevation profile for the line-of-sight analyzer.

    Request:  GET /elev?locations=lat,lon|lat,lon|...   (up to 100 points)
    Response: compact CSV of integer metres, one per point, in order:
              "12,15,40,38,..."   ('null' for any point with no data).

    The device computes the great-circle sample points itself; this just
    proxies them to the SRTM backend and strips the JSON down to a tiny
    body the firmware can parse without a JSON library.
    """
    locations = request.args.get("locations", "").strip()
    if not locations:
        abort(400)
    pts = locations.split("|")
    if len(pts) < 2 or len(pts) > ELEV_MAX_POINTS:
        abort(400)
    # Validate each "lat,lon" before forwarding so we can't be used to hit
    # arbitrary query strings upstream.
    for p in pts:
        try:
            lat_s, lon_s = p.split(",")
            lat, lon = float(lat_s), float(lon_s)
        except ValueError:
            abort(400)
        if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
            abort(400)

    # opentopodata rate-limits to ~1 req/sec and occasionally 5xx's. Retry a
    # couple of times with a >1 s gap so a transient limit/blip doesn't bubble
    # up to the device as a failure.
    # 2 attempts max so the proxy's worst case (~2 × (timeout + 1.2 s) ≈ 26 s)
    # stays under the device's read timeout (30 s) — otherwise the device
    # gives up mid-retry and the work is wasted.
    r = None
    for attempt in range(2):
        if attempt:
            time.sleep(1.2)
        try:
            r = _session.get(ELEV_URL, params={"locations": locations},
                             timeout=REQUEST_TIMEOUT)
        except requests.RequestException:
            r = None
            continue
        if r.status_code == 200:
            break
        if r.status_code in (429, 500, 502, 503, 504):
            continue          # transient — retry
        break                 # other 4xx — don't bother retrying
    if r is None:
        abort(502)
    if r.status_code != 200:
        abort(502 if r.status_code >= 500 or r.status_code == 429 else 400)
    try:
        data = r.json()
    except ValueError:
        abort(502)
    if data.get("status") != "OK":
        abort(502)

    out = []
    for res in data.get("results", []):
        e = res.get("elevation")
        out.append(str(int(round(e))) if e is not None else "null")
    if not out:
        abort(502)
    return Response(",".join(out), mimetype="text/plain",
                    headers={"Cache-Control": "public, max-age=2592000"})


@app.get("/healthz")
def healthz():
    return "ok\n", 200


if __name__ == "__main__":
    # threaded=True so concurrent device fetches don't serialize behind one
    # another. Bind to localhost only — nginx is the public face.
    app.run(host="127.0.0.1", port=5005, threaded=True)
