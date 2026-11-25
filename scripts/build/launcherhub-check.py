#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare what LauncherHub recorded for a release with what the release now holds.

bmorcelli's Launcher installs wadamesh from LauncherHub, whose catalog
(bmorcelli/M5Stack-json-fw, 3rd/r/<device>.json) records for every release the merged
image's size ("Fs") and the app image's size and offset ("install.app"), read ONCE when
the tag first appears and never again. The Launcher then copies exactly that many bytes
into a partition of exactly that size. If a merged .bin is replaced after LauncherHub has
read it, the install copies the wrong number of bytes: a shorter old record cuts the
image ("APP: Flash Read Failed" when the image is verified), a longer one waits for bytes
that never come. That is what broke beta_106 on the T-Deck: wadamesh-tdeck-merged.bin
was re-uploaded 7 minutes after LauncherHub had read the first upload.

Usage: scripts/build/launcherhub-check.py <tag>      (e.g. beta_106)
Exit 0 when every LauncherHub entry for <tag> matches the GitHub asset, 1 on a mismatch,
2 when <tag> is not in the catalog yet. Needs `gh` (logged in) for both lookups.
"""
import json
import subprocess
import sys

CATALOG = "bmorcelli/M5Stack-json-fw"
REPO = "ALLFATHER-BV/wadamesh"


def gh_json(path):
    out = subprocess.run(["gh", "api", path], check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def gh_raw(repo, path):
    out = subprocess.run(
        ["gh", "api", "-H", "Accept: application/vnd.github.raw", f"repos/{repo}/contents/{path}"],
        check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-4])
        return 2
    tag = sys.argv[1]
    assets = {a["name"]: a for a in gh_json(f"repos/{REPO}/releases/tags/{tag}")["assets"]}

    files = [e["name"] for e in gh_json(f"repos/{CATALOG}/contents/3rd/r")
             if e["name"].endswith(".json") and e["name"] != "all_devices_firmware.json"]
    seen = bad = 0
    for f in sorted(files):
        try:
            doc = gh_raw(CATALOG, f"3rd/r/{f}")
        except (subprocess.CalledProcessError, json.JSONDecodeError):
            continue
        entries = doc if isinstance(doc, list) else doc.get("firmwares", [])
        for e in entries:
            if not isinstance(e, dict):
                continue
            for v in e.get("versions", []):
                url = v.get("file", "")
                if f"/{REPO}/releases/download/{tag}/" not in url:
                    continue
                name = url.rsplit("/", 1)[-1]
                seen += 1
                app = (v.get("install") or {}).get("app") or {}
                rec_fs, rec_app = v.get("Fs"), app.get("image_size", v.get("as"))
                off = app.get("source_offset", v.get("ao", 0)) or 0
                a = assets.get(name)
                if not a:
                    print(f"MISSING  {f}: {e.get('name')}: {name} is not in the {tag} release")
                    bad += 1
                    continue
                now_app = a["size"] - off
                ok = rec_fs == a["size"] and rec_app == now_app
                print(f"{'ok      ' if ok else 'MISMATCH'} {f}: {e.get('name')}: {name} "
                      f"hub Fs={rec_fs} app={rec_app}, GitHub {a['size']} (app {now_app}), "
                      f"uploaded {a['updated_at']}")
                bad += 0 if ok else 1
    if not seen:
        print(f"{tag} is not in the LauncherHub catalog yet (it syncs about every 15 minutes)")
        return 2
    if bad:
        print("\nLauncher installs of the mismatched entries fail. LauncherHub never re-reads a tag:\n"
              "ask bmorcelli to refresh it (or publish a new tag). Never replace a merged .bin on a\n"
              "published release.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
