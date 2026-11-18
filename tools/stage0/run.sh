#!/bin/bash
# One headless run of Tempest 3000 with the scripted route into level one, for
# docs/speed-stage0-results.md.
#
#   tools/stage0/run.sh <core.dylib> <tag> <seconds> [VAR=value ...]
#
# Writes $STAGE0_OUT/logs/<tag>.log (the core's own log, NUANCE_LOG_FILE) and
# <tag>.err (perf_host's output, which ends with the number of game frames).
# NUANCE_PERF=1 is set unless NOPERF=1 is among the variables given.
# The turn counts and the finer time split need a core built with
# -DNUANCE_PERF_COUNTERS=ON (see docs/speed-stage0-results.md, section 3.1).
# SCRIPT_EXTRA, if set, is added to the input script (for example
# "1200-1205:b" fires the superzapper, which makes a busy scene; START must
# never be pressed once the game is running).
# Environment: STAGE0_OUT (default ./stage0), T3K_ISO (the disc image),
# BUILD (the CMake build directory holding perf_host and the BIOS files,
# default build-libretro under the repository).
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${STAGE0_OUT:-$PWD/stage0}
BUILD=${BUILD:-$REPO/build-libretro}
ISO=${T3K_ISO:-"/Volumes/External/dev/nagori3k/build/test/lib/Tempest 3000.iso"}
core=$1; tag=$2; secs=$3; shift 3
mkdir -p "$OUT/logs" "$OUT/saves/$tag"
rm -f "$OUT/logs/$tag.log"
cd "$BUILD" || exit 1
extra=(NUANCE_T3K_STATE=1)
perf=1
for v in "$@"; do if [ "$v" = "NOPERF=1" ]; then perf=0; else extra+=("$v"); fi; done
if [ $perf = 1 ]; then extra+=(NUANCE_PERF=1); fi
# START (twice, before the game has begun) and then A: the route into level one.
# START is the pause button once the game is running; never repeat it later.
env "${extra[@]}" NUANCE_LOG_FILE="$OUT/logs/$tag.log" \
  NUANCE_INPUT_SCRIPT="60-63:start;120-123:start;250-253:a;330-333:a;450-453:a;550-553:a${SCRIPT_EXTRA:+;$SCRIPT_EXTRA}" \
  ./perf_host "$core" . "$OUT/saves/$tag" "$secs" "$ISO" 2> "$OUT/logs/$tag.err"
tail -n 2 "$OUT/logs/$tag.err"
