#!/usr/bin/env bash
# wadamesh site deploy: publish the static landing page (deploy/site/) to the
# VPS web root behind Cloudflare.
#
# The apex `wadamesh.com` nginx block serves /srv/wadamesh/site
# (flasher.wadamesh.com 301-redirects there), so that dir IS the live site.
#
# Usage:
#   WADAMESH_VPS=user@your-vps scripts/deploy-site.sh             # deploy
#   WADAMESH_VPS=user@your-vps scripts/deploy-site.sh --dry-run   # preview only
#
# Optional:
#   WADAMESH_SITE_PATH=/srv/wadamesh/site   # override the web root
#
# The VPS target comes from the environment — never commit it (Cloudflare fronts
# the origin, so the IP isn't needed in the repo).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/deploy/site/"
DEST="${WADAMESH_VPS:?set WADAMESH_VPS=user@host}"
DEST_PATH="${WADAMESH_SITE_PATH:-/srv/wadamesh/site}"

DRY=""
[ "${1:-}" = "--dry-run" ] && DRY="--dry-run"

# Mirror deploy/site/ -> web root. --delete prunes files no longer in the repo;
# everything under deploy/site/ is static (html, svg, json manifests).
# Note: -a preserves source perms (644 files / 755 dirs) — no --chmod, since the
# rsync that ships with macOS rejects the D755,F644 per-type syntax.
rsync -avz $DRY --delete "$SRC" "$DEST:$DEST_PATH/"

if [ -z "$DRY" ]; then
  echo
  echo "deployed deploy/site/ -> $DEST:$DEST_PATH"
  echo "note: Cloudflare edge-caches; HTML is short-TTL but purge the zone if a"
  echo "      change doesn't show within a minute."
fi
