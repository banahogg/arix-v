#!/usr/bin/env bash
# Morning-after summary for the overnight.sh campaign: campaign health,
# every finding bucketed by failure signature, and repro commands. Writes
# fuzz-out/report-<date>.txt and prints it.

set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${FUZZ_OUT:-$ROOT/fuzz-out}"
BIN="$OUT/bin"
LOGS="$OUT/logs"
REPORT="$OUT/report-$(date +%Y%m%d-%H%M).txt"
RUN_WRAP="${RUN_WRAP:-}"

exec > >(tee "$REPORT") 2>&1

section() { printf '\n===== %s =====\n' "$1"; }

section "campaign status"
for dir in "$OUT/afl-rt" "$OUT/afl-dec"; do
  [ -d "$dir" ] && afl-whatsup -s "$dir" 2>/dev/null
done
echo "running jobs: $(awk '{print $1}' "$OUT/pids" 2>/dev/null |
  xargs -r -n1 sh -c 'kill -0 "$0" 2>/dev/null && echo x' | wc -l) / $(wc -l \
  <"$OUT/pids" 2>/dev/null || echo 0)"

# Reproduce each crash input against the matching libFuzzer binary (it runs
# single files given as arguments) and bucket by the sanitizer SUMMARY line
# or the harness FAIL message.
classify() { # <binary> <files...>
  local bin=$1
  shift
  for f in "$@"; do
    sig=$(env ASAN_OPTIONS=detect_leaks=0 $RUN_WRAP "$bin" "$f" 2>&1 |
      grep -m1 -E 'SUMMARY: |^FAIL' || echo "no signature (flaky/stateful?)")
    printf '%s\t%s\n' "$sig" "$f"
  done
}

section "round-trip findings (afl + libFuzzer)"
mapfile -t rt_crashes < <(
  find "$OUT/afl-rt" -path '*/crashes/id*' -type f 2>/dev/null
  find "$OUT/lf-rt/artifacts" -name 'crash-*' -type f 2>/dev/null
)
if [ "${#rt_crashes[@]}" -eq 0 ]; then
  echo "none"
else
  classify "$BIN/rt-libfuzzer" "${rt_crashes[@]}" | sort |
    awk -F'\t' '{n[$1]++; if (!(($1) in ex)) ex[$1]=$2}
      END {for (s in n) printf "%5d  %s\n       e.g. %s\n", n[s], s, ex[s]}'
  echo "minimize a case: afl-tmin -i <case> -o min -- $BIN/rt-afl"
fi

section "hostile-decode findings (afl + libFuzzer)"
mapfile -t dec_crashes < <(
  find "$OUT/afl-dec" -path '*/crashes/id*' -type f 2>/dev/null
  find "$OUT/lf-dec/artifacts" -name 'crash-*' -type f 2>/dev/null
)
if [ "${#dec_crashes[@]}" -eq 0 ]; then
  echo "none"
else
  classify "$BIN/dec-libfuzzer" "${dec_crashes[@]}" | sort |
    awk -F'\t' '{n[$1]++; if (!(($1) in ex)) ex[$1]=$2}
      END {for (s in n) printf "%5d  %s\n       e.g. %s\n", n[s], s, ex[s]}'
  echo "minimize a case: afl-tmin -i <case> -o min -- $BIN/dec-afl"
fi

section "generator-loop round-trip failures (valid SPIR-V mutants)"
if compgen -G "$OUT/spirvgen/failures/*" >/dev/null; then
  ls -la "$OUT/spirvgen/failures/"
  echo "repro: $RUN_WRAP $BIN/arixv-asan roundtrip <file>"
else
  echo "none"
fi

section "unsigned-integer-overflow sites (intsan, deduped by location)"
if compgen -G "$LOGS/intsan-ubsan.*" >/dev/null; then
  grep -h "runtime error" "$LOGS"/intsan-ubsan.* 2>/dev/null |
    sed 's/^\([^ ]*\).*runtime error: \(.*\)/\1 \2/' | sort | uniq -c |
    sort -rn | head -40
else
  echo "none logged"
fi

section "stress-loop failures (intsan / m32 / valgrind)"
found_loop_fail=0
for f in "$LOGS"/*.failures.log; do
  [ -e "$f" ] || continue
  found_loop_fail=1
  echo "--- $f"
  grep -c '^=== FAIL' "$f" | xargs echo "failing passes:"
  tail -n 45 "$f"
done
[ "$found_loop_fail" -eq 0 ] && echo "none"

echo
echo "full report saved to $REPORT"
