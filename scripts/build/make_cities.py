#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build cities.bin (WadaMesh city locator) from Census TIGER/Line place boundaries.

Usage:
  make_cities.py <tl_YYYY_SS_place basename> <out.bin> [minlon minlat maxlon maxlat] [tolerance_deg]
      One file for the places inside a box (default: Sacramento-Foothills area).

  make_cities.py --state <tl_YYYY_SS_place basename> <out_prefix> [--max-bytes N] [--tol DEG]
      Every place in the shapefile. Writes <out_prefix>.bin, or, when that would be bigger
      than --max-bytes (default 4,000,000; the firmware accepts up to 4 MiB), splits the
      state in half (north/south or east/west, whichever way it is longer) and writes
      <out_prefix>-north.bin / -south.bin (or -east / -west), splitting again if needed
      (e.g. -northeast). A place that straddles a split line goes in both files.
      Prints one "FILE <name> <places> <bytes>" line per file written.

File format v1 (little-endian):
  "WCTY" u8 version=1 u8 reserved u16 count
  count x record:
    u8 kind (0 = incorporated city/town, 1 = census-designated place)
    u8 name_len, name bytes (UTF-8, no terminator)
    i32 min_lat_e6, i32 min_lon_e6, i32 max_lat_e6, i32 max_lon_e6
    u16 ring_count
    ring_count x ( u16 point_count, point_count x (i32 lat_e6, i32 lon_e6) )
Point-in-place uses even-odd over ALL rings, so holes and multi-part places work.
"""
import math, os, struct, sys
import shapefile
from shapely.geometry import shape, box

DEFAULT_BOX = [-122.3, 38.0, -120.0, 40.0]   # Sacramento-Foothills area
DEFAULT_TOL = 0.00005                         # ~5 m
DEFAULT_MAX = 4_000_000                       # firmware limit is 4 MiB (4,194,304)


def e6(v):
    return int(round(v * 1e6))


def rings_of(g):
    polys = [g] if g.geom_type == "Polygon" else [p for p in g.geoms if p.geom_type == "Polygon"]
    rings = []
    for p in polys:
        rings.append(list(p.exterior.coords))
        rings += [list(i.coords) for i in p.interiors]
    return rings


def load_places(src, tol, area=None):
    """[(name, kind, bounds, rings, centroid)] for every place (intersecting `area`)."""
    recs = []
    for sr in shapefile.Reader(src).iterShapeRecords():
        if not sr.shape.points:
            continue
        g0 = shape(sr.shape.__geo_interface__)
        if area is not None and not g0.intersects(area):
            continue
        d = sr.record.as_dict()
        t = tol
        while True:   # the format stores ring sizes as u16; simplify harder if a ring is huge
            g = g0.simplify(t, preserve_topology=True)
            rings = rings_of(g)
            if rings and max(len(r) for r in rings) < 65536 and len(rings) < 65536:
                break
            t *= 2
        c = g0.representative_point()
        recs.append((d["NAME"], 1 if d["LSAD"] == "57" else 0, g.bounds, rings, (c.x, c.y), g0))
    recs.sort(key=lambda r: (r[1], r[0]))  # cities first, then CDPs
    return recs


def record_bytes(rec):
    name, kind, (x0, y0, x1, y1), rings = rec[:4]
    nb = name.encode("utf-8")[:63]
    while True:   # don't cut a UTF-8 character in half
        try:
            nb.decode("utf-8"); break
        except UnicodeDecodeError:
            nb = nb[:-1]
    b = bytearray(struct.pack("<BB", kind, len(nb)) + nb)
    b += struct.pack("<iiiiH", e6(y0), e6(x0), e6(y1), e6(x1), len(rings))
    for ring in rings:
        b += struct.pack("<H", len(ring))
        for x, y in ring:
            b += struct.pack("<ii", e6(y), e6(x))
    return bytes(b)


def build(recs):
    assert 0 < len(recs) < 65536
    buf = bytearray(b"WCTY" + struct.pack("<BBH", 1, 0, len(recs)))
    for r in recs:
        buf += r[6] if len(r) > 6 else record_bytes(r)
    return bytes(buf)


def write(path, data, nplaces):
    with open(path, "wb") as f:
        f.write(data)
    print(f"FILE {os.path.basename(path)} {nplaces} {len(data)}")


def suffix(labels):
    """["east", "north"] -> "-northeast"; ["north", "north"] -> "-north-north"."""
    if len(labels) == 2 and {labels[0], labels[1]} & {"north", "south"} and {labels[0], labels[1]} & {"east", "west"}:
        ns = [l for l in labels if l in ("north", "south")][0]
        ew = [l for l in labels if l in ("east", "west")][0]
        return "-" + ns + ew
    return "".join("-" + l for l in labels)


def split_write(recs, prefix, max_bytes, labels=()):
    data = build(recs)
    depth = len(labels)
    if len(data) <= max_bytes:
        write(prefix + suffix(list(labels)) + ".bin", data, len(recs))
        return
    if depth >= 4 or len(recs) < 2:
        sys.exit(f"cannot split {prefix} under {max_bytes} bytes")
    # Split at the median place, across the direction the places spread out more.
    xs = sorted(r[4][0] for r in recs)
    ys = sorted(r[4][1] for r in recs)
    mid_lat = ys[len(ys) // 2]
    spread_x = (xs[-1] - xs[0]) * math.cos(math.radians(mid_lat))
    spread_y = ys[-1] - ys[0]
    if spread_y >= spread_x:
        cut = mid_lat
        halves = [("north", box(-360, cut, 360, 90)), ("south", box(-360, -90, 360, cut))]
    else:
        cut = xs[len(xs) // 2]
        halves = [("east", box(cut, -90, 360, 90)), ("west", box(-360, -90, cut, 90))]
    for label, area in halves:
        part = [r for r in recs if r[5].intersects(area)]
        split_write(part, prefix, max_bytes, labels + (label,))


def main(argv):
    if argv and argv[0] == "--state":
        src, prefix = argv[1], argv[2]
        opts = dict(zip(argv[3::2], argv[4::2]))
        max_bytes = int(opts.get("--max-bytes", DEFAULT_MAX))
        tol = float(opts.get("--tol", DEFAULT_TOL))
        recs = load_places(src, tol)
        recs = [r + (record_bytes(r),) for r in recs]   # encode each place once
        if not recs:
            sys.exit(f"no places in {src}")
        split_write(recs, prefix, max_bytes)
        return
    src, out = argv[0], argv[1]
    bb = [float(x) for x in argv[2:6]] if len(argv) >= 6 else DEFAULT_BOX
    tol = float(argv[6]) if len(argv) >= 7 else DEFAULT_TOL
    recs = load_places(src, tol, box(*bb))
    data = build(recs)
    open(out, "wb").write(data)
    print(f"{len(recs)} places, {len(data)} bytes -> {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
