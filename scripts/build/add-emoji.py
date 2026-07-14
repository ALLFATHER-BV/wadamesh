#!/usr/bin/env python3
# Bake a few EXTRA Noto colour emoji into src/ui-touch/emoji_data.{c,h} WITHOUT
# regenerating (and re-downloading) the existing 252 — those stay byte-identical,
# so there's no Noto-`main` art drift. Splices new glyph arrays + sorted lookup
# entries into the file in place. Re-run is idempotent (new cps replace, existing
# untouched). PX must match the existing bake (16) and the imgfont in UITask.
#
#   python3 scripts/build/add-emoji.py
#
# Source art: googlefonts/noto-emoji png/128 (OFL). RGB565+alpha, swap=0.
import os, re, sys, urllib.request
from PIL import Image

PX     = 16
HERE   = os.path.dirname(os.path.abspath(__file__))
ROOT   = os.path.abspath(os.path.join(HERE, "..", ".."))
OUTDIR = os.path.join(ROOT, "src", "ui-touch")
OUTC   = os.path.join(OUTDIR, "emoji_data.c")
OUTH   = os.path.join(OUTDIR, "emoji_data.h")
CACHE  = os.path.join(ROOT, "data", "emoji-cache")
BASE   = "https://raw.githubusercontent.com/googlefonts/noto-emoji/main/png/128/emoji_u{}.png"
os.makedirs(CACHE, exist_ok=True)

# Single-codepoint emoji: (codepoint, noto-filename-stem)
SINGLE = [
    (0x26BD,  "26bd"),    # soccer ball   âš½
    (0x1F3C8, "1f3c8"),   # american football  ðŸ
    (0x1F5FF, "1f5ff"),   # moai  ðŸ—¿
    # objects/food listed in the picker (k_emoji_items) but never baked -> they
    # rendered as the notdef box on-device (the beta_38 "emoji tofu"). Bake them.
    (0x2614,  "2614"),    # umbrella with rain
    (0x2615,  "2615"),    # hot beverage
    (0x1F355, "1f355"),   # pizza
    (0x1F382, "1f382"),   # birthday cake
    (0x1F680, "1f680"),   # rocket
    (0x1F4F7, "1f4f7"),   # camera
    (0x1F4F1, "1f4f1"),   # mobile phone
    (0x1F4BB, "1f4bb"),   # laptop
    (0x1F310, "1f310"),   # globe with meridians (Web app icon)
    (0x1F4C5, "1f4c5"),   # calendar
    (0x1F4E1, "1f4e1"),   # satellite antenna
    (0x1F4E7, "1f4e7"),   # e-mail
    (0x1F3E0, "1f3e0"),   # house
    (0x1F697, "1f697"),   # car
    (0x231A,  "231a"),    # watch (sat between car + bulb in the picker, still tofu)
]
# ZWJ sequences baked as ONE combined image, keyed on the lead codepoint, with the
# trailing visible symbol(s) mapped to the zero-width glyph so the whole UTF-8
# sequence renders as the single combined emoji. The bare lead cp (a plain waving
# white flag U+1F3F3) is not offered anywhere in the UI, so reusing it as the key
# is safe. (noto-emoji drops the FE0F variation selectors from png filenames.)
SEQ = [
    # (key_cp, noto-stem, [extra cps -> zero])
    (0x1F3F3, "1f3f3_200d_26a7", [0x26A7]),   # transgender flag  ðŸ³ï¸â€âš§ï¸
]


# Country/region flags: a regional-indicator PAIR (e.g. LS = U+1F1F1 U+1F1F8) baked
# as ONE image keyed on the LEAD indicator, the trailing indicator mapped to the
# zero-width glyph (same trick as SEQ). Noto's png/128 ships no flags, so pull the
# flat rectangle from the same Noto project's third_party/region-flags. No bare
# regional indicator is ever offered in the UI, so reusing the lead cp as the key
# is safe — and the picker currently has no other flag using these indicators.
FLAGS = [
    # (lead_cp, trail_cp, ISO-3166-alpha-2)
    (0x1F1F1, 0x1F1F8, "LS"),   # Lesotho
]


def fetch(stem):
    fn = os.path.join(CACHE, "u_{}.png".format(stem))
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
    fn = os.path.join(CACHE, "flag_{}.png".format(iso))
    if os.path.exists(fn) and os.path.getsize(fn) > 0:
        return fn
    url = ("https://raw.githubusercontent.com/googlefonts/noto-emoji/main"
           "/third_party/region-flags/png/{}.png".format(iso))
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "emoji-gen"})
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
    for (r, g, b, a) in canvas.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((v & 0xFF, (v >> 8) & 0xFF, a))
    return out


# ---- bake the new glyphs ----
new_defs    = []   # (cp, ename, bytes)
new_entries = []   # (cp, ref)
for cp, stem in SINGLE:
    fn = fetch(stem)
    if not fn:
        sys.exit("FAILED to fetch U+{:X} ({})".format(cp, stem))
    new_defs.append((cp, "e_{:x}".format(cp), to_rgb565a8(Image.open(fn))))
    new_entries.append((cp, "d_{:x}".format(cp)))
for key_cp, stem, zeros in SEQ:
    fn = fetch(stem)
    if not fn:
        sys.exit("FAILED to fetch combined {} for U+{:X}".format(stem, key_cp))
    new_defs.append((key_cp, "e_{:x}".format(key_cp), to_rgb565a8(Image.open(fn))))
    new_entries.append((key_cp, "d_{:x}".format(key_cp)))
    for z in zeros:
        new_entries.append((z, "d_zero"))

for lead_cp, trail_cp, iso in FLAGS:
    fn = fetch_flag(iso)
    if not fn:
        sys.exit("FAILED to fetch flag {} for U+{:X}".format(iso, lead_cp))
    new_defs.append((lead_cp, "e_{:x}".format(lead_cp), to_rgb565a8(Image.open(fn))))
    new_entries.append((lead_cp, "d_{:x}".format(lead_cp)))
    new_entries.append((trail_cp, "d_zero"))

# ---- splice into emoji_data.c ----
src = open(OUTC).read()
TYPE_MARK = "\ntypedef struct { uint32_t cp;"
FUNC_MARK = "const lv_img_dsc_t* emojiGlyphLookup(uint32_t cp) {"
head = src[:src.index(TYPE_MARK)]                 # header + includes + all glyph defs
func = src[src.index(FUNC_MARK):]                 # the binary-search getter (verbatim)

# Idempotency: on a RE-RUN, `head` still holds the e_/d_ glyph defs spliced in by a
# PRIOR run (they live before the typedef), so blindly re-appending new_defs would
# redefine them ("redefinition of e_26bd"). Strip any def for a cp we're about to
# re-bake; the lookup table dedups separately (bycp), so this only touches defs.
for _cp, _ename, _data in new_defs:
    _hx = "{:x}".format(_cp)
    head = re.sub(r"static const uint8_t e_" + _hx + r"\[\] = [^;]*;\n", "", head)
    head = re.sub(r"static const lv_img_dsc_t d_" + _hx + r" = [^;]*;\n", "", head)

# Existing lookup entries (includes the FE0F/FE0E/200D -> d_zero rows).
existing = [(int(cp, 16), ref) for cp, ref in
            re.findall(r"\{\s*(0x[0-9A-Fa-f]+)u,\s*&(\w+)\s*\}", src)]
bycp = {cp: ref for cp, ref in existing}
for cp, ref in new_entries:                       # new cps win; never demote a real glyph to zero
    if not (ref == "d_zero" and bycp.get(cp, "d_zero") != "d_zero"):
        bycp[cp] = ref
entries = sorted(bycp.items())
count = sum(1 for _, ref in entries if ref != "d_zero")

# New glyph array + dsc definitions (appended after the existing ones).
defs = []
for cp, ename, data in new_defs:
    defs.append("static const uint8_t {}[] = {{{}}};".format(ename, ",".join(str(b) for b in data)))
    defs.append("static const lv_img_dsc_t d_{:x} = {{ {{ LV_IMG_CF_TRUE_COLOR_ALPHA, 0, 0, {}, {} }}, sizeof({}), {} }};"
                .format(cp, PX, PX, ename, ename))

out = []
out.append(head.rstrip("\n"))
out.append("")
out.append("\n".join(defs))
out.append("")
out.append("typedef struct { uint32_t cp; const lv_img_dsc_t* dsc; } EmojiGlyph;")
out.append("static const EmojiGlyph kGlyphs[] = {")
for cp, ref in entries:
    out.append("  {{ 0x{:X}u, &{} }},".format(cp, ref))
out.append("};")
out.append("const uint16_t kEmojiGlyphCount = {};".format(count))
out.append("")
out.append(func.rstrip("\n"))
out.append("")
open(OUTC, "w").write("\n".join(out))

# ---- header comment count ----
h = open(OUTH).read()
h = re.sub(r"// \d+ Noto colour emoji", "// {} Noto colour emoji".format(count), h, count=1)
open(OUTH, "w").write(h)

print("added {} glyphs -> {} total ({} lookup rows incl zero-width)".format(
    len(new_defs), count, len(entries)))
