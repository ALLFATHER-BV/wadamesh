#!/usr/bin/env bash
# wadamesh app+language store deploy: publish deploy/apps/ to the firmware VPS.
#
# The device reads BOTH catalogs from the firmware host, NOT the site host:
#   http://firmware.wadamesh.com/apps/apps.json          <- Store > Apps
#   http://firmware.wadamesh.com/apps/langs.json         <- Store > Languages
#   http://firmware.wadamesh.com/apps/lang/<ver>/<code>.lang
# so this tree lives under the FIRMWARE web root, alongside releases/ and latest*/.
#
# WHY THIS SCRIPT EXISTS: there wasn't one. release.sh rsyncs out/firmware/ (which
# does not contain apps/) and deploy-site.sh rsyncs deploy/site/ — so the store was
# only ever updated by someone remembering to rsync it by hand, and twice they did
# not. Translations sat at v8 on the server while the repo had v9/v10/v11, and a
# published app never appeared at all. Editing deploy/apps/ is not publishing it;
# running this is.
#
# Usage:
#   WADAMESH_VPS=user@your-vps scripts/deploy-apps.sh             # deploy
#   WADAMESH_VPS=user@your-vps scripts/deploy-apps.sh --dry-run   # preview only
#
# Optional:
#   WADAMESH_FW_PATH=/srv/wadamesh/firmware   # override the firmware web root
#
# The VPS target comes from the environment — never commit it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/deploy/apps/"
DEST="${WADAMESH_VPS:?set WADAMESH_VPS=user@host}"
DEST_PATH="${WADAMESH_FW_PATH:-/srv/wadamesh/firmware}/apps"

DRY=""
[ "${1:-}" = "--dry-run" ] && DRY="--dry-run"

# Sanity-check the catalogs before shipping them: a device that gets malformed JSON
# shows an empty store with no clue why, and the versioned .lang dir each catalog
# entry points at has to actually exist or the download 404s on the device.
python3 - "$ROOT" <<'PY'
import json, os, sys
root = sys.argv[1]
apps_dir = os.path.join(root, "deploy", "apps")
langs = json.load(open(os.path.join(apps_dir, "langs.json")))["langs"]
missing = [f"{l['code']} v{l['ver']}" for l in langs
           if not os.path.isfile(os.path.join(apps_dir, "lang", str(l["ver"]), l["code"] + ".lang"))]
if missing:
    sys.exit("ABORT: langs.json points at files that do not exist: " + ", ".join(missing))

# The catalog version is what makes a device re-download a language. If it does
# not match the "# ver:" line inside the file, translators' work is published to
# a version nobody fetches and the change is invisible on device.
import re
drift = []
for l in langs:
    f = os.path.join(apps_dir, "lang", str(l["ver"]), l["code"] + ".lang")
    m = re.search(r"^# ver:\s*(\S+)", open(f, encoding="utf-8").read(), re.M)
    if m and m.group(1) != str(l["ver"]):
        drift.append(f"{l['code']}: catalog v{l['ver']} but file says v{m.group(1)}")
if drift:
    sys.exit("ABORT: langs.json and the .lang headers disagree: " + "; ".join(drift))

# Same check for apps: an entry whose <id>/<ver>/<id>.lua is absent shows up in the
# Store and then fails to install, with nothing on screen to say why.
apps = json.load(open(os.path.join(apps_dir, "apps.json")))["apps"]
bad = []
for a in apps:
    base = os.path.join(apps_dir, a["id"], a["ver"])
    for f in (a["id"] + ".lua", a["id"] + ".json"):
        if not os.path.isfile(os.path.join(base, f)):
            bad.append(f'{a["id"]} v{a["ver"]}: {f}')
if bad:
    sys.exit("ABORT: apps.json points at files that do not exist: " + ", ".join(bad))
print(f"catalogs ok: {len(langs)} languages and {len(apps)} apps, every referenced file present")
PY

# --delete mirrors the repo: the store is generated content, the repo is the source
# of truth, and stale versioned dirs left on the server are how a device ends up
# offered a language file nothing points at any more.
rsync -avz $DRY --delete "$SRC" "$DEST:$DEST_PATH/"

if [ -z "$DRY" ]; then
  echo
  echo "deployed deploy/apps/ -> $DEST:$DEST_PATH"
  echo "verify:"
  echo "  curl -s http://firmware.wadamesh.com/apps/langs.json"
  echo "  curl -s http://firmware.wadamesh.com/apps/apps.json"
fi
