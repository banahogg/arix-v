#!/usr/bin/env bash
# Builds and launches the overnight stress campaign: coverage-guided AFL++
# and libFuzzer runs of the round-trip and hostile-decode harnesses, a
# valid-SPIR-V generator loop, and sanitizer/valgrind/-m32 stress loops.
#
# Everything lives under fuzz-out/ (bin/, logs/, seeds/, afl-*/, lf-*/,
# spirvgen/, pids). Re-running resumes the AFL campaigns (AFL_AUTORESUME).
# Stop with scripts/fuzz/stop.sh; summarize with scripts/fuzz/triage.sh.
#
# Requires: afl++ clang valgrind gcc-multilib g++-multilib spirv-tools
#   (sudo apt install afl++ clang lld llvm valgrind gcc-multilib \
#      g++-multilib spirv-tools)

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${FUZZ_OUT:-$ROOT/fuzz-out}"
CORPUS="${ARIXV_CORPUS:-$ROOT/corpus/spv}"
GRAMMAR="${ARIXV_GRAMMAR:-$ROOT/grammar-deps/spirv-headers/include/spirv/unified1/spirv.core.grammar.json}"
BIN="$OUT/bin"
LOGS="$OUT/logs"
SEEDS="$OUT/seeds"
cd "$ROOT"

if [ ! -d "$CORPUS" ]; then
  echo "SPIR-V corpus is missing: $CORPUS" >&2
  echo "run: python3 scripts/fetch_pinned_repos.py && python3 scripts/build_spv_corpus.py" >&2
  exit 1
fi
if [ ! -f "$GRAMMAR" ]; then
  echo "SPIR-V grammar is missing: $GRAMMAR" >&2
  echo "run: python3 scripts/fetch_pinned_repos.py --lock deps/grammar-sources.lock.json --dest grammar-deps" >&2
  exit 1
fi

LIB_SRCS="src/arixv.cpp src/arixv_encode.cpp src/model.cpp src/tracker.cpp \
  src/operand_cursor.cpp src/grammar_tables.cpp \
  src/arixv_pack.cpp src/arixv_pack_encode.cpp"
COMMON_FLAGS="-std=c++17 -pthread -Iinclude -Isrc -DARIXV_ENCODER=1"

# Optional command prefix for hosts that need to wrap sanitizer processes.
# For example: RUN_WRAP="setarch $(uname -m) -R" scripts/fuzz/overnight.sh
RUN_WRAP="${RUN_WRAP:-}"
export RUN_WRAP

command -v afl-fuzz >/dev/null || {
  echo "afl++ not installed; see header for the apt command" >&2
  exit 1
}
command -v clang++ >/dev/null || {
  echo "clang not installed; see header for the apt command" >&2
  exit 1
}

mkdir -p "$BIN" "$LOGS" "$SEEDS/rt" "$SEEDS/dec" "$OUT/spirvgen/queue" \
  "$OUT/lf-rt/corpus" "$OUT/lf-rt/artifacts" \
  "$OUT/lf-dec/corpus" "$OUT/lf-dec/artifacts"

# ---------------------------------------------------------------- builds ---
echo "== building fuzz targets"

# clang-17 in this environment segfaults sporadically (same class of
# problem as the documented ASan-runtime startup crashes), so every build
# gets a few attempts before we give up on it.
try_build() { # <out> <compiler...> — appends -o $BIN/<out>
  local out=$1
  shift
  local attempt
  for attempt in 1 2 3; do
    rm -f "$BIN/$out"
    if "$@" -o "$BIN/$out" >"$LOGS/build-$out.log" 2>&1 &&
      [ -x "$BIN/$out" ]; then
      return 0
    fi
    echo "  build of $out failed (attempt $attempt), retrying"
  done
  return 1
}

# AFL++ instrumented harnesses. -fsanitize=fuzzer makes afl-clang-fast++
# link AFL's libFuzzer-compatible driver (persistent mode).
# -fno-crash-diagnostics: on a compiler crash, fail fast instead of hanging
# for minutes in LLVM's crash-report machinery (broken in this environment).
# No AFL_LLVM_LAF_ALL variants: those passes crash this clang outright;
# CMPLOG covers the same guess-the-comparison ground.
afl_build() { # <out> <harness> [env VAR=1 ...]
  local out=$1 harness=$2
  shift 2
  try_build "$out" env "$@" AFL_QUIET=1 afl-clang-fast++ -O2 -g $COMMON_FLAGS \
    -fsanitize=fuzzer -fno-crash-diagnostics "$harness" $LIB_SRCS
}
afl_build rt-afl tools/fuzz/fuzz_roundtrip_cov.cpp
afl_build dec-afl tools/fuzz/fuzz_decode_cov.cpp
afl_build rt-afl-cmplog tools/fuzz/fuzz_roundtrip_cov.cpp AFL_LLVM_CMPLOG=1
afl_build dec-afl-cmplog tools/fuzz/fuzz_decode_cov.cpp AFL_LLVM_CMPLOG=1
afl_build rt-afl-asan tools/fuzz/fuzz_roundtrip_cov.cpp AFL_USE_ASAN=1 AFL_USE_UBSAN=1
afl_build dec-afl-asan tools/fuzz/fuzz_decode_cov.cpp AFL_USE_ASAN=1 AFL_USE_UBSAN=1

# libFuzzer builds: in-process, value profile, ASan+UBSan.
try_build rt-libfuzzer clang++ -O1 -g $COMMON_FLAGS -fno-crash-diagnostics \
  -fsanitize=fuzzer,address,undefined tools/fuzz/fuzz_roundtrip_cov.cpp $LIB_SRCS
try_build dec-libfuzzer clang++ -O1 -g $COMMON_FLAGS -fno-crash-diagnostics \
  -fsanitize=fuzzer,address,undefined tools/fuzz/fuzz_decode_cov.cpp $LIB_SRCS

# Unsigned-wraparound hunter: not UB, so plain UBSan stays silent, but in
# coder arithmetic a wrap is usually a bug. Recovering so one noisy site
# cannot mask others; triage dedupes the log by source location.
try_build rt-intsan clang++ -O1 -g $COMMON_FLAGS -fno-crash-diagnostics \
  -fsanitize=address,undefined,unsigned-integer-overflow \
  -fsanitize-recover=unsigned-integer-overflow \
  tools/fuzz/fuzz_roundtrip.cpp $LIB_SRCS

# 32-bit build: size_t shrinks to 4 bytes, exposing offset+length overflow
# unreachable on 64-bit. ASan if the 32-bit runtime is present, else plain.
if ! try_build rt-m32 g++ -m32 -O1 -g $COMMON_FLAGS \
  -fsanitize=address,undefined tools/fuzz/fuzz_roundtrip.cpp $LIB_SRCS
then
  echo "  (32-bit sanitizer runtime unavailable; building -m32 plain)"
  try_build rt-m32 g++ -m32 -O2 -g $COMMON_FLAGS \
    tools/fuzz/fuzz_roundtrip.cpp $LIB_SRCS
fi

# Valgrind wants an uninstrumented binary with symbols.
try_build rt-vg g++ -O1 -g $COMMON_FLAGS tools/fuzz/fuzz_roundtrip.cpp $LIB_SRCS

# CLI builds: plain for seed encoding, ASan for the generator loop's checks.
try_build arixv g++ -O2 $COMMON_FLAGS cli/main.cpp $LIB_SRCS
try_build arixv-asan clang++ -O1 -g $COMMON_FLAGS -fno-crash-diagnostics \
  -fsanitize=address,undefined cli/main.cpp $LIB_SRCS

# ----------------------------------------------------------------- seeds ---
echo "== preparing seeds and dictionary"

python3 scripts/fuzz/gen_afl_dict.py "$GRAMMAR" >"$OUT/spirv.dict"

# Round-trip seeds: corpus modules, size-capped to keep execs fast.
find "$CORPUS" -name '*.spv' -size -33k -exec cp --update=none {} "$SEEDS/rt/" \;
printf 'a' >"$SEEDS/rt/tiny"
# Minimal module header: magic, version 1.0, generator 0, bound 1, schema 0.
printf '\x03\x02\x23\x07\x00\x00\x01\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00' \
  >"$SEEDS/rt/header-only.spv"

# Decode seeds: real encoded blobs, so mutation starts inside the container
# format instead of dying at validation.
if [ -z "$(ls -A "$SEEDS/dec")" ]; then
  for f in "$SEEDS/rt"/*; do
    "$BIN/arixv" c "$f" "$SEEDS/dec/$(basename "$f").arixv" >/dev/null || true
  done
fi

cp --update=none "$SEEDS/rt"/* "$OUT/lf-rt/corpus/" 2>/dev/null || true
cp --update=none "$SEEDS/dec"/* "$OUT/lf-dec/corpus/" 2>/dev/null || true

# ---------------------------------------------------------------- launch ---
echo "== launching campaign"
: >"$OUT/pids"

launch() { # <name> <command...>
  local name=$1
  shift
  nohup "$@" >"$LOGS/$name.out" 2>&1 &
  echo "$! $name" >>"$OUT/pids"
}

# Keep crashes away from apport (kernel.core_pattern pipes to it, and each
# packaged binary that crashes, e.g. spirv-opt, becomes a desktop popup).
# A core limit of 0 still invokes the pipe; exactly 1 byte makes the kernel
# skip it. Every job launched below inherits this.
prlimit --pid $$ --core=1:1 2>/dev/null ||
  echo "warning: prlimit failed; tool crashes may reach apport" >&2

# AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES: kernel.core_pattern pipes to apport
# and we have no sudo to change it; crashes still surface via the forkserver.
AFL_ENV=(AFL_AUTORESUME=1 AFL_SKIP_CPUFREQ=1 AFL_NO_UI=1
  AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 AFL_IMPORT_FIRST=1)

afl_rt() { # <name> <extra afl args...> -- <target>
  local name=$1
  shift
  launch "$name" env "${AFL_ENV[@]}" afl-fuzz -i "$SEEDS/rt" -o "$OUT/afl-rt" \
    -m none -t 5000 -G 65536 "$@"
}
afl_dec() {
  local name=$1
  shift
  launch "$name" env "${AFL_ENV[@]}" afl-fuzz -i "$SEEDS/dec" -o "$OUT/afl-dec" \
    -m none -t 5000 -G 65536 "$@"
}

# Round-trip campaign: 11 instances.
afl_rt rt-main -M rt-main -x "$OUT/spirv.dict" -F "$OUT/spirvgen/queue" -- "$BIN/rt-afl"
afl_rt rt-c1 -S rt-c1 -c "$BIN/rt-afl-cmplog" -l 2 -- "$BIN/rt-afl"
afl_rt rt-c2 -S rt-c2 -c "$BIN/rt-afl-cmplog" -l 2AT -- "$BIN/rt-afl"
afl_rt rt-p1 -S rt-p1 -P exploit -- "$BIN/rt-afl"
afl_rt rt-p2 -S rt-p2 -x "$OUT/spirv.dict" -- "$BIN/rt-afl"
afl_rt rt-p3 -S rt-p3 -- "$BIN/rt-afl"
afl_rt rt-p4 -S rt-p4 -x "$OUT/spirv.dict" -- "$BIN/rt-afl"
afl_rt rt-p5 -S rt-p5 -- "$BIN/rt-afl"
# ASan instances: slower execs, so shorter inputs and a looser timeout.
launch rt-a1 env "${AFL_ENV[@]}" $RUN_WRAP afl-fuzz -i "$SEEDS/rt" \
  -o "$OUT/afl-rt" -m none -t 10000 -G 16384 -S rt-a1 -- "$BIN/rt-afl-asan"
launch rt-a2 env "${AFL_ENV[@]}" $RUN_WRAP afl-fuzz -i "$SEEDS/rt" \
  -o "$OUT/afl-rt" -m none -t 10000 -G 16384 -S rt-a2 -- "$BIN/rt-afl-asan"

# Hostile-decode campaign: 8 instances.
afl_dec dec-main -M dec-main -- "$BIN/dec-afl"
afl_dec dec-c1 -S dec-c1 -c "$BIN/dec-afl-cmplog" -l 2 -- "$BIN/dec-afl"
afl_dec dec-c2 -S dec-c2 -c "$BIN/dec-afl-cmplog" -l 2AT -- "$BIN/dec-afl"
afl_dec dec-p1 -S dec-p1 -P exploit -- "$BIN/dec-afl"
afl_dec dec-p2 -S dec-p2 -- "$BIN/dec-afl"
launch dec-a1 env "${AFL_ENV[@]}" $RUN_WRAP afl-fuzz -i "$SEEDS/dec" \
  -o "$OUT/afl-dec" -m none -t 10000 -G 16384 -S dec-a1 -- "$BIN/dec-afl-asan"
launch dec-a2 env "${AFL_ENV[@]}" $RUN_WRAP afl-fuzz \
  -i "$SEEDS/dec" -o "$OUT/afl-dec" -m none -t 10000 -G 16384 -S dec-a2 \
  -- "$BIN/dec-afl-asan"

# libFuzzer: 2 forked workers each, value profile, keep going past crashes.
LF_ARGS=(-fork=2 -ignore_crashes=1 -ignore_ooms=1 -ignore_timeouts=1
  -max_len=65536 -use_value_profile=1 -rss_limit_mb=4096)
launch lf-rt env ASAN_OPTIONS=detect_leaks=0 $RUN_WRAP "$BIN/rt-libfuzzer" \
  "${LF_ARGS[@]}" -artifact_prefix="$OUT/lf-rt/artifacts/" "$OUT/lf-rt/corpus"
launch lf-dec env ASAN_OPTIONS=detect_leaks=0 $RUN_WRAP "$BIN/dec-libfuzzer" \
  "${LF_ARGS[@]}" -artifact_prefix="$OUT/lf-dec/artifacts/" "$OUT/lf-dec/corpus"

# Valid-SPIR-V generator loop: 2 workers feeding rt-main via -F.
if command -v spirv-opt >/dev/null; then
  launch gen1 env ASAN_OPTIONS=detect_leaks=0 scripts/fuzz/spirv_gen_loop.sh 1 \
    "$CORPUS" "$BIN/arixv-asan" "$OUT/spirvgen"
  launch gen2 env ASAN_OPTIONS=detect_leaks=0 scripts/fuzz/spirv_gen_loop.sh 2 \
    "$CORPUS" "$BIN/arixv-asan" "$OUT/spirvgen"
else
  echo "  (spirv-tools not installed; skipping generator loop)"
fi

# Stress loops on the existing randomized harness.
launch intsan env ASAN_OPTIONS=detect_leaks=0 \
  UBSAN_OPTIONS=halt_on_error=0:print_stacktrace=0:log_path="$LOGS/intsan-ubsan" \
  $RUN_WRAP scripts/fuzz/loop_runner.sh intsan "$LOGS" \
  "$BIN/rt-intsan" --iters=20000 "$CORPUS"/*.spv
launch m32 env ASAN_OPTIONS=detect_leaks=0 $RUN_WRAP \
  scripts/fuzz/loop_runner.sh m32 "$LOGS" \
  "$BIN/rt-m32" --iters=20000 "$CORPUS"/*.spv
launch valgrind scripts/fuzz/loop_runner.sh valgrind "$LOGS" \
  valgrind --quiet --error-exitcode=99 "$BIN/rt-vg" --iters=300 \
  "$CORPUS"/*.spv

echo
echo "launched $(wc -l <"$OUT/pids") jobs; pids in $OUT/pids, logs in $LOGS"
echo "status:  afl-whatsup -s $OUT/afl-rt; afl-whatsup -s $OUT/afl-dec"
echo "triage:  scripts/fuzz/triage.sh"
echo "stop:    scripts/fuzz/stop.sh"
