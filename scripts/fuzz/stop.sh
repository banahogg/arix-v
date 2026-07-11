#!/usr/bin/env bash
# Stops every job launched by overnight.sh, including the fuzz targets
# and loop children the launcher pids don't cover.

set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${FUZZ_OUT:-$ROOT/fuzz-out}"

if [ ! -f "$OUT/pids" ]; then
  echo "no pid file at $OUT/pids" >&2
  exit 1
fi

while read -r pid name; do
  if kill -0 "$pid" 2>/dev/null; then
    # Kill the whole process group when the pid leads one; loop runners and
    # afl-fuzz both spawn children.
    pkill -TERM -P "$pid" 2>/dev/null
    kill -TERM "$pid" 2>/dev/null
    echo "stopped $name ($pid)"
  fi
done <"$OUT/pids"

sleep 2
# Sweep stragglers that outlived their parents (forkserver targets, valgrind).
pkill -TERM -f "$OUT/bin/" 2>/dev/null
pkill -TERM -f spirv_gen_loop.sh 2>/dev/null
pkill -TERM -f loop_runner.sh 2>/dev/null
echo "done; findings remain under $OUT"
