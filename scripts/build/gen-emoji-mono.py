#!/usr/bin/env python3
# Generate src/ui-touch/emoji_data_mono.c: the same emoji as emoji_data.c, drawn as
# hard black-and-white 16 px art for the e-paper boards (T-Deck Pro and Max).
#
# The colour set goes through a 1-bit panel as soft greys that dither into mush.
# Here every glyph is rendered from Noto Emoji, Google's monochrome outline font
# (OFL 1.1), scaled to the 16 px cell and thresholded to black or nothing.
#
#   python3 scripts/build/gen-emoji-mono.py [--weight 400] [--threshold 110]
#
# The glyph set, the sorted lookup table and the sequence table are copied from
# emoji_data.c, so the two files can never disagree about which emoji exist. The
# pixel format is also the same (RGB565+alpha, swap=0; black or fully clear), so
# nothing that draws an emoji needs to know which file it got.
#
# Flags and the trans-flag composite are bitmaps, not font glyphs: they are
# error-diffused from the colour art already baked into emoji_data.c.
#
# Needs Pillow. Output is deterministic for a given font file, weight and threshold.
import argparse, hashlib, os, re, sys, urllib.request
from PIL import Image, ImageDraw, ImageFont
from emoji_common import PX, ROOT, CACHE

FONT_URL = (
    "https://github.com/google/fonts/raw/b979dba422e445492b0eb9951ac52ee0b4d648c3"
    "/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
)
FONT_SHA256 = "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"
OUTDIR = os.path.join(ROOT, "src", "ui-touch")
SRC_C = os.path.join(OUTDIR, "emoji_data.c")
OUT_C = os.path.join(OUTDIR, "emoji_data_mono.c")
BIG = 256  # render size before the reduction to PX


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
    # emoji_data.c glyph bytes: [lo, hi, alpha] per pixel, RGB565 little-endian.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weight", type=int, default=400, help="Noto Emoji wght axis, 300-700")
    ap.add_argument("--threshold", type=int, default=110, help="ink cut-off, 1-254")
    args = ap.parse_args()

    src = open(SRC_C).read()
    cut = src.index("\ntypedef struct { uint32_t cp;")
    colour = {int(cp, 16): bytes(int(b) for b in body.split(","))
              for cp, body in re.findall(r"static const uint8_t e_([0-9a-f]+)\[\] = \{([^}]*)\};", src)}
    tail = src[cut:]

    font = ImageFont.truetype(font_path(), BIG)
    font.set_variation_by_axes([args.weight])
    from fontTools.ttLib import TTFont
    have = set(TTFont(font_path()).getBestCmap())

    # Glyphs the colour file keys by a lead codepoint but that are really bitmaps.
    bitmap = {cp for cp in colour if cp not in have} | {0x1F1F1, 0x1F3F3}
    defs, from_font, from_bitmap, empty = [], 0, 0, []
    for cp in sorted(colour):
        if cp in bitmap:
            mask, from_bitmap = dithered(colour[cp]), from_bitmap + 1
        else:
            mask, from_font = font_glyph(font, cp, args.threshold), from_font + 1
        if mask is None or not mask.getbbox():
            empty.append(hex(cp))
            continue
        name = "e_{:x}".format(cp)
        defs.append("static const uint8_t {}[] = {{{}}};".format(name, ",".join(map(str, to_bytes(mask)))))
        defs.append("static const lv_img_dsc_t d_{:x} = {{ {{ LV_IMG_CF_TRUE_COLOR_ALPHA, 0, 0, {}, {} }}, sizeof({}), {} }};"
                    .format(cp, PX, PX, name, name))
    if empty:
        sys.exit("no ink for: " + " ".join(empty))

    head = [
        "// AUTO-GENERATED by scripts/build/gen-emoji-mono.py - do not edit.",
        "// Black-and-white twin of emoji_data.c for the e-paper boards. Noto Emoji (OFL 1.1),",
        "// wght {}, threshold {}. Compiled only when WADA_EMOJI_MONO is set.".format(args.weight, args.threshold),
        "#if defined(WADA_EMOJI_MONO) && WADA_EMOJI_MONO",
        '#include "emoji_data.h"',
        "#include <string.h>",
        "",
        "static const uint8_t e_zero[] = {0,0,0, 0,0,0, 0,0,0, 0,0,0};",
        "static const lv_img_dsc_t d_zero = { { LV_IMG_CF_TRUE_COLOR_ALPHA, 0, 0, 2, 2 }, sizeof(e_zero), e_zero };",
        "",
    ]
    with open(OUT_C, "w") as f:
        f.write("\n".join(head) + "\n".join(defs) + "\n" + tail.rstrip("\n") + "\n\n#endif  // WADA_EMOJI_MONO\n")
    print("wrote {}: {} font glyphs, {} bitmap glyphs, wght {}, threshold {}".format(
        os.path.relpath(OUT_C, ROOT), from_font, from_bitmap, args.weight, args.threshold))


main()
