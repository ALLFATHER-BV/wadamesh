#!/usr/bin/env bash
# #744 off-device: a settings slot file that will not take a write is recreated.
#
# Runs the web host build three times on one data dir with WADA_PREFS_FILE=1 (the
# devices' file-backed settings store, SdNvsPrefs, as on a board without SD):
#   1. first boot, setup not finished: <data>/spiffs/prefs/touch.kv (slot a) is written;
#   2. slot b (touch.alt) is made a damaged, read-only file, then setup is finished:
#      the save targets slot b, cannot open it, and (#744) removes and rewrites it;
#   3. a reboot: setup must stay finished (Home, not the welcome screen).
# Shots in <out>/shots-N/: 1-first-boot, 2-setup-done, 3-after-reboot. Exit 0 when the
# "recreating it" line was logged and the reboot did not show the welcome again.
#
#   web/scripts/prefs-slot.sh [out-dir]      (build first: web/build.sh host)
set -euo pipefail
WEB="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$WEB/build-host/wadamesh-host"
OUT="${1:-$(mktemp -d)}"
mkdir -p "$OUT"
DATA="$OUT/data"; rm -rf "$DATA"
run() {   # run <script-text> <log>; each run its own shots dir (the host clears it)
  printf '%b' "$1" > "$OUT/s.wada"
  mkdir -p "$OUT/shots-$3"
  WADA_PREFS_FILE=1 WADA_DATA_DIR="$DATA" "$BIN" --headless --script "$OUT/s.wada" --shots "$OUT/shots-$3" > "$2" 2>&1
}
run 'wait 5000\nshot 1-first-boot\nquit\n' "$OUT/run1.log" 1
[ -f "$DATA/spiffs/prefs/touch.kv" ] || { echo "no touch.kv after the first boot"; exit 1; }
printf 'damaged entry' > "$DATA/spiffs/prefs/touch.alt"
chmod 444 "$DATA/spiffs/prefs/touch.alt"
run 'wait 5000\ntap 60 283\nwait 5000\nshot 2-setup-done\nquit\n' "$OUT/run2.log" 2
run 'wait 5000\nshot 3-after-reboot\nquit\n' "$OUT/run3.log" 3
grep -h "PREFS" "$OUT"/run*.log || true
ls -l "$DATA/spiffs/prefs"
if grep -q "recreating it" "$OUT/run2.log" && [ -w "$DATA/spiffs/prefs/touch.alt" ]; then
  echo "prefs-slot: slot b recreated (look at $OUT/shots-3/3-after-reboot.png: Home, not the welcome)"
else
  echo "prefs-slot: slot b was NOT recreated"; exit 1
fi
