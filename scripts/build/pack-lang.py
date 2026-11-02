#!/usr/bin/env python3
"""Slim the served .lang files in a staging copy of deploy/apps (deploy-apps.sh runs this).

The device downloads a language into ONE memory buffer of at most 128 KB (96 or 72 KB
when PSRAM is short) and calls anything larger "Server or connection failed". The v30
files all grew past 128 KB, so no language could be downloaded (2026-10-27). The files in
the repo stay complete for translators; only the copies the Store serves are slimmed:

  1. keep the header lines (# code / # ver / # name / ...), which the device reads;
  2. keep only the LAST row per key ("last row wins" on the device too);
  3. drop rows whose translation is empty or the English key itself: the device falls
     back to the key, so such a row changes nothing on screen;
  4. still over SOFT_CAP: drop keys the current firmware can never look up
     (scripts/build/lang_reach.py), so a fully translated file fits.

A file still over HARD_CAP aborts the deploy rather than shipping a language no device
can download.

Usage: pack-lang.py <staging apps dir> <repo root>
"""
import glob
import importlib.util
import os
import sys

SOFT_CAP = 120 * 1024   # aim: room under the 128 KB download buffer
HARD_CAP = 128 * 1024   # the device's largest buffer; above this nothing downloads


def parse(path):
    lines = open(path, encoding="utf-8").read().split("\n")
    header = [l for l in lines if "\t" not in l and l.startswith("# ") and ":" in l]
    final, order = {}, []
    for l in lines:
        if "\t" in l:
            k, v = l.split("\t", 1)
            if k not in final:
                order.append(k)
            final[k] = v
    return header, final, order


def render(header, final, order, keep=None):
    rows = [k + "\t" + final[k] for k in order
            if final[k] and final[k] != k and (keep is None or k in keep)]
    return ("\n".join(header + rows) + "\n").encode("utf-8")


def main():
    staging, root = sys.argv[1], sys.argv[2]
    reach = None
    files = sorted(glob.glob(os.path.join(staging, "lang", "*.lang")) +
                   glob.glob(os.path.join(staging, "lang", "*", "*.lang")))
    too_big = []
    for path in files:
        before = os.path.getsize(path)
        header, final, order = parse(path)
        out = render(header, final, order)
        if len(out) > SOFT_CAP:
            if reach is None:
                spec = importlib.util.spec_from_file_location(
                    "lang_reach", os.path.join(root, "scripts", "build", "lang_reach.py"))
                reach = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(reach)
            keep = reach.reachable_keys(root, set(final))
            out = render(header, final, order, keep)
        with open(path, "wb") as f:
            f.write(out)
        rel = os.path.relpath(path, staging)
        if len(out) > HARD_CAP:
            too_big.append("%s %d B" % (rel, len(out)))
        print("lang: %-18s %7d -> %7d B" % (rel, before, len(out)))
    if too_big:
        sys.exit("ABORT: still over the device's %d B download buffer: %s" % (HARD_CAP, ", ".join(too_big)))


if __name__ == "__main__":
    main()
