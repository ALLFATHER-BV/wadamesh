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

# Every wadamesh tile here is ESP32-S3, touch-UI ("gui") role. ota_0 sits at
# 0x10000 on every touch env (partitions_t*_touch.csv), so flash-update@0x10000 is correct.
# `name` MUST match the official MeshCore device name character-for-character, or Mesh America lists it
# as a NEW device instead of folding our firmware into the existing tile (TJ Downes, 2026-06). Canonical
# names verified from apps.meshamerica.com/proxy/flasher/config.json. The V4 entry is the Expansion Kit
# (Touch) variant only, per Kaj. `slug` still maps to our bin filenames on firmware.wadamesh.com.
#
# A board may carry `variants` instead of a single `slug`: one "gui" firmware entry is emitted per
# variant, all under the one tile. The T-Lora Pager ships with either radio, and Mesh America folds both
# under a single "LilyGo T-Lora Pager" tile distinguished by a [RADIO] title prefix — matching how their
# own stock pager entries are labelled ([SX1262]/[LR1121]), so ours sit naturally alongside them.
BOARDS = [
    {"name": "Heltec v4 + Expansion Kit (Touch)", "slug": "heltec-v4-tft"},
    {"name": "LilyGo T-Deck", "slug": "tdeck"},
    {"name": "LilyGo T-Lora Pager", "variants": [
        {"title": "[LR1121] Touch UI", "slug": "tlora-pager-lr1121"},
        {"title": "[SX1262] Touch UI", "slug": "tlora-pager-sx1262"},
    ]},
]

def _firmware(title, slug):
    merged, appbin = "wadamesh-%s-merged.bin" % slug, "wadamesh-%s.bin" % slug
    return {
        "role": "gui",
        "title": title,
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
    }

def device(b):
    # `variants` (per-radio builds) fold under one tile; a plain board is the single default variant.
    variants = b.get("variants") or [{"title": "Touch UI", "slug": b["slug"]}]
    return {
        "maker": "wadamesh",
        "class": "wadamesh",
        "name": b["name"],
        "type": "esp32",
        "tooltip": "<img class='device' src='%s'>" % ICON,
        "firmware": [_firmware(v["title"], v["slug"]) for v in variants],
    }

# Optional "what's this?" blurb Mesh America shows on a hover tooltip next to our provider badge
# (spec section 3, added by TJ 2026-06-30). Plain text only — rendered escaped, so no markup/links.
DESCRIPTION = (
    "WADAMESH is a full touch-screen interface for MeshCore: on-device chat, contacts, a live map "
    "with offline tiles, and complete radio settings. Same MeshCore protocol and phone-app companion "
    "as stock MeshCore, just a richer on-device front-end."
)

catalog = {
    "description": DESCRIPTION,
    "maker":  {"wadamesh": {"name": "wadamesh"}},
    "device": [device(b) for b in BOARDS],
}
print(json.dumps(catalog, indent=2, ensure_ascii=False))
