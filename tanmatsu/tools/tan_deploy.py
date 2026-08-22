#!/usr/bin/env python3
"""Build the AppFS write-images for a wadamesh sideload on the Tanmatsu.

Takes the pristine AppFS partition image plus the freshly-built application.bin,
inserts/updates the "wadamesh" app inside it, then works out the MINIMUM set of
64 KB sectors that changed and writes them out as two blobs:

    <out>/appfs_app.bin   -> flash at the app data offset   (write FIRST)
    <out>/appfs_meta.bin  -> flash at the partition offset  (write LAST)

Order matters. The metadata sector is what makes the app visible to the launcher,
so writing it last means an interrupted flash leaves the previous app intact
rather than a half-written one referenced as complete.

Run AFTER `tanmatsu/build.sh build`. Normally invoked by tan_flash.sh.
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))          # <repo>/tanmatsu/tools -> <repo>

# appfs.py is badge.team's, from esp32-component-appfs. It is NOT vendored here (no
# licence header on the copy in circulation); fetch-appfs.sh clones it next to this
# script. See TANMATSU_SIDELOAD.md.
for cand in (os.path.join(HERE, "appfs-src", "tools"), os.path.join(HERE, "appfs-src")):
    if os.path.isdir(cand):
        sys.path.insert(0, cand)
try:
    from appfs import AppFS, AppFSMeta, APPFS_SECTOR_SIZE, APPFS_METADATA_SIZE
except ImportError:
    sys.exit("appfs.py not found. Run tanmatsu/tools/fetch-appfs.sh first "
             "(see TANMATSU_SIDELOAD.md).")

ap = argparse.ArgumentParser()
ap.add_argument("--pristine", default=os.path.join(HERE, "dev_appfs.bin"),
                help="dump of the device's AppFS partition BEFORE wadamesh is installed")
ap.add_argument("--app", default=os.path.join(REPO, "tanmatsu", "build", "tanmatsu", "application.bin"))
ap.add_argument("--out", default="/tmp")
ap.add_argument("--part-offset", type=lambda v: int(v, 0), default=0x420000,
                help="absolute flash offset of the appfs partition (16M.csv)")
ap.add_argument("--version", type=int, default=0, help="0 = previous+1, or 1 on a first install")
a = ap.parse_args()

for path, what in ((a.pristine, "pristine AppFS image"), (a.app, "application.bin")):
    if not os.path.exists(path):
        sys.exit(f"missing {what}: {path}")

pristine = open(a.pristine, "rb").read()
appbin = open(a.app, "rb").read()

# Version must increase or the launcher ignores the update: it keys on name+version,
# so re-publishing the same number leaves users on the old binary with no error.
prev = 0
meta_prev = os.path.join(a.out, "appfs_meta.bin")
if os.path.exists(meta_prev):
    try:
        mr = open(meta_prev, "rb").read()
        for idx in range(2):
            m = AppFSMeta(mr[idx * APPFS_METADATA_SIZE:(idx + 1) * APPFS_METADATA_SIZE], idx)
            if m.header.check_magic() and m.check_crc32():
                for p in m.pageInfo:
                    if p.get_name() == "wadamesh":
                        prev = max(prev, p.get_version())
    except Exception as e:
        print("warn: could not read the previous version:", e)
ver = a.version or ((prev + 1) if prev else 1)
print(f"app: {len(appbin)} bytes (0x{len(appbin):X})   version: {ver} (previous {prev})")

fs = AppFS(pristine)
fs.create_file("wadamesh", "WadaMesh", ver, appbin)
newimg = fs.get_data()
if len(newimg) != len(pristine):
    sys.exit("image size changed: the app does not fit this AppFS partition")

nsec = len(newimg) // APPFS_SECTOR_SIZE
changed = [i for i in range(nsec)
           if pristine[i * APPFS_SECTOR_SIZE:(i + 1) * APPFS_SECTOR_SIZE]
           != newimg[i * APPFS_SECTOR_SIZE:(i + 1) * APPFS_SECTOR_SIZE]]
data = [i for i in changed if i != 0]
if 0 not in changed:
    sys.exit("metadata sector did not change: nothing would become visible to the launcher")
if data != list(range(data[0], data[-1] + 1)):
    sys.exit("changed data sectors are not contiguous; refusing to write a partial app")

app_off = a.part_offset + data[0] * APPFS_SECTOR_SIZE
open(os.path.join(a.out, "appfs_app.bin"), "wb").write(
    newimg[data[0] * APPFS_SECTOR_SIZE:(data[-1] + 1) * APPFS_SECTOR_SIZE])
open(os.path.join(a.out, "appfs_meta.bin"), "wb").write(newimg[0:APPFS_SECTOR_SIZE])

print(f"changed sectors: meta=[0]  data=[{data[0]}..{data[-1]}] ({len(data)} sectors)")
print(f"APP  write_flash 0x{app_off:X}   {a.out}/appfs_app.bin")
print(f"META write_flash 0x{a.part_offset:X}   {a.out}/appfs_meta.bin   (write this LAST)")
print(f"APP_OFFSET=0x{app_off:X}")
