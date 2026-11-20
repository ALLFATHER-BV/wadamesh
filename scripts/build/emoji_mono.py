#!/usr/bin/env python3
# Black-and-white emoji art for the e-paper boards, shared by gen-emoji-mono.py
# (bakes glyphs into the firmware) and gen-emoji-pack.py --mono (SD-card pack).
#
# Source: Noto Emoji, Google's monochrome outline font (OFL 1.1), rendered large,
# scaled to the 16 px cell and thresholded to black or nothing. Art the font does
# not have (flags, ZWJ sequences) is error-diffused from the colour PNG instead.
# The output pixel format is the colour one (RGB565+alpha, swap=0) with every
# pixel black or fully clear, so nothing that draws an emoji needs to know which
# set it got.
import hashlib, os, sys, urllib.request
from PIL import Image, ImageDraw, ImageFont
from emoji_common import PX, CACHE

FONT_URL = (
    "https://github.com/google/fonts/raw/b979dba422e445492b0eb9951ac52ee0b4d648c3"
    "/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
)
FONT_SHA256 = "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"
BIG = 256  # render size before the reduction to PX
DEFAULT_WEIGHT = 400
DEFAULT_THRESHOLD = 110


def font_path():
    os.makedirs(CACHE, exist_ok=True)
    fn = os.path.join(CACHE, "NotoEmoji-wght.ttf")
    if not os.path.exists(fn):
        req = urllib.request.Request(FONT_URL, headers={"User-Agent": "emoji-gen"})
        with open(fn, "wb") as f:
            f.write(urllib.request.urlopen(req, timeout=60).read())
    if hashlib.sha256(open(fn, "rb").read()).hexdigest() != FONT_SHA256:
        sys.exit("Noto Emoji font hash mismatch: delete " + fn + " and retry")
    return fn


def load_font(weight):
    """(font, codepoints the font has a glyph for)."""
    from fontTools.ttLib import TTFont

    font = ImageFont.truetype(font_path(), BIG)
    font.set_variation_by_axes([weight])
    return font, set(TTFont(font_path()).getBestCmap())


def fit(mask):
    # Same placement as emoji_common.to_rgb565a8: crop to the ink, scale to fit the
    # cell, centre horizontally and sit on the bottom edge.
    bbox = mask.getbbox()
    if not bbox:
        return None
    mask = mask.crop(bbox)
    w, h = mask.size
    s = min(PX / w, PX / h)
    nw, nh = max(1, round(w * s)), max(1, round(h * s))
    out = Image.new("L", (PX, PX), 0)
    out.paste(mask.resize((nw, nh), Image.LANCZOS), ((PX - nw) // 2, PX - nh))
    return out


def font_glyph(font, cp, threshold):
    big = Image.new("L", (BIG * 2, BIG * 2), 0)
    ImageDraw.Draw(big).text((BIG // 2, BIG // 2), chr(cp), font=font, fill=255)
    m = fit(big)
    return m.point(lambda v: 255 if v >= threshold else 0) if m else None


def decode_colour(arr):
    # Glyph bytes: [lo, hi, alpha] per pixel, RGB565 little-endian.
    img = Image.new("RGBA", (PX, PX))
    px = []
    for i in range(0, len(arr), 3):
        v = arr[i] | (arr[i + 1] << 8)
        px.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63,
                   (v & 31) * 255 // 31, arr[i + 2]))
    img.putdata(px)
    return img


def dithered(arr):
    img = decode_colour(arr)
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    grey = Image.alpha_composite(bg, img).convert("L")
    dark = grey.convert("1").convert("L").point(lambda v: 255 if v == 0 else 0)  # Floyd-Steinberg
    alpha = img.split()[3].point(lambda a: 255 if a >= 128 else 0)
    return Image.composite(dark, Image.new("L", (PX, PX), 0), alpha)


def to_bytes(mask):
    out = bytearray()
    for a in mask.tobytes():
        out += bytes((0, 0, 255 if a else 0))  # RGB565 black, alpha 0 or 255
    return out
