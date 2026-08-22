#!/bin/sh
# Build the host Lua and run the harness. Default app: gpscompass.
#
#   scripts/lua-harness/run.sh                          # all scenarios
#   scripts/lua-harness/run.sh <app.lua>                # a different app
#   SCENARIO=declination scripts/lua-harness/run.sh     # just one
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
app=${1:-$root/deploy/apps/gpscompass/1.0/gpscompass.lua}

# rebuild only when something actually changed
if [ ! -x "$here/luah" ] || [ "$here/main.c" -nt "$here/luah" ]; then
  cc -O1 -w -I"$root/lib/lua/src" -o "$here/luah" "$here/main.c" "$root"/lib/lua/src/*.c -lm
fi
exec "$here/luah" "$here/harness.lua" "$app" ${SCENARIO:+"$SCENARIO"}
