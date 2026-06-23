#!/usr/bin/env python3
"""Generate the Mesh America Configurator catalog JSON for wadamesh.

Mesh America (apps.meshamerica.com — the flasher Cascadia Mesh uses) merges a
provider "catalog" JSON into its device list. Spec: third-party hosts ONE static
JSON (this file's output) at a stable HTTPS+CORS URL, plus the firmware bins over
HTTPS+CORS; their app reads it live. See provider-api-spec.md from TJ Downes.

We host:
  - the bins on firmware.wadamesh.com/releases/TOUCH/<TAG>/  (Cloudflare HTTPS +
    `Access-Control-Allow-Origin: *`, immutable per-tag dir — see deploy/nginx/
    firmware.wadamesh.com.conf), and
  - this catalog at a stable HTTPS+CORS URL (deployed alongside the firmware).

Usage:  python3 deploy/gen-meshamerica-catalog.py <TAG>  > meshamerica-catalog.json
        (TAG defaults to the newest beta_N dir; notes come from release-notes/<TAG>.txt)

Re-run it each release (or wire it into scripts/release.sh) so the catalog always
points at the newest immutable bins — the spec wants firmware URLs treated as
immutable, so each release adds a fresh versioned URL.
"""
import sys, json, os, glob, re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def newest_tag():
    tags = []
    for p in glob.glob(os.path.join(REPO, "release-notes", "beta_*.txt")):
        m = re.search(r"beta_(\d+)", os.path.basename(p))
        if m:
            tags.append(int(m.group(1)))
    return "beta_%d" % max(tags) if tags else "beta_9"

TAG  = sys.argv[1] if len(sys.argv) > 1 else newest_tag()
BASE = "https://firmware.wadamesh.com/releases/TOUCH/%s" % TAG
ICON = "https://wadamesh.com/wadamesh-icon-512.png"

# "What's new" — one user-facing line per non-blank, non-# line of the notes file.
notes = ""
nf = os.path.join(REPO, "release-notes", "%s.txt" % TAG)
if os.path.exists(nf):
    notes = "\n".join(l.strip() for l in open(nf, encoding="utf-8")
                      if l.strip() and not l.strip().startswith("#"))

# Both touch boards are ESP32-S3, single touch-UI ("gui") role. ota_0 sits at
# 0x10000 on both (partitions_t*_touch.csv), so flash-update@0x10000 is correct.
BOARDS = [
    {
        "name": "wadamesh — Heltec V4 / Expansion Kit",
        "slug": "heltec-v4-tft",
        "description": "Firmware for the Heltec V4 touch build, with support for the Heltec V4 Expansion Kit sensors, buzzer, and local status UI.",
    },
    {
        "name": "wadamesh — LilyGo T-Deck",
        "slug": "tdeck",
        "description": "Firmware for the LilyGo T-Deck touch build with keyboard and trackball support.",
    },
]

def device(b):
    merged, appbin = "wadamesh-%s-merged.bin" % b["slug"], "wadamesh-%s.bin" % b["slug"]
    return {
        "maker": "wadamesh",
        "class": "wadamesh",
        "name": b["name"],
        "type": "esp32",
        "tooltip": "<img class='device' src='%s'><p>%s</p>" % (ICON, b["description"]),
        "firmware": [{
            "role": "gui",
            "title": "Touch UI firmware",
            "version": {TAG: {
                "notes": notes,
                "files": [
                    {"type": "flash-wipe",   "name": merged,
                     "url": "%s/%s" % (BASE, merged),
                     "title": "Full install (bootloader + firmware)"},
                    {"type": "flash-update", "name": appbin,
                     "url": "%s/%s" % (BASE, appbin),
                     "title": "Update (app only)"},
                ],
            }},
        }],
    }

catalog = {
    "maker":  {"wadamesh": {"name": "wadamesh"}},
    "device": [device(b) for b in BOARDS],
}
print(json.dumps(catalog, indent=2, ensure_ascii=False))
