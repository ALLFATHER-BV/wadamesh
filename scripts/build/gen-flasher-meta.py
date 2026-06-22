#!/usr/bin/env python3
# Generate the web-flasher metadata that must roll with every release:
#   - version.json            : {tag, notes[]} — the site shows this ("what's new")
#   - manifest-tdeck.json      : esp-web-tools manifest (its "version" drives the
#   - manifest-heltec-v4-tft.json   install dialog, so it must equal the tag)
#
# These live next to the rolling bins at firmware.wadamesh.com/latest/, so the
# flasher always reflects the current release without a site redeploy. The
# manifests themselves point at the IMMUTABLE per-tag bins (see below) to dodge
# the latest/*.bin 4h edge cache.
#
# Usage: gen-flasher-meta.py <tag> <outdir> [notes_file]
#   notes_file: one note per non-empty line (plain text); optional.
import sys, json, os

tag = sys.argv[1]
outdir = sys.argv[2]
notes_file = sys.argv[3] if len(sys.argv) > 3 else None

notes = []
if notes_file and os.path.exists(notes_file):
    notes = [ln.strip() for ln in open(notes_file) if ln.strip() and not ln.startswith("#")]

os.makedirs(outdir, exist_ok=True)

with open(os.path.join(outdir, "version.json"), "w") as f:
    json.dump({"tag": tag, "notes": notes}, f, indent=2)

BOARDS = {
    "manifest-tdeck.json":         ("wadamesh — LilyGo T-Deck", "wadamesh-tdeck-merged.bin"),
    "manifest-heltec-v4-tft.json": ("wadamesh — Heltec V4 TFT", "wadamesh-heltec-v4-tft-merged.bin"),
}
for fn, (name, binf) in BOARDS.items():
    manifest = {
        "name": name,
        "version": tag,
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": "ESP32-S3",
            # Point at the IMMUTABLE per-tag bin, NOT latest/. The latest/*.bin URLs
            # are stable filenames cached 4h (max-age=14400) and overwritten in place,
            # so for up to 4h after a release the flasher could hand out the PREVIOUS
            # beta's bytes while version.json already advertised the new tag. The
            # per-tag path is never overwritten, so its (24h) cache is always correct.
            # The manifest itself is max-age=300, so this new path propagates in <=5min.
            "parts": [{"path": f"https://firmware.wadamesh.com/releases/TOUCH/{tag}/{binf}", "offset": 0}],
        }],
    }
    with open(os.path.join(outdir, fn), "w") as f:
        json.dump(manifest, f, indent=2)

print(f"wrote version.json + 2 manifests for {tag} -> {outdir}  (notes: {len(notes)})")
