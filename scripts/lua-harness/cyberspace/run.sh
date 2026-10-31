#!/bin/sh
# Build the host runner and run the Cyberspace app test at several board sizes.
#
#   scripts/lua-harness/cyberspace/run.sh               # every size: both room sizes, and touchless
#   scripts/lua-harness/cyberspace/run.sh 320 196       # one size
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
app="$root/deploy/apps/cyberspace/1.0/cyberspace.lua"
bin="$here/host"
if [ ! -x "$bin" ] || [ "$here/host.cpp" -nt "$bin" ] || [ "$root/src/ui-touch/LuaJson.h" -nt "$bin" ]; then
  obj=$(mktemp -d)
  (cd "$obj" && for f in "$root"/lib/lua/src/*.c; do cc -c -O1 -w -I"$root/lib/lua/src" "$f"; done)
  c++ -std=c++17 -w -I"$root/src" -I"$root/lib/lua/src" -o "$bin" "$here/host.cpp" "$obj"/*.o -lm
  rm -rf "$obj"
fi
if [ $# -ge 2 ]; then sizes="$1x$2"; else sizes="320x196 240x280 222x400 480x222 720x1180"; fi
for s in $sizes; do
  w=${s%x*}; h=${s#*x}
  "$bin" "$here/test.lua" "$app" "$w" "$h" | tail -1
  NMSG=230 "$bin" "$here/test.lua" "$app" "$w" "$h" | tail -1
  TOUCH=0 "$bin" "$here/test.lua" "$app" "$w" "$h" | tail -1
done
