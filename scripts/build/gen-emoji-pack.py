#!/usr/bin/env python3
# Build the optional SD-card emoji pack. Copy the result to /emoji/emoji.pack on
# the card; the firmware loads it at boot (src/ui-touch/EmojiPack.cpp) and merges
# its glyphs and picker entries with the baked-in set.
#
#   python3 scripts/build/gen-emoji-pack.py                       # default manifest -> ./emoji.pack
#   python3 scripts/build/gen-emoji-pack.py -m my.txt -o out.pack
#   python3 scripts/build/gen-emoji-pack.py --verify out.pack     # dump an existing pack
#
# Manifest lines are "<category> <codepoints> <source>", '#' starts a comment:
#   faces     1f929        1f929        # single codepoint, noto png stem
#   activity  1f3f3+fe0f+200d+1f308  1f3f3_200d_1f308   # ZWJ sequence, one image
#   extra     1f1e9+1f1ea  flag:DE     # regional-indicator pair, flat flag art
#
# Art comes from the same pinned Noto source as the baked glyphs, converted by the
# same to_rgb565a8(), so pack art sits on the same baseline as the built-ins.
import argparse, concurrent.futures, json, os, re, struct, sys, urllib.request
from PIL import Image
from emoji_common import PX, ROOT, fetch, fetch_flag, to_rgb565a8

MAGIC = b"WMEP"
VERSION = 1
BLOB = PX * PX * 3
HEADER = 32
ZERO_OFF = 0xFFFFFFFF
MAX_FILE = 2 * 1024 * 1024  # must match the caps in src/ui-touch/EmojiPack.cpp
MAX_INDEX = 8192
MAX_ITEMS = 4096
MAX_ITEM_LEN = 32

CATS = {
    "faces": 0,
    "gestures": 1,
    "hearts": 2,
    "symbols": 3,
    "objects": 4,
    "animals": 5,
    "activity": 6,
    "special": 7,
    "food": 8,
    "places": 9,
    "flags": 10,
    "extra": 255,
}
CAT_NAMES = {v: k for k, v in CATS.items()}

DEFAULT_MANIFEST = os.path.join(ROOT, "scripts", "build", "emoji-pack.txt")
BAKED_C = os.path.join(ROOT, "src", "ui-touch", "emoji_data.c")
UITASK_CPP = os.path.join(ROOT, "src", "ui-touch", "UITask.cpp")


def die(msg):
    sys.exit("gen-emoji-pack: " + msg)


def baked_codepoints():
    try:
        src = open(BAKED_C).read()
    except OSError:
        return set()
    return {
        int(cp, 16)
        for cp, ref in re.findall(r"\{\s*(0x[0-9A-Fa-f]+)u,\s*&(\w+)\s*\}", src)
        if ref != "d_zero"
    }


def reserved_text_codepoints():
    """Codepoints the firmware renders as TEXT, taken from its own k_special_items.

    Several of them are also emoji upstream (©, ®, ™, the arrows), and the imgfont
    lookup is global: claiming U+00A9 would turn the "© OpenStreetMap" status-bar
    attribution into a colour emoji mid-sentence.
    """
    try:
        src = open(UITASK_CPP, encoding="utf-8").read()
    except OSError:
        return set()
    m = re.search(r"k_special_items\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        return set()
    out = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
        raw = re.sub(r"\\x([0-9A-Fa-f]{2})", lambda h: chr(int(h.group(1), 16)), lit)
        try:
            out.update(ord(c) for c in raw.encode("latin-1").decode("utf-8"))
        except (UnicodeDecodeError, UnicodeEncodeError):
            continue
    return out


def parse_manifest(path):
    entries = []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 3:
                die(
                    "{}:{}: expected '<category> <codepoints> <source>'".format(
                        path, lineno
                    )
                )
            cat, cps, src = parts
            if cat not in CATS:
                die(
                    "{}:{}: unknown category '{}' (one of {})".format(
                        path, lineno, cat, ", ".join(sorted(CATS))
                    )
                )
            try:
                codes = [int(c, 16) for c in cps.split("+")]
            except ValueError:
                die("{}:{}: codepoints must be '+'-joined hex".format(path, lineno))
            if not codes or any(c > 0x10FFFF for c in codes):
                die("{}:{}: codepoint out of range".format(path, lineno))
            entries.append((lineno, CATS[cat], codes, src))
    return entries


# ---- --all: enumerate the whole upstream set --------------------------------

NOTO_TREE = (
    "https://api.github.com/repos/googlefonts/noto-emoji/git/trees/v2.047?recursive=1"
)
EMOJI_TEST = "https://unicode.org/Public/emoji/15.1/emoji-test.txt"
SKIN_TONES = {0x1F3FB, 0x1F3FC, 0x1F3FD, 0x1F3FE, 0x1F3FF}
# Unicode's own grouping, one entry per picker tab.
GROUP_TO_CAT = {
    "Smileys & Emotion": "faces",
    "People & Body": "gestures",
    "Animals & Nature": "animals",
    "Food & Drink": "food",
    "Travel & Places": "places",
    "Activities": "activity",
    "Objects": "objects",
    "Symbols": "symbols",
    "Flags": "flags",
}
# Subgroup overrides where Unicode's own placement misleads a picker user:
# 🏂/🏄/🏋 are filed under People & Body, not Activities.
SUBGROUP_TO_CAT = {"heart": "hearts", "person-sport": "activity"}


def http_text(url):
    req = urllib.request.Request(url, headers={"User-Agent": "emoji-gen"})
    return urllib.request.urlopen(req, timeout=30).read().decode("utf-8")


def unicode_categories():
    """{codepoint-tuple (FE0F stripped): category} from Unicode's emoji-test.txt."""
    cats, group, sub = {}, None, None
    for line in http_text(EMOJI_TEST).splitlines():
        if line.startswith("# group:"):
            group = line.split(":", 1)[1].strip()
            continue
        if line.startswith("# subgroup:"):
            sub = line.split(":", 1)[1].strip()
            continue
        if not line or line.startswith("#") or ";" not in line:
            continue
        try:
            key = tuple(
                int(c, 16)
                for c in line.split(";", 1)[0].split()
                if int(c, 16) != 0xFE0F
            )
        except ValueError:
            continue
        cat = SUBGROUP_TO_CAT.get(sub) or GROUP_TO_CAT.get(group)
        if cat and key not in cats:
            cats[key] = cat
    return cats


def enumerate_all():
    tree = json.loads(http_text(NOTO_TREE))
    if tree.get("truncated"):
        die("upstream tree listing was truncated; cannot enumerate reliably")
    stems = sorted(
        {
            e["path"].split("/")[-1][len("emoji_u") : -len(".png")]
            for e in tree["tree"]
            if e["path"].startswith("png/128/emoji_u") and e["path"].endswith(".png")
        }
    )
    cats = unicode_categories()
    baked = baked_codepoints()
    text = reserved_text_codepoints()

    entries = []
    skipped_skin = skipped_baked = skipped_uncat = skipped_seq = skipped_text = 0
    for stem in stems:
        try:
            codes = [int(c, 16) for c in stem.split("_")]
        except ValueError:
            continue
        if any(c in SKIN_TONES for c in codes):
            skipped_skin += 1
            continue
        bare = [c for c in codes if c != 0xFE0F]
        # Glyphs are keyed on their LEAD codepoint, and essentially every ZWJ
        # sequence leads with a codepoint that is also a standalone emoji
        # (1F468 is "man" and the lead of dozens of profession sequences), so
        # including them would hijack the plain emoji. Singles only.
        if len(bare) > 1:
            skipped_seq += 1
            continue
        if codes[0] in text:
            skipped_text += 1
            continue
        cat = cats.get(tuple(bare))
        if not cat:  # Component group, or art with no Unicode entry
            skipped_uncat += 1
            continue
        # Already in the firmware: the baked art wins at lookup time, so shipping
        # it again would only cost 768 bytes and a pointless warning.
        if codes[0] in baked:
            skipped_baked += 1
            continue
        entries.append((0, CATS[cat], codes, stem))

    print(
        "upstream {} images -> {} entries (skipped {} skin-tone, {} sequences, "
        "{} baked, {} text symbols, {} uncategorised)".format(
            len(stems),
            len(entries),
            skipped_skin,
            skipped_seq,
            skipped_baked,
            skipped_text,
            skipped_uncat,
        ),
        file=sys.stderr,
    )
    return entries


def prefetch(entries, workers=12):
    """Warm the art cache in parallel; the build loop then hits disk only."""
    todo = [src for _, _, _, src in entries]
    done = [0]

    def one(src):
        r = fetch_flag(src[5:]) if src.startswith("flag:") else fetch(src)
        done[0] += 1
        if done[0] % 200 == 0:
            print("  fetched {}/{}".format(done[0], len(todo)), file=sys.stderr)
        return r

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        list(ex.map(one, todo))


def build(manifest, entries=None, quiet_dupes=False):
    if entries is None:
        entries = parse_manifest(manifest)
    if not entries:
        die("nothing to build")

    baked = baked_codepoints()
    blobs, index, items = [], {}, []
    for lineno, cat, codes, src in entries:
        png = fetch_flag(src[5:]) if src.startswith("flag:") else fetch(src)
        if not png:
            die("{}:{}: could not fetch art '{}'".format(manifest, lineno, src))
        data = to_rgb565a8(Image.open(png))
        if len(data) != BLOB:
            die(
                "{}:{}: converted to {} bytes, expected {}".format(
                    manifest, lineno, len(data), BLOB
                )
            )

        lead = codes[0]
        if lead in index:
            # Two entries keyed on the same lead codepoint would silently render
            # as whichever sorted first — the exact trap the flag pairs invite.
            die(
                "{}:{}: U+{:04X} is already the key of another entry".format(
                    manifest, lineno, lead
                )
            )
        index[lead] = (len(blobs) * BLOB, cat)
        blobs.append(data)
        if lead in baked and not quiet_dupes:
            print(
                "warning: {}:{}: U+{:04X} is already baked into the firmware; "
                "the baked art wins on-device".format(manifest, lineno, lead),
                file=sys.stderr,
            )

        for c in codes[1:]:
            prev = index.get(c)
            if prev and prev[0] != ZERO_OFF:
                die(
                    "{}:{}: U+{:04X} is a trailing codepoint here but the key of "
                    "another entry".format(manifest, lineno, c)
                )
            index[c] = (ZERO_OFF, cat)

        utf8 = "".join(chr(c) for c in codes).encode("utf-8")
        if len(utf8) > MAX_ITEM_LEN:
            die(
                "{}:{}: picker entry is {} UTF-8 bytes, max {}".format(
                    manifest, lineno, len(utf8), MAX_ITEM_LEN
                )
            )
        items.append((cat, utf8))

    idx = sorted(index.items())
    if len(idx) > MAX_INDEX:
        die("{} index entries, max {}".format(len(idx), MAX_INDEX))
    if len(items) > MAX_ITEMS:
        die("{} picker items, max {}".format(len(items), MAX_ITEMS))

    index_b = b"".join(
        struct.pack("<IIBBH", cp, off, cat, 0, 0) for cp, (off, cat) in idx
    )
    items_b = struct.pack("<H", len(items)) + b"".join(
        struct.pack("<BB", cat, len(u)) + u for cat, u in items
    )
    blob_b = b"".join(bytes(b) for b in blobs)

    items_off = HEADER + len(index_b)
    blob_off = items_off + len(items_b)
    header = MAGIC + struct.pack(
        "<HHIIIIII",
        VERSION,
        PX,
        len(blobs),
        len(idx),
        items_off,
        len(items_b),
        blob_off,
        len(blob_b),
    )
    assert len(header) == HEADER

    pack = header + index_b + items_b + blob_b
    if len(pack) > MAX_FILE:
        die(
            "pack is {} bytes, over the {} byte device limit — drop some entries".format(
                len(pack), MAX_FILE
            )
        )
    return pack, len(blobs), len(idx), len(items)


def verify(path):
    data = open(path, "rb").read()
    if len(data) < HEADER or data[:4] != MAGIC:
        die("{}: not an emoji pack".format(path))
    ver, px, glyphs, idxn, ioff, ilen, boff, blen = struct.unpack(
        "<HHIIIIII", data[4:HEADER]
    )
    print("{}: {} bytes".format(path, len(data)))
    print("  version={} px={} glyphs={} index={}".format(ver, px, glyphs, idxn))
    print("  items   off={} len={}".format(ioff, ilen))
    print("  blobs   off={} len={}".format(boff, blen))
    n = struct.unpack("<H", data[ioff : ioff + 2])[0]
    pos = ioff + 2
    per_cat, sample = {}, {}
    for _ in range(n):
        cat, ln = data[pos], data[pos + 1]
        pos += 2
        per_cat[cat] = per_cat.get(cat, 0) + 1
        sample.setdefault(cat, []).append(data[pos : pos + ln].decode("utf-8"))
        pos += ln
    for cat in sorted(per_cat):
        print(
            "  {:<9} {:4d}  {}".format(
                CAT_NAMES.get(cat, cat), per_cat[cat], " ".join(sample[cat][:12])
            )
        )


def main():
    ap = argparse.ArgumentParser(description="Build the SD-card emoji pack.")
    ap.add_argument("-m", "--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "emoji.pack"))
    ap.add_argument(
        "--all",
        action="store_true",
        help="ignore the manifest; take every upstream single-codepoint "
        "emoji that isn't a skin-tone variant or already baked in",
    )
    ap.add_argument(
        "--verify", metavar="PACK", help="dump an existing pack instead of building"
    )
    args = ap.parse_args()

    if args.verify:
        verify(args.verify)
        return

    entries = None
    if args.all:
        entries = enumerate_all()
        print("fetching art (cached in data/emoji-cache)...", file=sys.stderr)
        prefetch(entries)

    pack, glyphs, idxn, items = build(args.manifest, entries, quiet_dupes=args.all)
    with open(args.out, "wb") as f:
        f.write(pack)
    print(
        "wrote {} — {} glyphs, {} index rows, {} picker items, {} bytes".format(
            args.out, glyphs, idxn, items, len(pack)
        )
    )
    print("copy it to /emoji/emoji.pack on the SD card")


if __name__ == "__main__":
    main()
