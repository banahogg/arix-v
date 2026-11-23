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
# spirv-opt / spirv-fuzz crashes on an input are saved to tool-crashes/.

set -u

# spirv-opt and spirv-fuzz segfault on some inputs. With kernel.core_pattern
# piped to apport, each crash becomes a /var/crash report and a desktop popup
# (and, with uutils timeout, a second report blaming timeout itself). A core
# limit of 0 does not help: the kernel still invokes the pipe and apport files
# the report anyway. A limit of exactly 1 byte is the kernel's documented
# "skip piped core dumps" value, so set that for this shell and its children.
# bash's ulimit -c counts 1 KiB blocks, hence prlimit.
prlimit --pid $$ --core=1:1 2>/dev/null ||
  echo "warning: prlimit failed; tool crashes may reach apport" >&2

worker=$1
corpus_dir=$2
cli=$3
out=$4

queue="$out/queue"
failures="$out/failures"
tool_crashes="$out/tool-crashes"
mkdir -p "$queue" "$failures" "$tool_crashes"
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

# Record a crash of an upstream mutation tool (exit 128+signal, as passed
# through by timeout; 124 is timeout's own expiry). These are spirv-tools
# bugs, not arix-v ones, but worth keeping for upstream reports.
note_tool_crash() { # <status> <tool> <src> <args...>
  local status=$1 tool=$2 src=$3
  shift 3
  [ "$status" -gt 128 ] && [ "$status" -ne 124 ] || return 0
  local keep
  keep="$tool_crashes/$tool-sig$((status - 128))-$(basename "$src")"
  [ -e "$keep" ] || cp "$src" "$keep"
  echo "=== TOOL CRASH worker=$worker iter=$iter tool=$tool status=$status" \
    "src=$src args=$* -> $keep" >>"$log"
}

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
    note_tool_crash $? spirv-fuzz "$src" --donors="$donors" --seed="$seed"
  fi
  # spirv-opt path, also taken when spirv-fuzz rejected the input.
  if [ ! -s "$mutant" ]; then
    rm -f "$mutant"
    picks=()
    for _ in $(seq $((1 + RANDOM % 4))); do
      picks+=("${opt_passes[RANDOM % ${#opt_passes[@]}]}")
    done
    timeout 30 spirv-opt "$src" -o "$mutant" "${picks[@]}" >>"$log" 2>&1
    note_tool_crash $? spirv-opt "$src" "${picks[@]}"
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
