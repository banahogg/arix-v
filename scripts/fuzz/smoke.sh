#!/usr/bin/env bash
# Short coverage-guided fuzz run: libFuzzer + ASan/UBSan on the round-trip
# and hostile-decode harnesses for single modules (ARX2) and shader packs
# (ARXP), several minutes in total. Sits between the
# deterministic CTest smoke fuzzer and scripts/fuzz/overnight.sh; run it
# (via `cmake --preset fuzz-smoke && cmake --build --preset fuzz-smoke --target fuzz-smoke`)
# after major model or tracker changes and before pushing. Exits non-zero on
# any crash or sanitizer report.
#
# Knobs (environment):
#   FUZZ_SMOKE_SECONDS  seconds per harness (default 120)
#   FUZZ_SMOKE_JOBS     parallel fuzzing processes (default: nproc)
#   FUZZ_SMOKE_ALLOW_NONCRASH  set to 1 to treat timeout/OOM artifacts as warnings
#   ARIXV_CORPUS        SPIR-V seed directory (default corpus/spv)
#   BUILD_DIR           where the harness binaries live (default build/fuzz-smoke)
#   RUN_WRAP            optional process wrapper, e.g. "setarch $(uname -m) -R"
#
# The working corpora persist in $BUILD_DIR/fuzz-smoke/*/corpus, so repeated
# runs start from previously reached coverage; removing $BUILD_DIR resets them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SECS="${FUZZ_SMOKE_SECONDS:-120}"
JOBS="${FUZZ_SMOKE_JOBS:-$(nproc)}"
ALLOW_NONCRASH="${FUZZ_SMOKE_ALLOW_NONCRASH:-0}"
BUILD="${BUILD_DIR:-build/fuzz-smoke}"
CORPUS="${ARIXV_CORPUS:-corpus/spv}"
SEED_KB="${FUZZ_SMOKE_SEED_KB:-100000}"
RUN_WRAP="${RUN_WRAP:-}"
OUT="$BUILD/fuzz-smoke"

# Pack harnesses are absent in ARIXV_BUILD_PACK=OFF builds; they are skipped.
PACK=1
for bin in "$BUILD/fuzz_pack_rt_lf" "$BUILD/fuzz_pack_dec_lf"; do
  [ -x "$bin" ] || PACK=0
done
for bin in "$BUILD/fuzz_roundtrip_lf" "$BUILD/fuzz_decode_lf" "$BUILD/arixv"; do
  [ -x "$bin" ] || { echo "fuzz-smoke: missing $bin (build via cmake --preset fuzz-smoke)" >&2; exit 2; }
done
if [ ! -d "$CORPUS" ]; then
  echo "fuzz-smoke: SPIR-V corpus is missing: $CORPUS" >&2
  echo "run: python3 scripts/fetch_pinned_repos.py && python3 scripts/build_spv_corpus.py" >&2
  exit 2
fi

for h in rt dec pack-rt pack-dec; do
  mkdir -p "$OUT/$h"/{seeds,corpus,artifacts}
done

# Round-trip seeds: corpus modules up to FUZZ_SMOKE_SEED_KB, plus a tiny
# non-SPIR-V input and a header-only module.
rm -f "$OUT"/rt/seeds/*
find "$CORPUS" -name '*.spv' -size -"${SEED_KB}"k -exec cp {} "$OUT/rt/seeds/" \;
printf 'a' >"$OUT/rt/seeds/tiny"
printf '\x03\x02\x23\x07\x00\x00\x01\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00' \
  >"$OUT/rt/seeds/header-only.spv"

# Decode seeds: encoded blobs, so mutation starts inside the container format.
# Re-encoded every run because the wire format can change between runs.
rm -f "$OUT"/dec/seeds/*
for f in "$OUT"/rt/seeds/*; do
  "$BUILD/arixv" c "$f" "$OUT/dec/seeds/$(basename "$f").arixv" >/dev/null 2>&1 || true
done

# Pack seeds: small groups of modules. pack-rt takes them concatenated (the
# harness splits at SPIR-V magic words); pack-dec takes them packed, under a
# few layouts, so mutation starts inside the pack format.
rm -f "$OUT"/pack-rt/seeds/* "$OUT"/pack-dec/seeds/*
seed_files=()
if [ "$PACK" = 1 ]; then
  mapfile -t seed_files < <(find "$OUT/rt/seeds" -type f -size -64k | sort)
fi
group=0
for ((i = 0; i + 1 < ${#seed_files[@]} && group < 200; i += 3, group++)); do
  members=("${seed_files[@]:i:4}")
  cat "${members[@]}" >"$OUT/pack-rt/seeds/group$group"
  # shellcheck disable=SC2059
  printf "$(printf '\\%03o' $((group % 256)))" >>"$OUT/pack-rt/seeds/group$group"
  for opts in "--parallelism 1" "--parallelism 4" "--parallelism 8 --fast-decode"; do
    # shellcheck disable=SC2086
    "$BUILD/arixv" pack $opts "$OUT/pack-dec/seeds/group$group-${opts// /_}.arxp" \
      "${members[@]}" >/dev/null 2>&1 || true
  done
done

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

failed=0
run() { # <name> <binary>
  local name=$1 bin=$2 dir="$OUT/$1" status=0
  rm -f "$dir"/artifacts/*
  echo "== fuzz-smoke $name: ${SECS}s x ${JOBS} jobs (log: $dir/fuzz.log)"
  # Fork mode stops at the first crash and exits non-zero; timeouts and OOMs
  # are recorded as artifacts and fail the run unless FUZZ_SMOKE_ALLOW_NONCRASH=1.
  $RUN_WRAP "$bin" -fork="$JOBS" -max_total_time="$SECS" -timeout=10 \
    -rss_limit_mb=4096 -print_final_stats=1 \
    -artifact_prefix="$dir/artifacts/" "$dir/corpus" "$dir/seeds" \
    >"$dir/fuzz.log" 2>&1 || status=$?
  local crashes others
  crashes=$(find "$dir/artifacts" -name 'crash-*' | wc -l)
  others=$(find "$dir/artifacts" -type f ! -name 'crash-*' | wc -l)
  grep -E "^#[0-9]+: cov:" "$dir/fuzz.log" | tail -1 || true
  if [ "$status" -ne 0 ] || [ "$crashes" -ne 0 ]; then
    echo "FAIL: fuzz-smoke $name (exit $status, $crashes crash artifact(s) in $dir/artifacts)"
    grep -m1 -A12 -E "ERROR: (AddressSanitizer|libFuzzer)|runtime error:|FAIL:" \
      "$dir/fuzz.log" || tail -20 "$dir/fuzz.log"
    failed=1
  elif [ "$others" -ne 0 ] && [ "$ALLOW_NONCRASH" != "1" ]; then
    echo "FAIL: fuzz-smoke $name ($others timeout/oom artifact(s) in $dir/artifacts)"
    failed=1
  else
    echo "ok: fuzz-smoke $name"
    if [ "$others" -ne 0 ]; then
      echo "note: $others timeout/oom artifact(s) in $dir/artifacts (allowed via FUZZ_SMOKE_ALLOW_NONCRASH=1)"
    fi
  fi
}

run rt "$BUILD/fuzz_roundtrip_lf"
run dec "$BUILD/fuzz_decode_lf"
if [ "$PACK" = 1 ]; then
  run pack-rt "$BUILD/fuzz_pack_rt_lf"
  run pack-dec "$BUILD/fuzz_pack_dec_lf"
else
  echo "note: pack harnesses not built (ARIXV_BUILD_PACK=OFF); skipped"
fi

if [ "$failed" -ne 0 ]; then
  echo "fuzz-smoke: FAILED (reproduce: $BUILD/fuzz_<harness>_lf <artifact>;"
  echo "  pack-rt/pack-dec artifacts: $BUILD/fuzz_pack_rt_lf / fuzz_pack_dec_lf)"
  exit 1
fi
echo "fuzz-smoke: all harnesses clean"
