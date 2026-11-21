#!/usr/bin/env bash
# Endless generator of valid-but-weird SPIR-V modules, round-trip checked.
#
# Random-byte fuzzing mostly exercises the byte-fallback model; this loop
# targets the grammar model by mutating real corpus modules while keeping
# them valid SPIR-V: spirv-fuzz if available, otherwise random spirv-opt
# pass pipelines (also the fallback whenever spirv-fuzz yields no mutant).
# Upstream spirv-fuzz rejects SPIR-V 1.4+ input; see
# docs/development/fuzzing.md for the local patch. Every mutant is
# round-trip checked with an ASan/UBSan CLI build, and also dropped into a
# foreign-sync directory that the AFL++ main instance imports as extra seeds.
#
# Usage: spirv_gen_loop.sh <worker-id> <corpus-dir> <cli-binary> <out-dir>
#   out-dir gets queue/ (AFL -F sync), failures/, and gen.log.
# Honors $RUN_WRAP (e.g. "setarch x86_64 -R") as a prefix for the CLI binary.

set -u

worker=$1
corpus_dir=$2
cli=$3
out=$4

queue="$out/queue"
failures="$out/failures"
mkdir -p "$queue" "$failures"
log="$out/gen-$worker.log"

mapfile -t corpus < <(find "$corpus_dir" -name '*.spv' -size -256k)
if [ "${#corpus[@]}" -eq 0 ]; then
  echo "no corpus files under $corpus_dir" >&2
  exit 1
fi

have_spirv_fuzz=0
command -v spirv-fuzz >/dev/null 2>&1 && have_spirv_fuzz=1

# Passes chosen to reshape modules structurally (inlining, unrolling, block
# merging) so the mutant stresses grammar-model paths the original did not.
opt_passes=(
  -O
  --strip-debug
  --eliminate-dead-code-aggressive
  --inline-entry-points-exhaustive
  --loop-unroll
  --merge-blocks
  --merge-return
  --scalar-replacement=100
  --redundancy-elimination
  --copy-propagate-arrays
  --if-conversion
  --vector-dce
  --private-to-local
  --convert-local-access-chains
)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
donors="$tmp/donors.txt"

iter=0
while :; do
  src=${corpus[RANDOM % ${#corpus[@]}]}
  seed=$((RANDOM * 32768 + RANDOM))
  mutant="$tmp/mutant.spv"
  rm -f "$mutant"

  if [ "$have_spirv_fuzz" -eq 1 ] && [ $((RANDOM % 2)) -eq 0 ]; then
    # spirv-fuzz refuses to fuzz without a donor list; give it a few random
    # corpus modules to splice functions from.
    : >"$donors"
    for _ in 1 2 3 4; do
      printf '%s\n' "${corpus[RANDOM % ${#corpus[@]}]}" >>"$donors"
    done
    timeout 30 spirv-fuzz "$src" -o "$mutant" --donors="$donors" \
      --seed="$seed" >>"$log" 2>&1
  fi
  # spirv-opt path, also taken when spirv-fuzz rejected the input.
  if [ ! -s "$mutant" ]; then
    rm -f "$mutant"
    picks=()
    for _ in $(seq $((1 + RANDOM % 4))); do
      picks+=("${opt_passes[RANDOM % ${#opt_passes[@]}]}")
    done
    timeout 30 spirv-opt "$src" -o "$mutant" "${picks[@]}" >>"$log" 2>&1
  fi
  # Mutation failures (unsupported extensions, timeouts) are expected; skip.
  [ -s "$mutant" ] || { iter=$((iter + 1)); continue; }

  if ! ${RUN_WRAP:-} "$cli" roundtrip "$mutant" >>"$log" 2>&1; then
    keep="$failures/w$worker-$iter-$(basename "$src")"
    cp "$mutant" "$keep"
    echo "=== FAIL worker=$worker iter=$iter seed=$seed src=$src -> $keep" \
      >>"$log"
  fi

  # Feed AFL's foreign sync dir; cycle names to cap the directory size.
  cp "$mutant" "$queue/w$worker-$((iter % 2000)).spv"
  iter=$((iter + 1))
done
