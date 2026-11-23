#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build cities.bin (WadaMesh city locator) from Canadian municipal boundaries, optionally
mixed with US Census TIGER/Line places. Encoding and splitting come from make_cities.py.

Sources (all lon/lat NAD83, used as WGS84; within about 1 m):
  --qc   Quebec, MRNF "Decoupages administratifs" 1:20 000, the munic_s layer of SDA.shp.zip
         (CC-BY 4.0): https://www.donneesquebec.ca/recherche/dataset/decoupages-administratifs
  --on   Ontario, LIO "Municipal Boundary - Lower and Single Tier" (Open Government Licence -
         Ontario), a folder of GeoJSON written by the fetch-ontario command below
  --us   US places, tl_YYYY_SS_place basename (public domain), as make_cities.py; repeatable

Area: a place goes in when it touches --box, or the buffer of --buffer-km around --route.
--us-box adds an area for US places only (a band along the border, say).

  make_cities_ca.py build <out_prefix> [--qc munic_s] [--on dir] [--us tl ...]...
        (--box minlon minlat maxlon maxlat | --route="lon,lat;lon,lat;..." --buffer-km N)
        [--us-box minlon minlat maxlon maxlat] [--max-bytes N] [--tol DEG]
  make_cities_ca.py fetch-ontario <out_dir> minlon minlat maxlon maxlat [--delay S]

fetch-ontario asks the LIO ArcGIS service for the object IDs inside the box, then fetches
20 features per request, one request at a time, --delay seconds apart (default 5). It pages by
object ID on purpose: paging with resultOffset on this service skipped and repeated rows, and
large pages came back as HTML error pages.

Example, Montreal to Ottawa with 50 km on each side:
  make_cities_ca.py fetch-ontario on -76.5 44.5 -74.3 46.0
  make_cities_ca.py build cities --qc SDA/munic_s --on on --route="-73.57,45.50;-75.70,45.42" --buffer-km 50
"""
import argparse, glob, json, math, os, re, sys, time, urllib.parse, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_cities as mc
import shapefile
from shapely.geometry import shape, box, LineString
from shapely.affinity import scale
from shapely.ops import unary_union

LIO = ("https://ws.lioservices.lrc.gov.on.ca/arcgis2/rest/services/"
       "LIO_OPEN_DATA/LIO_Open03/MapServer/14/query")


# ---- area -------------------------------------------------------------------------------

def route_buffer(route, km):
    """Buffer of `km` around a polyline, in a local equirectangular km grid (a few % off at
    the far ends of a long north-south route, fine for picking places)."""
    lat0 = sum(la for _, la in route) / len(route)
    kx, ky = 111.32 * math.cos(math.radians(lat0)), 110.57
    line = LineString(route) if len(route) > 1 else LineString(route * 2)
    in_km = scale(line, kx, ky, origin=(0, 0))
    return scale(in_km.buffer(km, 64), 1 / kx, 1 / ky, origin=(0, 0))


# ---- loaders ----------------------------------------------------------------------------

SMALL = {"of", "and", "on", "the", "upon"}


def ontario_name(s):
    """LIO short names are upper case: 'DRUMMOND/NORTH ELMSLEY' -> 'Drummond/North Elmsley',
    'MCNAB/BRAESIDE' -> 'McNab/Braeside', 'FRONT OF YONGE' -> 'Front of Yonge',
    "O'CONNOR" -> "O'Connor", "BURK'S FALLS" -> "Burk's Falls"."""
    words = re.split(r"([ /\-])", s.strip().lower())
    out = []
    for i, w in enumerate(words):
        if w in " /-" or not w or (w in SMALL and i > 0):
            out.append(w)
            continue
        w = w[0].upper() + w[1:]
        w = re.sub(r"^(Mc)(\w)", lambda m: m.group(1) + m.group(2).upper(), w)
        w = re.sub(r"'(\w)(?=\w)", lambda m: "'" + m.group(1).upper(), w)   # not a possessive 's
        out.append(w)
    return "".join(out)


def load_qc(path):
    for sr in shapefile.Reader(path, encoding="utf-8").iterShapeRecords():
        if sr.shape.points:
            d = sr.record.as_dict()
            # kind 1 for unorganized territory (TNO), like a CDP: not a municipality
            yield d["MUS_NM_MUN"], 1 if d["MUS_CO_DES"] == "NO" else 0, shape(sr.shape.__geo_interface__)


def load_on(folder):
    seen = set()   # an object ID fetched twice must not become two places
    parts = {}     # LIO splits some municipalities into several features; join them into one place
    for f in sorted(glob.glob(os.path.join(folder, "*.geojson"))):
        with open(f, encoding="utf-8") as fh:
            d = json.load(fh)
        if "features" not in d:
            sys.exit(f"{f}: not a GeoJSON feature collection (an error reply?); fetch again")
        for ft in d["features"]:
            p = ft["properties"]
            if p["OBJECTID"] in seen or not ft.get("geometry"):
                continue
            seen.add(p["OBJECTID"])
            g = shape(ft["geometry"])
            parts.setdefault(ontario_name(p["MUNICIPAL_NAME_SHORTFORM"]), []).append(g if g.is_valid else g.buffer(0))
    for name, gs in parts.items():
        yield name, 0, unary_union(gs)


def load_us(path):
    for sr in shapefile.Reader(path).iterShapeRecords():
        if sr.shape.points:
            d = sr.record.as_dict()
            yield d["NAME"], 1 if d["LSAD"] == "57" else 0, shape(sr.shape.__geo_interface__)


def make_rec(name, kind, g0, tol):
    if not g0.is_valid:
        g0 = g0.buffer(0)
    t = tol
    while True:   # ring sizes are u16; simplify harder if a ring is huge
        g = g0.simplify(t, preserve_topology=True)
        rings = mc.rings_of(g)
        if rings and max(len(r) for r in rings) < 65536 and len(rings) < 65536:
            break
        t *= 2
    c = g0.representative_point()
    return (name, kind, g.bounds, rings, (c.x, c.y), g0)


# ---- commands ---------------------------------------------------------------------------

def build(a):
    if a.box:
        area = box(*a.box)
    elif a.route and a.buffer_km:
        area = route_buffer([tuple(map(float, p.split(","))) for p in a.route.split(";")], a.buffer_km)
    else:
        sys.exit("give --box, or --route with --buffer-km")
    us_area = area.union(box(*a.us_box)) if a.us_box else area
    sources = ([("QC", load_qc(a.qc), area)] if a.qc else []) + \
              ([("ON", load_on(a.on), area)] if a.on else []) + \
              [("US " + os.path.basename(u), load_us(u), us_area) for u in a.us]
    if not sources:
        sys.exit("no source given (--qc, --on, --us)")
    recs, counts = [], {}
    for label, it, ar in sources:
        for name, kind, g0 in it:
            if not g0.is_empty and g0.intersects(ar):
                recs.append(make_rec(name, kind, g0, a.tol))
                counts[label] = counts.get(label, 0) + 1
    if not recs:
        sys.exit("no places in that area")
    print("places per source:", counts)
    recs.sort(key=lambda r: (r[1], r[0]))   # municipalities first, then TNO / CDPs
    recs = [r + (mc.record_bytes(r),) for r in recs]
    mc.split_write(recs, a.out_prefix, a.max_bytes)


def get_json(url, tries=3):
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "wadamesh-make_cities_ca"})
            with urllib.request.urlopen(req, timeout=120) as r:
                d = json.loads(r.read().decode("utf-8"))
            if "error" not in d:   # ArcGIS reports errors as HTTP 200 with an "error" object
                return d
            e = d["error"]
        except (OSError, ValueError) as ex:   # HTML error pages fail the JSON parse
            e = ex
        if attempt + 1 < tries:
            print(f"  request failed ({e}); waiting 30 s", file=sys.stderr)
            time.sleep(30)
    sys.exit(f"LIO service not answering ({e}); try again later")


def fetch_ontario(a):
    os.makedirs(a.out_dir, exist_ok=True)
    q = {"where": "1=1", "geometryType": "esriGeometryEnvelope", "inSR": "4326",
         "geometry": ",".join(map(str, a.bbox)), "spatialRel": "esriSpatialRelIntersects"}
    ids = sorted(get_json(LIO + "?" + urllib.parse.urlencode({**q, "returnIdsOnly": "true", "f": "json"}))
                 .get("objectIds") or [])
    print(f"{len(ids)} boundary features in the box")
    for n, i in enumerate(range(0, len(ids), 20), 1):
        time.sleep(a.delay)   # one request at a time, spaced: it's a small public service
        chunk = ",".join(map(str, ids[i:i + 20]))
        d = get_json(LIO + "?" + urllib.parse.urlencode({
            "objectIds": chunk, "outFields": "OBJECTID,MUNICIPAL_NAME_SHORTFORM,MUNICIPAL_TYPE",
            "outSR": "4326", "f": "geojson"}))
        with open(os.path.join(a.out_dir, f"c_{n}.geojson"), "w", encoding="utf-8") as f:
            json.dump(d, f)
        print(f"  chunk {n}: {len(d.get('features', []))} features")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("out_prefix")
    b.add_argument("--qc"); b.add_argument("--on"); b.add_argument("--us", action="append", default=[])
    b.add_argument("--box", type=float, nargs=4)
    b.add_argument("--route"); b.add_argument("--buffer-km", type=float)
    b.add_argument("--us-box", type=float, nargs=4)
    b.add_argument("--max-bytes", type=int, default=mc.DEFAULT_MAX)
    b.add_argument("--tol", type=float, default=mc.DEFAULT_TOL)
    f = sub.add_parser("fetch-ontario")
    f.add_argument("out_dir"); f.add_argument("bbox", type=float, nargs=4)
    f.add_argument("--delay", type=float, default=5.0)
    a = p.parse_args(argv)
    build(a) if a.cmd == "build" else fetch_ontario(a)


if __name__ == "__main__":
    main(sys.argv[1:])
