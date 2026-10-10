#!/usr/bin/env python3
# Mirror of the validation in src/ui-touch/EmojiPack.cpp parseLoaded(), so a pack
# can be cleared (or blamed) on the host instead of on the device.
#
#   python3 scripts/build/check-emoji-pack.py emoji.pack
import struct, sys

BLOB, HEADER, IDX_ENTRY, ZERO = 768, 32, 12, 0xFFFFFFFF
MAX_FILE, MAX_INDEX, MAX_ITEMS, MAX_ITEM_LEN = 2 * 1024 * 1024, 8192, 4096, 32
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
    return cond


def utf8_ok(b):
    try:
        s = b.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return all(not (0xD800 <= ord(c) <= 0xDFFF) for c in s)


def main(path):
    data = open(path, "rb").read()
    print("file: {} ({} bytes)".format(path, len(data)))
    if not check(HEADER <= len(data) <= MAX_FILE, "size out of range"):
        return
    if not check(data[:4] == b"WMEP", "bad magic"):
        return
    ver, px, glyphs, idxn, ioff, ilen, boff, blen = struct.unpack(
        "<HHIIIIII", data[4:HEADER]
    )
    print(
        "  version={} px={} glyphs={} index={} items@{}+{} blobs@{}+{}".format(
            ver, px, glyphs, idxn, ioff, ilen, boff, blen
        )
    )

    check(ver == 1, "version != 1")
    check(px == 16, "px != 16")
    check(0 < glyphs <= MAX_FILE // BLOB, "glyph count out of range")
    check(
        0 < idxn <= MAX_INDEX,
        "index count out of range ({} > {})".format(idxn, MAX_INDEX),
    )
    check(
        blen % BLOB == 0 and blen // BLOB == glyphs,
        "blob length disagrees with glyph count",
    )
    check(HEADER + idxn * IDX_ENTRY <= len(data), "index runs past EOF")
    check(HEADER <= ioff and ioff + ilen <= len(data), "items section out of bounds")
    check(HEADER <= boff and boff + blen <= len(data), "blob section out of bounds")
    if fails:
        return

    prev, zeros = -1, 0
    for i in range(idxn):
        cp, off = struct.unpack(
            "<II", data[HEADER + i * IDX_ENTRY : HEADER + i * IDX_ENTRY + 8]
        )
        if not check(cp <= 0x10FFFF, "index[{}] codepoint out of range".format(i)):
            return
        if not check(
            cp > prev, "index[{}] not strictly increasing (U+{:04X})".format(i, cp)
        ):
            return
        prev = cp
        if off == ZERO:
            zeros += 1
        elif not check(
            off % BLOB == 0 and off <= blen - BLOB,
            "index[{}] blob offset {} invalid".format(i, off),
        ):
            return
    print("  index ok: {} rows, {} zero-width".format(idxn, zeros))

    check(ilen >= 2, "items section too short")
    n = struct.unpack("<H", data[ioff : ioff + 2])[0]
    check(n <= MAX_ITEMS, "item count out of range ({} > {})".format(n, MAX_ITEMS))
    if fails:
        return
    pos = 2
    for i in range(n):
        if not check(pos + 2 <= ilen, "item[{}] header past section".format(i)):
            return
        ln = data[ioff + pos + 1]
        pos += 2
        if not check(
            0 < ln <= MAX_ITEM_LEN and pos + ln <= ilen,
            "item[{}] length {} invalid".format(i, ln),
        ):
            return
        if not check(
            utf8_ok(data[ioff + pos : ioff + pos + ln]), "item[{}] bad UTF-8".format(i)
        ):
            return
        pos += ln
    print("  items ok: {}".format(n))
    print(
        "  PSRAM needed: ~{} KB".format(
            (len(data) + idxn * 4 + idxn * 12 + n * 5) // 1024
        )
    )


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "emoji.pack")
    print(
        "FAILED: " + "; ".join(fails)
        if fails
        else "PASS - the device would accept this pack"
    )
