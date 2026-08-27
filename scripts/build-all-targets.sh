#!/usr/bin/env bash
# wadamesh: compile every board this repo can produce firmware for. Builds only —
# nothing is flashed, uploaded or published.
#
# WHY THIS SCRIPT EXISTS: "does it still build?" spans two toolchains. The eight
# S3/PIO boards come from platformio.ini, but the Tanmatsu and the T-Display P4
# are standalone ESP-IDF apps with their own build.sh wrappers, and release.sh
# says so in a comment while building only the PlatformIO half. So a change that
# broke an IDF-only board was invisible until someone cut a release by hand. This
# builds all ten, keeps going after a failure, and prints one table at the end.
#
# Usage:
#   scripts/build-all-targets.sh                 # everything it can build here
#   scripts/build-all-targets.sh --list          # show the targets, build nothing
#   scripts/build-all-targets.sh --pio           # PlatformIO boards only
#   scripts/build-all-targets.sh --idf           # Tanmatsu + T-Display P4 only
#   scripts/build-all-targets.sh --fail-fast     # stop at the first failure
#   scripts/build-all-targets.sh -- -v           # pass the rest through to pio run
#
# Optional:
#   PIO=/path/to/pio     # override the PlatformIO executable (default: on PATH)
#
# The PlatformIO env list is read from platformio.ini rather than hard-coded, so
# adding a board to that file is enough — this cannot drift out of step with it.
#
# Exit status is 0 only when every target that ran succeeded. Skipped IDF targets
# (toolchain not installed on this machine) are reported as SKIP and do NOT fail
# the run — see the note printed at the end for how to enable them.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIO="${PIO:-$(command -v pio || true)}"
LOGDIR="$ROOT/out/build-logs"

DO_PIO=1; DO_IDF=1; LIST_ONLY=0; FAIL_FAST=0; PASSTHRU=()
while [ $# -gt 0 ]; do
  case "$1" in
    --list)      LIST_ONLY=1 ;;
    --pio)       DO_IDF=0 ;;
    --idf)       DO_PIO=0 ;;
    --fail-fast) FAIL_FAST=1 ;;
    --)          shift; PASSTHRU=("$@"); break ;;
    -h|--help)   sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)           echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
  shift
done

# --- Target inventory ----------------------------------------------------------
# PlatformIO envs, in platformio.ini order. Every [env:NAME] is a board we ship.
PIO_ENVS=()
while IFS= read -r e; do PIO_ENVS+=("$e"); done < <(sed -n 's/^\[env:\(.*\)\]$/\1/p' platformio.ini)

# The two ESP-IDF apps. Each is <dir>/build.sh, which needs a project-local
# ESP-IDF 5.5.1; the P4 reuses the Tanmatsu's via symlinks, so one check covers
# both. Absent toolchain = SKIP, not failure: a laptop with only PlatformIO
# installed should still be able to run this and get the eight boards checked.
IDF_DIRS=(tanmatsu tdisplay_p4)
idf_available() { [ -f "$ROOT/tanmatsu/esp-idf/export.sh" ]; }

if [ "$LIST_ONLY" = 1 ]; then
  echo "PlatformIO targets (${#PIO_ENVS[@]}):"
  for e in "${PIO_ENVS[@]}"; do echo "  $e"; done
  echo "ESP-IDF targets (${#IDF_DIRS[@]}):"
  for d in "${IDF_DIRS[@]}"; do
    idf_available && echo "  $d" || echo "  $d   (toolchain not installed here - would SKIP)"
  done
  exit 0
fi

[ "$DO_PIO" = 1 ] && [ -z "$PIO" ] && { echo "ERROR: no 'pio' on PATH - set PIO=/path/to/pio" >&2; exit 1; }

mkdir -p "$LOGDIR"
NAMES=(); RESULTS=(); TIMES=(); FAILED=0

# run_target <label> <logfile> <command...>
run_target() {
  local label="$1" log="$2"; shift 2
  local start; start=$(date +%s)
  printf '\n==> %s\n' "$label"
  if "$@" >"$log" 2>&1; then
    local rc=OK
  else
    local rc=FAIL; FAILED=1
    tail -25 "$log" | sed 's/^/    /'
  fi
  local secs=$(( $(date +%s) - start ))
  NAMES+=("$label"); RESULTS+=("$rc"); TIMES+=("$secs")
  printf '    %s  (%dm%02ds)  log: %s\n' "$rc" $((secs/60)) $((secs%60)) "${log#$ROOT/}"
  [ "$rc" = FAIL ] && [ "$FAIL_FAST" = 1 ] && { summary; exit 1; }
  return 0
}

skip_target() {
  NAMES+=("$1"); RESULTS+=("SKIP"); TIMES+=(0)
  printf '\n==> %s\n    SKIP  (%s)\n' "$1" "$2"
}

summary() {
  printf '\n%s\n' "-------------------------------------------------------------------"
  printf '%-46s %-6s %s\n' "TARGET" "STATUS" "TIME"
  local i total=0
  for i in "${!NAMES[@]}"; do
    printf '%-46s %-6s %dm%02ds\n' "${NAMES[$i]}" "${RESULTS[$i]}" $((TIMES[i]/60)) $((TIMES[i]%60))
    total=$((total + TIMES[i]))
  done
  printf '%s\n' "-------------------------------------------------------------------"
  printf 'total %dm%02ds\n' $((total/60)) $((total%60))
  if [ "$DO_IDF" = 1 ] && ! idf_available; then
    printf '\nnote: the ESP-IDF boards were skipped - install the project-local IDF with\n'
    printf '      tanmatsu/fetch-deps.sh (needs an S3 pio build first), then re-run.\n'
  fi
}

# --- Build ---------------------------------------------------------------------
if [ "$DO_PIO" = 1 ]; then
  for e in "${PIO_ENVS[@]}"; do
    # `pio run` with no -t builds; upload/mergebin are never requested here.
    run_target "$e" "$LOGDIR/$e.log" "$PIO" run -e "$e" ${PASSTHRU+"${PASSTHRU[@]}"}
  done
fi

if [ "$DO_IDF" = 1 ]; then
  for d in "${IDF_DIRS[@]}"; do
    if idf_available; then
      run_target "$d (esp-idf)" "$LOGDIR/$d.log" "$ROOT/$d/build.sh" build
    else
      skip_target "$d (esp-idf)" "no tanmatsu/esp-idf - see tanmatsu/fetch-deps.sh"
    fi
  done
fi

summary
exit $FAILED
