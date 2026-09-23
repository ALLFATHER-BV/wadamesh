#!/usr/bin/env bash
# wadamesh report service deploy: install/refresh the beta test-report service
# (deploy/report-service.py) on the firmware VPS, behind the existing
# firmware.wadamesh.com vhost at /report/.
#
# What it does, all of it idempotent:
#   * rsyncs the service into /opt/wadamesh-reports
#   * checks flask + requests are importable by the system python
#   * installs the systemd unit, enables and restarts it
#   * refreshes the firmware vhost (which now proxies /report/) and reloads nginx
#
# The SQLite database at /opt/wadamesh-reports/reports.db is never touched, so a
# redeploy keeps every report.
#
# Usage:
#   WADAMESH_VPS=user@your-vps scripts/deploy-reports.sh
#   WADAMESH_VPS=user@your-vps scripts/deploy-reports.sh --dry-run
#
# The GitHub token is NOT deployed by this script and is not in the repo. It
# lives on the box in /etc/wadamesh-reports.env:
#
#   WADA_GH_TOKEN=<token>             # fine-grained, this repo only, issues: read+write
#   WADA_GH_REPO=ALLFATHER-BV/wadamesh
#
# Without it the service runs normally and simply never publishes to GitHub, so
# reports are still collected and the site still shows them.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${WADAMESH_VPS:?set WADAMESH_VPS=user@host}"
APP_DIR="${WADAMESH_REPORTS_PATH:-/opt/wadamesh-reports}"
# Same escape hatch rsync has: the VPS key is not the default identity here.
#   SSH="ssh -i ~/.ssh/your_key" RSYNC_RSH="$SSH" scripts/deploy-reports.sh
SSH="${SSH:-ssh}"

if [ "${1:-}" = "--dry-run" ]; then
  echo "would deploy:"
  echo "  $ROOT/deploy/report-service.py         -> $DEST:$APP_DIR/"
  echo "  $ROOT/deploy/wadamesh-reports.service  -> $DEST:/etc/systemd/system/"
  echo "  $ROOT/deploy/nginx/firmware.wadamesh.com.conf -> $DEST:/etc/nginx/sites-available/firmware.wadamesh.com.conf"
  exit 0
fi

python3 - "$ROOT/deploy/report-service.py" <<'PY'
import ast, sys
ast.parse(open(sys.argv[1]).read())        # never ship a file that will not import
print("report-service.py parses")
PY

# The vhost on the box has drifted ahead of the repo before (/apps/ and
# /bringup/ were added live and never committed). Pushing a stale copy would
# 404 the app store, so compare first and stop rather than overwrite.
LIVE_CONF=$($SSH "$DEST" "cat /etc/nginx/sites-available/firmware.wadamesh.com.conf" 2>/dev/null || true)
for needed in $(printf '%s\n' "$LIVE_CONF" | grep -oE 'location [=~ ]*/[a-z.-]+/?' | sort -u); do
  if ! grep -qF "$needed" "$ROOT/deploy/nginx/firmware.wadamesh.com.conf"; then
    echo "ABORT: the live vhost has '$needed' and the repo copy does not." >&2
    echo "Pull the live file into deploy/nginx/ first, then re-add your changes." >&2
    exit 1
  fi
done

$SSH "$DEST" "mkdir -p $APP_DIR"
rsync -avz "$ROOT/deploy/report-service.py" "$DEST:$APP_DIR/"
rsync -avz "$ROOT/deploy/wadamesh-reports.service" "$DEST:/etc/systemd/system/wadamesh-reports.service"
# The live name carries the .conf suffix and is what sites-enabled points at.
# Getting this wrong writes a file nginx never reads, and the /report/ proxy
# silently does not exist.
rsync -avz "$ROOT/deploy/nginx/firmware.wadamesh.com.conf" \
           "$DEST:/etc/nginx/sites-available/firmware.wadamesh.com.conf"

$SSH "$DEST" bash -s <<EOF
set -e
# No venv: this box's python3.14 has no ensurepip, so venv cannot bootstrap pip.
# flask and requests come from apt and are already in use by the tile service.
if ! /usr/bin/python3 -c 'import flask, requests' 2>/dev/null; then
  echo "installing flask and requests from apt"
  apt-get update -qq && apt-get install -y -qq python3-flask python3-requests
fi
systemctl daemon-reload
systemctl enable --now wadamesh-reports
systemctl restart wadamesh-reports
sleep 1
nginx -t && systemctl reload nginx
systemctl is-active wadamesh-reports
EOF

echo
echo "deployed. checking the live endpoint:"
curl -fsS "http://firmware.wadamesh.com/report/health" && echo
echo
echo 'publishes:false means no GitHub token on the box yet (see the header of this script).'
