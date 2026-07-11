#!/usr/bin/env bash
# Runs a stress command in an endless loop with a fresh --seed each pass,
# recording failing passes without stopping the loop. Used by
# overnight.sh for the non-coverage-guided jobs (integer-sanitizer,
# -m32, valgrind).
#
# Usage: loop_runner.sh <name> <logdir> <command...>
# Each pass runs: <command...> --seed=<random>, stdout+stderr to
# <logdir>/<name>.last.log. A nonzero exit appends the seed, exit code, and
# log tail to <logdir>/<name>.failures.log and the loop continues.

set -u

name=$1
logdir=$2
shift 2
mkdir -p "$logdir"

pass=0
while :; do
  seed=$((($(od -An -N4 -tu4 /dev/urandom) % 0x7FFFFFFF) + 1))
  log="$logdir/$name.last.log"
  "$@" --seed="$seed" >"$log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then
    {
      echo "=== FAIL name=$name pass=$pass seed=$seed exit=$rc at $(date -Is)"
      tail -n 40 "$log"
    } >>"$logdir/$name.failures.log"
  fi
  pass=$((pass + 1))
done
