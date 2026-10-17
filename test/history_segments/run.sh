#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host regression test for the chat-history segment scheduler (the beta_95
# "messages vanish after a reboot" stall at 24 segments). extract.py lifts
# segBuildJob / segCommitJob and friends verbatim out of src/ui-touch/UITask.cpp;
# harness.cpp runs them against a simulated card with a 5000-record ring,
# per-chat caps, random reboots and power cuts in the middle of a merge, and
# checks that every record that was live (and on the card, for a power cut)
# survives the reboot with no duplicates and no stuck save.
#
# Usage: test/history_segments/run.sh [seeds per mode, default 10]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
seeds="${1:-10}"
work="$(mktemp -d)"
python3 "$here/extract.py" "$root/src/ui-touch/UITask.cpp" > "$work/seg.inc"
c++ -std=c++17 -O1 -Wall -Wno-unused-function -I"$work" -DSEG_INC='"seg.inc"' \
    -o "$work/harness" "$here/harness.cpp"
fail=0
for mode in 0 1 2 3 4; do
  for s in $(seq 1 "$seeds"); do
    "$work/harness" "$s" "$mode" > "$work/out.txt" || { echo "FAIL mode $mode: $(cat "$work/out.txt")"; fail=1; }
  done
  echo "mode $mode: $(tail -1 "$work/out.txt")"
done
[ "$fail" = 0 ] && echo "history segments: all modes passed"
exit "$fail"
