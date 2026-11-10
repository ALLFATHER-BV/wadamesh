#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Repack .epub books for the Wadamesh Ebook app.

The app reads a book straight off the card with wada.fs, and the Lua SDK has no
inflate, so every file inside the .epub has to be stored uncompressed. Most
.epub files are deflated. This rewrites them with ZIP_STORED (the result is still
a valid .epub that other readers open) and renames the file to what wada.fs
accepts: A-Z a-z 0-9 . _ - only, 32 characters at most, including ".epub".

Usage:
    scripts/ebook-prep.py book.epub [more.epub ...] [-o OUTDIR] [--no-images]

Copy the results into /meshcomod/apps/ebook.d/ on the SD card (or /apps/ebook.d/
on the device's own flash). --no-images also drops pictures, which the app does
not show; it makes the file much smaller but is no longer a complete book.
"""
import argparse
import os
import re
import sys
import zipfile

MAX_NAME = 32
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".bmp")


def safe_name(path, taken):
    stem = os.path.splitext(os.path.basename(path))[0]
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", stem).strip("._-") or "book"
    stem = stem[: MAX_NAME - len(".epub")].rstrip("._-") or "book"
    name, n = stem + ".epub", 1
    while name.lower() in taken:
        n += 1
        suffix = "-%d" % n
        name = stem[: MAX_NAME - len(".epub") - len(suffix)] + suffix + ".epub"
    taken.add(name.lower())
    return name


def repack(src, dst, drop_images):
    with zipfile.ZipFile(src) as zin:
        infos = [i for i in zin.infolist() if not i.is_dir()]
        if not any(i.filename == "META-INF/container.xml" for i in infos):
            raise ValueError("no META-INF/container.xml: not an epub")
        if drop_images:
            infos = [i for i in infos if not i.filename.lower().endswith(IMAGE_EXT)]
        infos.sort(key=lambda i: i.filename != "mimetype")   # the epub spec wants it first
        with zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as zout:
            for i in infos:
                out = zipfile.ZipInfo(i.filename, date_time=i.date_time)
                out.compress_type = zipfile.ZIP_STORED
                out.external_attr = 0o644 << 16
                zout.writestr(out, zin.read(i))


def main():
    ap = argparse.ArgumentParser(description="Repack .epub files uncompressed for the Wadamesh Ebook app.")
    ap.add_argument("books", nargs="+", help=".epub files to convert")
    ap.add_argument("-o", "--outdir", default=".", help="where to write the results (default: here)")
    ap.add_argument("--no-images", action="store_true", help="drop pictures to save space")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    taken, failed = set(), 0
    for src in args.books:
        name = safe_name(src, taken)
        dst = os.path.join(args.outdir, name)
        if os.path.abspath(dst) == os.path.abspath(src):
            print("%s: already in place, skipped" % src, file=sys.stderr)
            continue
        try:
            repack(src, dst, args.no_images)
        except (OSError, ValueError, zipfile.BadZipFile) as e:
            print("%s: %s" % (src, e), file=sys.stderr)
            failed += 1
            continue
        print("%s -> %s (%d KB)" % (src, dst, (os.path.getsize(dst) + 1023) // 1024))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
