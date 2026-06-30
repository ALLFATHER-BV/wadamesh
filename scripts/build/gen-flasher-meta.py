#!/usr/bin/env python3
# Generate the web-flasher metadata that must roll with every release:
#   - version.json            : {tag, channel, notes[]} — the site shows this ("what's new")
#   - manifest-tdeck.json      : esp-web-tools manifest (its "version" drives the
#   - manifest-heltec-v4-tft.json   install dialog, so it must equal the tag)
#
# These live next to the rolling bins in a per-channel feed dir on
# firmware.wadamesh.com (latest/ for stable, latest-beta/ for the test channel),
# so the flasher always reflects the current release without a site redeploy. The
# manifests themselves point at the IMMUTABLE per-tag bins (releases/TOUCH for
# stable, releases/BETA for beta) to dodge the latest/*.bin 4h edge cache.
#
# Usage: gen-flasher-meta.py <tag> <outdir> [notes_file] [channel]
#   notes_file: one note per non-empty line (plain text); optional ("" to skip).
#   channel:    "stable" (default) or "beta" — picks the immutable archive dir
#               (releases/TOUCH vs releases/BETA) the manifests point at, and is
#               recorded in version.json so the site/flasher can label the channel.
import sys, json, os

tag = sys.argv[1]
outdir = sys.argv[2]
notes_file = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None
channel = sys.argv[4] if len(sys.argv) > 4 else "stable"
archive = "BETA" if channel == "beta" else "TOUCH"

notes = []
if notes_file and os.path.exists(notes_file):
    notes = [ln.strip() for ln in open(notes_file) if ln.strip() and not ln.startswith("#")]

os.makedirs(outdir, exist_ok=True)

with open(os.path.join(outdir, "version.json"), "w") as f:
    json.dump({"tag": tag, "channel": channel, "notes": notes}, f, indent=2)

BOARDS = {
    "manifest-tdeck.json":         ("wadamesh — LilyGo T-Deck", "wadamesh-tdeck-merged.bin"),
    "manifest-heltec-v4-tft.json": ("wadamesh — Heltec V4 TFT", "wadamesh-heltec-v4-tft-merged.bin"),
}
for fn, (name, binf) in BOARDS.items():
    manifest = {
        "name": name + ("" if channel == "stable" else " (beta)"),
        "version": tag,
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": "ESP32-S3",
            # Point at the IMMUTABLE per-tag bin, NOT the rolling feed bin. The feed
            # *.bin URLs are stable filenames cached 4h and overwritten in place, so
            # for up to 4h after a release the flasher could hand out the PREVIOUS
            # build's bytes while version.json already advertised the new tag. The
            # per-tag path is never overwritten, so its cache is always correct. The
            # manifest itself is max-age=300, so the new path propagates in <=5min.
            "parts": [{"path": f"https://firmware.wadamesh.com/releases/{archive}/{tag}/{binf}", "offset": 0}],
        }],
    }
    with open(os.path.join(outdir, fn), "w") as f:
        json.dump(manifest, f, indent=2)

print(f"wrote version.json + 2 manifests for {tag} ({channel}) -> {outdir}  (notes: {len(notes)})")
