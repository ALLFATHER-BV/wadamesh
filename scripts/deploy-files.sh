#!/usr/bin/env bash
# Publish the USB Files page (deploy/files/) to files.wadamesh.com's web root.
# Nginx vhost: deploy/nginx/files.wadamesh.com.conf.
#
# Usage:
#   WADAMESH_VPS=user@your-vps scripts/deploy-files.sh             # deploy
#   WADAMESH_VPS=user@your-vps scripts/deploy-files.sh --dry-run   # preview only
#
# Optional:
#   WADAMESH_FILES_PATH=/srv/wadamesh/files   # override the web root
#
# The VPS target comes from the environment; never commit it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/deploy/files/"
DEST="${WADAMESH_VPS:?set WADAMESH_VPS=user@host}"
DEST_PATH="${WADAMESH_FILES_PATH:-/srv/wadamesh/files}"

DRY=""
[ "${1:-}" = "--dry-run" ] && DRY="--dry-run"

rsync -avz $DRY --delete "$SRC" "$DEST:$DEST_PATH/"

if [ -z "$DRY" ]; then
  echo
  echo "deployed deploy/files/ -> $DEST:$DEST_PATH"
  echo "note: Cloudflare edge-caches; purge files.wadamesh.com if a change does not show."
fi
