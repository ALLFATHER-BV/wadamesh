#!/usr/bin/env bash
# One-time: fetch badge.team's appfs.py, which tan_deploy.py needs to read and rewrite
# the AppFS partition image.
#
# It is NOT vendored into this repo on purpose: it is badge.team's code and the copy in
# circulation carries no licence header, so cloning it keeps the authorship where it
# belongs rather than silently absorbing it into a GPL tree.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/appfs-src"
if [ -d "$DEST" ]; then
  echo "already present: $DEST"
else
  git clone --depth 1 https://github.com/badgeteam/esp32-component-appfs.git "$DEST"
fi
python3 -c "import sys; sys.path.insert(0,'$DEST/tools'); import appfs; print('appfs.py OK')"
