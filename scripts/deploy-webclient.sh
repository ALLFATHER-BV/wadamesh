#!/usr/bin/env bash
# Publish the WADAMESH web client to app.wadamesh.com.
#   web/dist/           (pthreads build, https)   -> <root>/
#   web/dist-nothreads/ (no-threads build, http)  -> <root>/nt/   (when built)
# Nginx vhost: deploy/nginx/app.wadamesh.com.conf (installed separately, once).
#
# Usage:
#   WADAMESH_VPS=user@your-vps scripts/deploy-webclient.sh             # build + deploy
#   WADAMESH_VPS=user@your-vps scripts/deploy-webclient.sh --dry-run   # preview only
#   WADAMESH_WEB_NOBUILD=1 ...                                         # deploy what is built
#
# The VPS target comes from the environment; never commit it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${WADAMESH_VPS:?set WADAMESH_VPS=user@host}"
DEST_PATH="${WADAMESH_APP_PATH:-/srv/wadamesh/app}"
DRY=""
[ "${1:-}" = "--dry-run" ] && DRY="--dry-run"

if [ -z "${WADAMESH_WEB_NOBUILD:-}" ]; then
  "$ROOT/web/build.sh" web
  [ -x "$ROOT/web/build.sh" ] && grep -q "web-nothreads" "$ROOT/web/build.sh" && "$ROOT/web/build.sh" web-nothreads || true
fi
[ -f "$ROOT/web/dist/index.html" ] || { echo "no web/dist: build first (web/build.sh web)" >&2; exit 1; }

# The threaded build owns the root; the no-threads build lives in nt/ next to it, so
# --delete on the root must leave nt/ alone (it is synced on its own below).
rsync -avz $DRY --delete --exclude 'nt/' "$ROOT/web/dist/" "$DEST:$DEST_PATH/"
if [ -d "$ROOT/web/dist-nothreads" ]; then
  rsync -avz $DRY --delete "$ROOT/web/dist-nothreads/" "$DEST:$DEST_PATH/nt/"
fi

if [ -z "$DRY" ]; then
  echo
  echo "deployed web/dist -> $DEST:$DEST_PATH"
  echo "note: index.html is no-cache; bundles are versioned (?v=<hash>), so no purge is needed."
fi
