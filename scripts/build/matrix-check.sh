#!/usr/bin/env bash
# Print what beta testers actually reported for a tag, from the report service
# (deploy/report-service.py). Read-only and side-effect free: release.sh calls it
# before a --promote, and you can run it on its own any time.
#
#   scripts/build/matrix-check.sh beta_84
#
# It never fails a promote. A board nobody owns can never go green, and some
# promotes are made on other evidence; this exists so the promote is a decision
# rather than a habit.
TAG="${1:?usage: matrix-check.sh <tag>}"
command -v curl >/dev/null 2>&1 || exit 0
command -v python3 >/dev/null 2>&1 || exit 0

curl -fsS --max-time 8 "http://firmware.wadamesh.com/report/matrix.json?tag=$TAG" 2>/dev/null \
  | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
s = d.get("summary", {})
print("")
print("test matrix for %s: %d green, %d part-way, %d with a problem, %d silent (%d reports, %d devices)"
      % (d.get("tag", "?"), s.get("green", 0), s.get("amber", 0), s.get("red", 0),
         s.get("none", 0), s.get("reports", 0), s.get("devices", 0)))
for b in d.get("boards", []):
    if b.get("status") in ("red", "amber") or (b.get("status") == "none" and b.get("devices")):
        print("  %-22s %s (%d works, %d problems, %s running)"
              % (b.get("board", "?"), b.get("status"), b.get("works", 0), b.get("issues", 0),
                 b.get("devices") or "?"))
if s.get("promote_ready"):
    print("every board anyone is known to be running has been vouched for. wadamesh.com/beta")
elif not s.get("reports"):
    print("NOBODY has reported on this build. wadamesh.com/beta")
else:
    print("NOT vouched for: %s" % (", ".join(s.get("blocked_by", [])) or "no board is green yet"))
    print("promoting anyway is a choice, not an error. wadamesh.com/beta")
print("")
'
