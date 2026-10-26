#!/usr/bin/env python3
"""Write PMTiles v3 test archives for test/test_pmtiles.cpp.

Everything is written here from the v3 spec (no pmtiles package, no network):
the 127-byte header, varint directories (delta tile ids, run lengths, lengths,
offsets with the "0 = follows the previous entry" rule), gzip directories,
leaf directories and the Hilbert tile id. The Hilbert id is the textbook xy2d,
written independently of src/ui-touch/PmTiles.cpp so the test can catch a
mismatch between the two.

Usage: python3 -I test/pmtiles/make_fixtures.py <out_dir>
Writes <out_dir>/<name>.pmtiles plus <out_dir>/<name>.expect, one line per
addressed tile: "z x y length crc32" (crc32 of the exact bytes the reader must
return). Corrupt archives get an empty .expect: every lookup must fail cleanly.
Also writes <out_dir>/sd/maps/<name>/<name>.pmtiles copies for a card image.
"""
import gzip
import io
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------- primitives

def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def xy2d(n, x, y):
    """Textbook Hilbert curve index (Wikipedia xy2d) on an n x n grid."""
    d = 0
    s = n // 2
    while s > 0:
        rx = 1 if (x & s) else 0
        ry = 1 if (y & s) else 0
        d += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                x = n - 1 - x
                y = n - 1 - y
            x, y = y, x
        s //= 2
    return d


def tile_id(z, x, y):
    return sum(4 ** i for i in range(z)) + xy2d(1 << z, x, y)


def png_chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def make_png(z, x, y, sea=False):
    """A 256x256 RGB tile: a colour per zoom, a darker 16 px grid, and a
    bright square whose position encodes x/y, so map shots are readable."""
    w = h = 256
    if sea:
        base = (150, 190, 230)
    else:
        palette = [(240, 200, 120), (200, 230, 160), (250, 170, 170), (190, 180, 240),
                   (160, 220, 220), (230, 230, 150), (220, 180, 220), (180, 220, 180)]
        base = palette[z % len(palette)]
    grid = bytes(max(0, c - 60) for c in base)
    base = bytes(base)
    mx = 32 + (x % 6) * 32
    my = 32 + (y % 6) * 32
    plain = bytearray(base * w)
    for px in list(range(0, w, 64)) + [w - 1]:
        plain[px * 3:px * 3 + 3] = grid
    line = bytes(grid * w)
    marked = bytearray(plain)
    if not sea:
        marked[mx * 3:(mx + 24) * 3] = b"\x1e\x1e\x1e" * 24
    rows = bytearray()
    for py in range(h):
        rows.append(0)
        if py % 64 == 0 or py == h - 1:
            rows += line
        elif my <= py < my + 24:
            rows += marked
        else:
            rows += plain
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    text = b"wada\x00z%d x%d y%d" % (z, x, y)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) + png_chunk(b"tEXt", text) +
            png_chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + png_chunk(b"IEND", b""))


def gz(data, name=None, extra=None, comment=None, hcrc=False):
    """gzip member written by hand so the optional header fields can be set."""
    flags = 0
    hdr_tail = b""
    if extra is not None:
        flags |= 0x04
        hdr_tail += struct.pack("<H", len(extra)) + extra
    if name is not None:
        flags |= 0x08
        hdr_tail += name + b"\x00"
    if comment is not None:
        flags |= 0x10
        hdr_tail += comment + b"\x00"
    head = b"\x1f\x8b\x08" + bytes([flags | (0x02 if hcrc else 0)]) + b"\x00\x00\x00\x00\x00\xff" + hdr_tail
    if hcrc:
        head += struct.pack("<H", zlib.crc32(head) & 0xFFFF)
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = co.compress(data) + co.flush()
    return head + body + struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF)


def serialize_dir(entries):
    """entries: list of (tile_id, offset, length, run_length), sorted by id."""
    out = bytearray(varint(len(entries)))
    last = 0
    for e in entries:
        out += varint(e[0] - last)
        last = e[0]
    for e in entries:
        out += varint(e[3])
    for e in entries:
        out += varint(e[2])
    for i, e in enumerate(entries):
        if i > 0 and e[1] == entries[i - 1][1] + entries[i - 1][2]:
            out += varint(0)
        else:
            out += varint(e[1] + 1)
    return bytes(out)


def build(tiles, *, min_z, max_z, internal=2, tile_comp=1, tile_type=2,
          leaf_size=None, clustered=True, gz_kwargs=None):
    """tiles: {(z, x, y): bytes}. Returns (archive bytes, expect lines)."""
    by_id = sorted((tile_id(z, x, y), (z, x, y), data) for (z, x, y), data in tiles.items())
    # Tile data: dedupe identical contents; clustered = written in tile id order.
    order = by_id if clustered else list(reversed(by_id))
    blob = bytearray()
    where = {}
    for _, _, data in order:
        if data not in where:
            where[data] = (len(blob), len(data))
            blob += data
    # Entries, merging consecutive ids with the same content into runs.
    entries = []
    for tid, _, data in by_id:
        off, ln = where[data]
        if entries and entries[-1][0] + entries[-1][3] == tid and entries[-1][1] == off:
            e = entries[-1]
            entries[-1] = (e[0], e[1], e[2], e[3] + 1)
        else:
            entries.append((tid, off, ln, 1))

    def comp(raw):
        if internal == 1:
            return raw
        return gz(raw, **(gz_kwargs or {}))

    leaves = bytearray()
    if leaf_size:
        root_entries = []
        for i in range(0, len(entries), leaf_size):
            chunk = entries[i:i + leaf_size]
            data = comp(serialize_dir(chunk))
            root_entries.append((chunk[0][0], len(leaves), len(data), 0))
            leaves += data
        root = comp(serialize_dir(root_entries))
    else:
        root = comp(serialize_dir(entries))
    meta = comp(b'{"name":"wada test","format":"png"}')

    root_off = 127
    meta_off = root_off + len(root)
    leaf_off = meta_off + len(meta)
    data_off = leaf_off + len(leaves)
    header = bytearray(b"PMTiles\x03")
    header += struct.pack("<QQQQQQQQ", root_off, len(root), meta_off, len(meta),
                          leaf_off, len(leaves), data_off, len(blob))
    header += struct.pack("<QQQ", len(by_id), len(entries), len(where))
    header += bytes([1 if clustered else 0, internal, tile_comp, tile_type, min_z, max_z])
    header += struct.pack("<iiii", -1800000000, -850511287, 1800000000, 850511287)
    header += bytes([min_z]) + struct.pack("<ii", 0, 0)
    assert len(header) == 127, len(header)
    archive = bytes(header) + root + meta + bytes(leaves) + bytes(blob)
    expect = ["%d %d %d %d %d" % (z, x, y, len(d), zlib.crc32(d) & 0xFFFFFFFF)
              for _, (z, x, y), d in by_id]
    return archive, expect, dict(root_off=root_off, root_len=len(root), leaf_off=leaf_off,
                                 leaf_len=len(leaves), data_off=data_off, data_len=len(blob))


def world(min_z, max_z, sea_ids=None):
    tiles = {}
    for z in range(min_z, max_z + 1):
        n = 1 << z
        for x in range(n):
            for y in range(n):
                sea = sea_ids is not None and sea_ids(z, x, y)
                tiles[(z, x, y)] = make_png(z, x, y, sea=sea) if not sea else SEA
    return tiles


SEA = make_png(0, 0, 0, sea=True)

# ---------------------------------------------------------------- fixtures

def region(z_lo, z_hi, lat, lon, radius):
    """Tiles around lat/lon, `radius` tiles each way, for z_lo..z_hi."""
    import math
    tiles = {}
    for z in range(z_lo, z_hi + 1):
        n = 1 << z
        tx = int((lon + 180.0) / 360.0 * n)
        lr = math.radians(lat)
        ty = int((1.0 - math.asinh(math.tan(lr)) / math.pi) / 2.0 * n)
        for x in range(tx - radius, tx + radius + 1):
            for y in range(ty - radius, ty + radius + 1):
                if 0 <= x < n and 0 <= y < n:
                    tiles[(z, x, y)] = make_png(z, x, y)
    return tiles


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    written = {}

    def put(name, data, expect):
        with open(os.path.join(out, name + ".pmtiles"), "wb") as f:
            f.write(data)
        with open(os.path.join(out, name + ".expect"), "w") as f:
            f.write("\n".join(expect) + ("\n" if expect else ""))
        written[name] = data

    # z0-4, root directory only, clustered, a run of identical "sea" tiles at
    # z3 (ids 21+10 .. 21+25 are one entry with run_length 16).
    sea = lambda z, x, y: z == 3 and 10 <= xy2d(8, x, y) <= 25
    root, exp_root, lay_root = build(world(0, 4, sea), min_z=0, max_z=4)
    put("raster_root", root, exp_root)

    # z0-5 with 64-entry leaf directories and tile data in REVERSE id order
    # (not clustered), so every offset is explicit rather than "follows".
    leaf, exp_leaf, lay_leaf = build(world(0, 5), min_z=0, max_z=5, leaf_size=64, clustered=False)
    put("raster_leaf", leaf, exp_leaf)

    # gzip members with FEXTRA + FNAME + FCOMMENT + FHCRC set: the header
    # parser has to skip all of them.
    gzf, exp_gzf, _ = build(world(0, 2), min_z=0, max_z=2,
                            gz_kwargs=dict(name=b"dir.bin", extra=b"xy\x02\x00ab", comment=b"c", hcrc=True))
    put("raster_gzip_fields", gzf, exp_gzf)

    # Uncompressed directories (internal compression 1 = none), allowed by the spec.
    plain, exp_plain, _ = build(world(0, 3), min_z=0, max_z=3, internal=1, leaf_size=16)
    put("raster_plain_dirs", plain, exp_plain)

    # z10-12 around the web demo's own position (Amsterdam), for map shots.
    reg, exp_reg, _ = build(region(10, 12, 52.3728, 4.8934, 4), min_z=10, max_z=12, leaf_size=128)
    put("raster_z10_12", reg, exp_reg)

    # ---- archives that must be refused at the header check
    small = world(0, 1)
    vec, _, _ = build({k: gzip.compress(b"\x1a\x00mvt") for k in small}, min_z=0, max_z=1,
                      tile_comp=2, tile_type=1)
    put("vector_mvt", vec, [])
    jpg, _, _ = build({k: b"\xff\xd8\xff\xe0jpeg\xff\xd9" for k in small}, min_z=0, max_z=1, tile_type=3)
    put("raster_jpeg", jpg, [])
    gzt, _, _ = build(small, min_z=0, max_z=1, tile_comp=2)
    put("raster_gzip_tiles", gzt, [])
    brotli, _, _ = build(small, min_z=0, max_z=1, internal=3)
    put("brotli_dirs", brotli, [])
    v2 = bytearray(root); v2[7] = 2
    put("not_v3", bytes(v2), [])
    put("short_header", root[:100], [])
    put("truncated", root[:len(root) // 2], [])
    zr = bytearray(root); zr[100] = 9; zr[101] = 4      # min_zoom > max_zoom
    put("zoom_inverted", bytes(zr), [])

    # ---- archives that pass the header check but are corrupt inside: every
    # lookup must come back "not found / corrupt", never crash or hang.
    r0, rl = lay_root["root_off"], lay_root["root_len"]
    bad = bytearray(root); bad[r0 + rl - 6] ^= 0xFF        # gzip CRC32 wrong
    put("bad_crc", bytes(bad), [])
    bad = bytearray(root); struct.pack_into("<I", bad, r0 + rl - 4, 10)   # ISIZE too small
    put("isize_short", bytes(bad), [])
    bad = bytearray(root); struct.pack_into("<I", bad, r0 + rl - 4, 0x7FFFFFFF)   # ISIZE huge
    put("isize_huge", bytes(bad), [])
    bad = bytearray(root); bad[r0 + 12] ^= 0x55              # deflate stream damaged
    put("deflate_damaged", bytes(bad), [])

    def with_root(base, lay, raw_dir, internal=2):
        """Replace the root directory with raw_dir (gzipped), fixing offsets."""
        d = gz(raw_dir) if internal == 2 else raw_dir
        hdr = bytearray(base[:127])
        old_len = lay["root_len"]
        delta = len(d) - old_len
        struct.pack_into("<Q", hdr, 16, len(d))
        for off in (24, 40, 56):
            v = struct.unpack_from("<Q", hdr, off)[0]
            struct.pack_into("<Q", hdr, off, v + delta)
        return bytes(hdr) + d + base[127 + old_len:]

    # Entry count claims 2^40 entries in a few bytes.
    put("huge_count", with_root(root, lay_root, varint(1 << 40) + b"\x00" * 16), [])
    # Entry count is plausible for the bytes but the fields run out early.
    put("short_dir", with_root(root, lay_root, varint(4) + varint(1) * 9), [])
    # Tile id deltas overflow uint64.
    put("id_overflow", with_root(root, lay_root,
                                 varint(2) + varint((1 << 64) - 1) + varint(5) + varint(1) * 2 +
                                 varint(10) * 2 + varint(1) * 2), [])
    # A single tile entry pointing past the end of the tile data section.
    put("tile_oob", with_root(root, lay_root,
                              serialize_dir([(0, lay_root["data_len"] - 4, 100, 1)])), [])
    # A leaf entry pointing past the leaf section.
    put("leaf_oob", with_root(leaf, lay_leaf,
                              serialize_dir([(0, lay_leaf["leaf_len"] + 10, 50, 0)])), [])
    # A leaf directory whose only entry points at itself: the depth limit must stop it.
    L = 20
    for _ in range(20):   # fixed point: the entry's length is the leaf's own size
        loop_dir = gz(serialize_dir([(0, 0, L, 0)]))
        if len(loop_dir) == L:
            break
        L = len(loop_dir)
    assert len(loop_dir) == L
    rootd = gz(serialize_dir([(0, 0, L, 0)]))
    meta = gz(b"{}")
    tiles_blob = SEA
    hdr = bytearray(leaf[:127])
    struct.pack_into("<QQQQQQQQ", hdr, 8, 127, len(rootd), 127 + len(rootd), len(meta),
                     127 + len(rootd) + len(meta), L, 127 + len(rootd) + len(meta) + L, len(tiles_blob))
    loop = bytes(hdr) + rootd + meta + loop_dir + tiles_blob
    put("leaf_loop", loop, [])
    # Garbage after the directory's last field (cursor != end).
    put("dir_trailing", with_root(root, lay_root, serialize_dir([(0, 0, 10, 1)]) + b"\x00"), [])

    # A card image: sd/maps/<name>/<name>.pmtiles for the good archives, plus a
    # misnamed one and a vector one, so the picker's problem rows show up.
    for name in ("raster_root", "raster_leaf", "raster_z10_12"):
        d = os.path.join(out, "sd", "maps", name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name + ".pmtiles"), "wb") as f:
            f.write(written[name])
    d = os.path.join(out, "sd", "maps", "misnamed"); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "other.pmtiles"), "wb") as f:
        f.write(written["raster_root"])
    d = os.path.join(out, "sd", "maps", "vector"); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "vector.pmtiles"), "wb") as f:
        f.write(written["vector_mvt"])
    print("make_fixtures: %d archives in %s" % (len(written), out))


if __name__ == "__main__":
    main()
