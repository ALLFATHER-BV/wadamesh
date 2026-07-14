#!/usr/bin/env bash
#
# Publish the Tanmatsu build of WadaMesh to the Nicolai-Electronics app-repository
# (the official Tanmatsu launcher app-store). Run this as part of cutting a beta.
#
#   scripts/tanmatsu-appstore-publish.sh beta_17               # -> version 1.0.17, revision 17
#   scripts/tanmatsu-appstore-publish.sh 1.2.0 8              # -> explicit version 1.2.0, revision 8
#   scripts/tanmatsu-appstore-publish.sh beta_17 --skip-build # use the existing build
#
# What it does:
#   1. builds the Tanmatsu app (unless --skip-build) and verifies it
#   2. syncs our fork (B0N3-GD/app-repository) to upstream main
#   3. (re)stages deploy/tanmatsu-app/com.wadamesh.wadamesh/ + the fresh binary,
#      setting `version` and every application `revision`
#   4. pushes an `update-wadamesh-<version>` branch and opens/updates a PR to
#      Nicolai-Electronics/app-repository
#
# NB:
#  - WadaMesh on the Tanmatsu is KEYBOARD-driven (no touchscreen). Keep the
#    listing wording free of "touch" — the canonical text lives in the template.
#  - For an update you MUST bump BOTH `version` AND `revision` or the launcher
#    won't offer it. This script bumps both.
#  - The CLA is signed once per GitHub user (Kaj), so follow-up PRs auto-pass.
#  - First run can't sync until PR #55 (the initial add) is merged; until then
#    the fork's main lacks the folder, but step 3 re-stages it in full anyway.
set -euo pipefail

# ---- args ---------------------------------------------------------------
ARG="${1:?usage: tanmatsu-appstore-publish.sh <beta_N | version> [revision] [--skip-build]}"
shift || true
SKIP_BUILD=0; REV_ARG=""
for a in "$@"; do
  case "$a" in
    --skip-build) SKIP_BUILD=1 ;;
    *)            REV_ARG="$a" ;;
  esac
done
if [[ "$ARG" =~ ^beta_([0-9]+)$ ]]; then
  N="${BASH_REMATCH[1]}"; VERSION="1.0.$N"; REVISION="${REV_ARG:-$N}"
else
  VERSION="$ARG"; REVISION="${REV_ARG:?explicit version needs a revision: tanmatsu-appstore-publish.sh <version> <revision>}"
fi

# ---- paths --------------------------------------------------------------
SLUG=com.wadamesh.wadamesh
WADAMESH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"          # repo root
TPL="$WADAMESH/deploy/tanmatsu-app/$SLUG"                            # versioned template (icons/README/metadata)
BIN="$WADAMESH/tanmatsu/build/tanmatsu/application.bin"              # IDF build output
FORK_DIR="${WADAMESH_APPSTORE_FORK:-/Users/kaj/wadamesh-appstore-fork}"
UPSTREAM=Nicolai-Electronics/app-repository
GHUSER="$(gh api user -q .login)"

echo "==> WadaMesh app-store publish: version $VERSION, revision $REVISION"

# ---- 1. build + verify the Tanmatsu app --------------------------------
if [ "$SKIP_BUILD" -eq 0 ]; then
  # Bake the EXACT release tag into the P4 binary (About page + update check). We know
  # it here; git describe inside build.sh would read a "-N-g<sha>" suffix (or the prior
  # beta) whenever the beta_N tag isn't fetched locally.
  [[ "$ARG" =~ ^beta_([0-9]+)$ ]] && export WADA_FW_TAG="$ARG"
  echo "==> building Tanmatsu app (./build.sh build)..."
  LOG="$(mktemp)"
  ( cd "$WADAMESH/tanmatsu" && ./build.sh build ) >"$LOG" 2>&1 || true   # app_check_size exit!=0 is EXPECTED
  # real compile/link errors abort; the cosmetic 'app partitions too small' does not
  if grep -E "error:|undefined reference|was not declared" "$LOG" | grep -vE "app_check_size|too small|partitions are too small"; then
    echo "!! real build error above — aborting"; rm -f "$LOG"; exit 1
  fi
  rm -f "$LOG"
fi
[ -f "$BIN" ] || { echo "!! no Tanmatsu binary at $BIN — build first (drop --skip-build)"; exit 1; }
echo "==> binary present: $(wc -c <"$BIN") bytes"

# ---- 2. sync the fork to upstream main ---------------------------------
[ -d "$FORK_DIR/.git" ] || { echo "!! fork clone missing at $FORK_DIR — clone $GHUSER/app-repository there first"; exit 1; }
git -C "$FORK_DIR" remote get-url upstream >/dev/null 2>&1 || \
  git -C "$FORK_DIR" remote add upstream "https://github.com/$UPSTREAM.git"
git -C "$FORK_DIR" fetch -q upstream
git -C "$FORK_DIR" checkout -q main 2>/dev/null || git -C "$FORK_DIR" checkout -qb main upstream/main
git -C "$FORK_DIR" reset -q --hard upstream/main

# ---- 3. stage the app folder (template + fresh binary; set version/rev) -
APP="$FORK_DIR/$SLUG"
mkdir -p "$APP"
cp "$TPL/icon16.png" "$TPL/icon32.png" "$TPL/icon64.png" "$TPL/README.md" "$APP/"
cp "$WADAMESH/LICENSE" "$APP/LICENSE"
cp "$WADAMESH/NOTICE"  "$APP/NOTICE"      # MeshCore (MIT) + other third-party attribution
cp "$BIN" "$APP/wadamesh.bin"
python3 - "$TPL/metadata.json" "$APP/metadata.json" "$VERSION" "$REVISION" <<'PY'
import json, sys, re
src, dst, ver, rev = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
m = json.load(open(src))
# The launcher store's verify_metadata check restricts name/description to this set
# (NO commas, NO em-dash). Fail loudly HERE instead of as a red check on the PR.
pat = r'^[A-Za-z0-9_\- .!@#$%^&*()\/\\]+$'
for f in ("name", "description"):
    v = m.get(f, "")
    if not re.match(pat, v) or len(v) > 256:
        sys.stderr.write(f"!! metadata '{f}' fails the store regex (no comma / em-dash, <=256 chars): {v!r}\n")
        sys.exit(1)
m["version"] = ver
for app in m["application"]:
    app["revision"] = rev
json.dump(m, open(dst, "w"), indent=4); open(dst, "a").write("\n")
PY
echo "==> staged $SLUG (version $VERSION, revision $REVISION)"

# ---- 4. commit on a branch, push, open/update the PR -------------------
BR="update-wadamesh-$VERSION"
git -C "$FORK_DIR" checkout -qB "$BR"
git -C "$FORK_DIR" add "$SLUG"
GIT_COMMITTER_NAME="Kaj Schittecat" GIT_COMMITTER_EMAIL="kaj@schittecat.com" \
  git -C "$FORK_DIR" commit -q --author="Kaj Schittecat <kaj@schittecat.com>" \
    -m "Update WadaMesh to $VERSION (revision $REVISION)" || echo "   (nothing new to commit)"
git -C "$FORK_DIR" push -fq -u origin "$BR"
if gh pr create --repo "$UPSTREAM" --base main --head "$GHUSER:$BR" \
     --title "Update WadaMesh to $VERSION" \
     --body "Updates WadaMesh ($SLUG) to v$VERSION (revision $REVISION) — new Tanmatsu build." 2>/dev/null; then
  echo "==> opened PR for $VERSION"
else
  echo "==> PR already open for $BR (force-push updated it)"
fi
echo "==> done. Track it: gh pr list --repo $UPSTREAM --author $GHUSER"
