#!/usr/bin/env python3
# Shared Noto colour-emoji fetch + RGB565A8 conversion.
#
# Used by add-emoji.py (bakes glyphs into the firmware, src/ui-touch/emoji_data.c)
# and gen-emoji-pack.py (builds the optional SD-card pack). Both must emit
# byte-identical pixels or a pack glyph would sit at a different baseline next to
# a baked one, so the conversion lives here once.
#
# Source art: googlefonts/noto-emoji png/128 (OFL). RGB565+alpha, swap=0.
# Pinned to v2.047: `main` dropped png/128 entirely (404s), and a moving tag would
# silently re-render already-baked glyphs on the next run.
import os, urllib.request
from PIL import Image

PX = 16
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CACHE = os.path.join(ROOT, "data", "emoji-cache")
BASE = "https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.047/png/128/emoji_u{}.png"
FLAG = (
    "https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.047"
    "/third_party/region-flags/png/{}.png"
)


def _cached(name):
    os.makedirs(CACHE, exist_ok=True)
    return os.path.join(CACHE, name)


def fetch(stem):
    fn = _cached("u_{}.png".format(stem))
    if os.path.exists(fn) and os.path.getsize(fn) > 0:
        return fn
    for url in (BASE.format(stem), BASE.format(stem + "_fe0f")):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "emoji-gen"})
            data = urllib.request.urlopen(req, timeout=20).read()
            with open(fn, "wb") as f:
                f.write(data)
            return fn
        except Exception:
            continue
    return None


def fetch_flag(iso):
    fn = _cached("flag_{}.png".format(iso))
    if os.path.exists(fn) and os.path.getsize(fn) > 0:
        return fn
    try:
        req = urllib.request.Request(
            FLAG.format(iso), headers={"User-Agent": "emoji-gen"}
        )
        data = urllib.request.urlopen(req, timeout=20).read()
        with open(fn, "wb") as f:
            f.write(data)
        return fn
    except Exception:
        return None


def to_rgb565a8(img):
    # IDENTICAL to gen-emoji-font.py: autocrop transparent margin, bottom-align to
    # the text baseline, emit [lo, hi, alpha] per pixel (LV_COLOR_16_SWAP=0).
    PAD_BOTTOM = 0
    img = img.convert("RGBA")
    bbox = img.split()[3].getbbox()
    if bbox:
        img = img.crop(bbox)
    w, h = img.size
    avail_h = PX - PAD_BOTTOM
    scale = min(PX / w, avail_h / h)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    content = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (PX, PX), (0, 0, 0, 0))
    canvas.paste(content, ((PX - nw) // 2, PX - PAD_BOTTOM - nh))
    out = bytearray()
    for r, g, b, a in canvas.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((v & 0xFF, (v >> 8) & 0xFF, a))
    return out
