#!/usr/bin/env bash
# #731 off-device: Settings > Contact Management > Auto-delete old contacts.
#
# Three runs of the web host build on one data dir (demo contacts: Femke last heard
# 3 days ago, Old Node 9 days, the rest within a day):
#   1. clock NOT set (WADA_CLOCK_UNSET=1): Old Node made a favourite, the switch turned
#      on, Days = 2. The pass it asks for must not run on an unset clock: all 8 stay.
#   2. clock set, Femke's chat opened at once (through Contacts > Search, the list
#      order follows activity) and kept open past the first pass
#      (3 min after boot): Femke (open chat) and Old Node (favourite) both stay.
#   3. clock set, the Contacts tab open past the first pass: Femke goes, Old Node stays.
# Shots in <out>/shots-N/. Look at them: 1-contacts (8), 2-contacts (8), 3-contacts
# (7, no Femke, Old Node still there).
#
#   web/scripts/contact-purge.sh [out-dir]      (build first: web/build.sh host)
set -euo pipefail
WEB="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$WEB/build-host/wadamesh-host"
OUT="${1:-$(mktemp -d)}"
mkdir -p "$OUT"
DATA="$OUT/data"; rm -rf "$DATA"
run() {   # run <n> <script-text> [env...]
  local n="$1" text="$2"; shift 2
  printf '%b' "$text" > "$OUT/s$n.wada"
  mkdir -p "$OUT/shots-$n"
  env "$@" WADA_DATA_DIR="$DATA" "$BIN" --headless --script "$OUT/s$n.wada" --shots "$OUT/shots-$n" > "$OUT/run$n.log" 2>&1
}
# (Days is typed on the on-screen keys: backspace twice, 1#, 2, OK.)
# Wait ~200 s, past the first pass 3 min after boot (run 1 set Screen timeout to Never).
awake="wait 200000\n"

# 1: onboarding, Screen timeout Never, favourite Old Node, Days = 2, switch on; clock unset.
run 1 "wait 5000\ntap 60 283\nwait 3000\n\
tap 720 456\nwait 1500\ntap 600 160\nwait 1500\ndrag 27 150 770 150 400\nwait 1000\nshot 1-timeout-never\ntap 18 30\nwait 1500\n\
tap 240 456\nwait 1500\nwheel 400 300 300\nwait 800\ntap 300 400\nwait 1500\ntap 535 232\nwait 1500\n\
tap 720 456\nwait 1500\ntap 200 266\nwait 1500\nwheel 400 300 400\nwait 800\n\
tap 744 234\nwait 6000\nshot 1-switch-on\n\
tap 726 316\nwait 1500\ntap 743 370\nwait 300\ntap 743 370\nwait 300\ntap 42 370\nwait 800\n\
tap 104 370\nwait 300\ntap 735 460\nwait 6000\nshot 1-days-2\n\
tap 18 30\nwait 1500\ntap 240 456\nwait 1500\nshot 1-contacts-top\nwheel 400 300 300\nwait 800\nshot 1-contacts\nquit\n" WADA_CLOCK_UNSET=1

# 2: Femke's chat open through the first pass.
run 2 "wait 5000\ntap 240 456\nwait 1500\ntap 695 50\nwait 1000\ntype Femk\nwait 1200\ntap 522 278\nwait 1500\n\
shot 2-search\ntap 300 100\nwait 1500\ntap 265 148\nwait 2000\nshot 2-femke-chat\n${awake}shot 2-chat-after-pass\n\
tap 18 30\nwait 1500\ntap 240 456\nwait 1500\ntap 695 50\nwait 1000\ntap 254 278\nwait 1500\nshot 2-contacts-top\nwheel 400 300 300\nwait 800\nshot 2-contacts\nquit\n"

# 3: Contacts open through the first pass.
run 3 "wait 5000\ntap 240 456\nwait 1500\n${awake}shot 3-contacts-top\nwheel 400 300 300\nwait 800\nshot 3-contacts\nquit\n"
echo "contact-purge: shots in $OUT/shots-{1,2,3}"
