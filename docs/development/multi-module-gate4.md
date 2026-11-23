# Multi-module gate 4 (in progress): decode gap, native planner, parallel blocks, new corpora

Research only (Track B of the [multi-module plan](multi-module.md), after
[gate 3](multi-module-gate3.md)). `AXST` remains a research framing, not a
format proposal. Single-module `ARX2`, the golden vectors, the public API, and
the standard translation units are unchanged (`src/model.cpp` is not modified
by this phase). Everything builds only with `ARIXV_BUILD_RESEARCH_PACK=ON`.

> **Retired tooling (library 1.1).** The block codec described here now ships
> as the ARXP pack API (`src/pack_block.inc`, `src/arixv_pack.cpp`). The AXST
> framing, the ARXP research mirror, `arixv_struct_probe` and
> `arixv_struct_parallel` were removed then; check out commit `99df5ed` to
> rerun the commands below. `arixv_pack_api_bench` remains, on the public API.
> The raw results cited as `build/gate4/...` were kept, uncommitted, in
> `corpus/research/gate4/` of the original checkout.

Status, 2026-11-22:

- The decision after gate 3 was to pursue it: about −24% at about 1.2×
  single-thread decode is close enough that the gap is plausibly closable.
  Any multi-worker design must keep the interface simple and fit an engine
  job system that owns the threading.
- Done in this phase:
  - the decode gap is narrowed and fully characterized;
  - a native C++ planner replaces the Python plan;
  - immutable shared base sets enable block-parallel decode;
  - eight independent real-application shader sets are evaluated;
  - long fuzz campaigns ran.
- The size/speed **design choice is the user's**. See
  [Decision needed](#decision-needed).

## Summary

1. **On held-out real application sets, solid AXST is 47–79% smaller than
   ARX2.** It is also far below xz -9e (§4):
   - Wicked Engine: −68%;
   - 0 A.D.: −69%, on hash-deduplicated shaders;
   - Xenia: −79%;
   - Sascha Willems' examples: −50% for each producer.

   This replaces the gate-3 caveat that only two application-derived
   origins existed.
2. **The per-pack parallelism problem is solvable with a two-stage job
   graph.** Base sub-blocks decode in parallel and export an immutable base
   set; independent blocks then copy from it.
   - At 16 workers, Dota 2 decodes in 2.3 ms at ratio 0.056. ARX2 takes
     1.6 ms at 0.149, and the solid stream 20.5 ms at 0.040.
   - Wicked takes 45 ms at 0.047. ARX2 takes 28 ms at 0.111, and the solid
     stream 446 ms at 0.035.
   - Large application sets land at 1.4–1.8× ARX2's 16-worker latency.
     Sets with few or large modules land at about 3×.
3. **Single-worker decode is 1.15–1.25× ARX2 on duplicate-free sets.** Most
   of the remaining cost is inherent: a copied instruction must still advance
   the model's tracker state. Skipping probability training for copied
   instructions ("no-adapt") trades about 0.5 points of ratio for about 7
   points of decode time.

## 1. Decode gap

Single worker, Release, pinned core, best of 7 (two runs pooled per cell),
the same 470 frozen packs. Full table:
`build/gate4/results/decode_gap.md`.

| Group | copy+warm size vs ARX2 | copy+warm time | no-adapt size | no-adapt time | dedup+warm time (no copies) |
|---|---|---|---|---|---|
| Duplicate-free rep collections | −23.9% | 1.22× | −23.5% | 1.15× | 1.07× |
| All rep collections | −41.1% | 1.18× | −40.6% | 1.09× | 1.02× |
| Dota 2 | −73.0% | 1.09× | −72.5% | 0.95× | 0.91× |
| Dota 2 (#unique) | −69.0% | 1.27× | −68.3% | 1.08× | 1.05× |
| Blender | −27.5% | 1.23× | −27.2% | 1.12× | 1.04× |
| Vulkan-Samples collections | −24.8% | 1.21× | −24.2% | 1.16× | 1.08× |
| Stage pairs | −8.6% | 1.14× | −8.2% | 1.13× | 1.08× |
| Singletons (control) | 0 | 1.08× | 0 | 1.06× | 1.06× |

What changed since gate 3:

- **Correspondence tables.** Gate 3 used dense tables sized by the base's ID
  bound, capped at its word count, with a hash map above the cap. Dota 2
  modules have bounds about 12× their word count, so most restorations fell
  into the hash map: about 32k allocations and the per-module map teardown.
  Each base now has a stamped open-addressing table sized from its own ID
  words, reused by every target without clearing and independent of the
  bound. On Dota 2 (#unique) this cut decode from 1.37× to 1.22× on a busy
  machine. A sort-built slot index was tried first and was slower.
- **No-adapt (research flag `kStructNoAdapt`).** Copied instructions run
  through the step with a coder that updates neither probabilities nor CDFs.
  Only the tracker and ID state advance.
  - Cost: +0.2–2% of AXST bytes (Dota 2 +2.0%, Blender +0.4%, Wicked +2.0%,
    0 A.D. +0.9%).
  - Benefit: 5–20 points less decode time.

Where the remaining time goes (callgrind instruction counts):

- **Train-through.** A copied instruction costs about 3.6k instructions
  with adaptation and 2.7k without, against about 3.5k for an ARX2 decode.
  Tracker and recency-list work is about 75% of the per-instruction model
  cost and cannot be skipped, because later literal instructions predict
  from it. Copying saves bytes, not model work.
- **Fixed overhead, about 5% of instructions even with no copies**
  (singletons and dedup-only packs show 1.06–1.08×):
  - about 2 points in the generated step: loop-carried state through a
    struct, and role recording;
  - about 1 point from the per-instruction copy-start flag;
  - the rest is output zeroing and copying, and frame bookkeeping.

  Force-inlining the generated step did not help (within ±3% timing noise).
- **Update (2026-11-22): production hook.** `CodeModule` is now split into
  force-inlined pieces (`CodeModuleHeader`, `CodeInstruction`,
  `CodeModuleTail`) with an optional hook. The default `NoModuleHook`
  compiles away; branch `pack/codemodule-hook`, on `main`. Against `main`:
  - all 1,080 corpus encodes are byte-identical;
  - stats output is identical;
  - decode uses 0.4% fewer instructions, at unchanged throughput;
  - CTest passes on every preset, and fuzz-smoke is clean.

  The research codec now runs on it (branch `research/pack-on-main`),
  replacing the generated copy and its check. All 470 frozen packs (2,820
  AXST size cells) and 24 AXSB pack cases are byte-identical.

  Fixed overhead on singletons fell from 1.06–1.08× to 1.036–1.038× ARX2.
  The rest is container work: output zeroing and copying, frame
  bookkeeping and role recording. A production decoder writing straight
  into caller outputs removes most of the copying.
- **Encoder-path strings.** On string-heavy sets (Wicked's debug names) the
  encoder-path string coder used for train-through costs more than string
  decoding: about 4% of Wicked's decode instructions. A decoder-semantics
  train for strings is a production optimization candidate.

Wicked is 1.21× ARX2 in instructions. Its wall-time gap is larger, probably
from cache effects of its 21 MB of retained bases; this needs measuring.

## 2. Native planner

`PlanStructPack` (`src/research_struct.inc`, encoder builds only) is a C++
port of the gate-2 greedy parser:

- ID-abstract tokens;
- 3-instruction hash chains, depth 8;
- the same adaptive cost estimates.

Literal costs come from a cost pass of the unchanged model. `CostBits`
trains like the encoder and sums `-log2 p` per decision, so no stats build
is needed.

- **Update (2026-11-22): integer costs.** All planner and block-assignment
  arithmetic is now integer: Q16 fixed-point bits, an exact integer `log2`,
  and the budget in integer percent.
  - **Size:** 1 of 470 packs changed by 3 bytes (+0.0003% in total), and 7
    of 24 AXSB cases by at most 21 bytes (+0.0007% in total).
  - **Determinism:** streams are byte-identical across gcc -O3, gcc -O0,
    and clang `-march=native -ffast-math`.

- **Size.** Across all 470 packs it is never worse than the Python plans.
  The total is 43 B smaller (Dota 2 −18 B); every other pack is identical.
- **Speed.** Planning all 470 packs takes 0.76 s in a RelWithDebInfo build.
  Dota 2 takes 53 ms, against about 1.3 s in Python; Wicked (21.7 MB) takes
  2.5 s.
  - About half of planning is the cost pass, which runs at about encode
    speed.
  - Encoding with a plan takes about 24 ms for Dota 2 and 0.6 s for Wicked.
- `arixv_struct_probe pack --native` and the gate config `copy_warm_native`
  use it.

## 3. Shared base sets and block-parallel decode

### Design (job-system friendly)

The library owns no threads. A pack is split into independent AXST
**blocks**, each a complete stream. Some leading members form **base
sub-blocks**:

1. **Stage 1:** each base sub-block decodes independently. Decoding exports
   a `StructBaseSet`: the members' bytes, plus their words, frames and ID
   roles for those usable as copy sources.
2. The caller concatenates the exported sets in order. A set is a plain
   value; the research tool does this with `AppendSet`.
3. **Stage 2:** every other block decodes independently, given
   `const StructBaseSet*`. It can deduplicate against and copy from shared
   members.
   - The set is immutable and safe to read from concurrent decodes; TSan
     runs the concurrent test.
   - Mutable copy state (correspondence tables) stays private to each
     decode.

The job graph is N independent jobs, one join, then M independent jobs. Each
job is one plain function call. There are no callbacks, locks or pools in
the library, and the work never needs to be split mid-module.

- **Stream header.** It now records the shared-member count (version 2).
- **Decoder checks.** Decoding fails with `kBadContainer` without the
  matching set or with a set of a different size. A same-size wrong set
  fails cleanly or decodes to different bytes; it never reads out of
  bounds, and the sanitizer tests and fuzzing check this.
- **What a production format would add:** a checksum of the set, a block
  index, and per-block module counts and sizes for pre-allocating outputs.

For encoding, the encoder first encodes the base sub-blocks and then
decodes them with export. This guarantees its base view equals the
decoder's. Base selection (`--pick-shared 1`) takes the members that the
whole-set native plan copies the most instructions from. Taking the first
members in manifest order instead was much worse; for example, on Dota 2
with an 8-member base and 4-member blocks it gave 0.0863 against 0.0501.

### Results

Tool: `arixv_struct_parallel`. A pool of W persistent workers stands in for
an engine job system: the calling thread participates, tasks are picked
dynamically, largest first. Independent ARX2 decode runs per module on the
same pool. The machine is an idle Ryzen 9 5950X (16 cores, SMT on, workers
unpinned). Times are best of 7 (best of 3 for the new corpora) and include
stage 1. Ratio = (all streams + a compact block-length index) / original.
Labels: "base 8p" is an 8-member base in parallel single-member
sub-blocks, "base 8" is an 8-member base in one sub-block, "4/blk" is
4-member blocks. Full tables: `build/gate4/parallel/parallel.md`,
`build/gate4/parallel-new/parallel.md`, `build/gate4/parallel-frontier.md`.

Selected Pareto points at **16 workers** (ratio / ms):

| Pack | ARX2 | Solid AXST | Plain blocks | Shared base, fast | Shared base, small |
|---|---|---|---|---|---|
| Dota 2 (110 modules, 1.05 MiB) | 0.149 / 1.60 | 0.040 / 20.5 | 4/blk 0.111 / 2.08 | base 8p + 4/blk **0.056 / 2.34** | base 8 + 8/blk 0.048 / 5.25 |
| Dota 2 (#unique) | 0.149 / 1.55 | 0.046 / 20.3 | 2/blk 0.129 / 1.47 | base 8p + 4/blk **0.060 / 2.20** | base 8 + 8/blk 0.051 / 5.80 |
| 0 A.D. (339, 1.64 MiB) | 0.110 / 2.25 | 0.034 / 35.9 | 4/blk 0.091 / 3.15 | base 8p + 16/blk **0.054 / 3.76** | base 16 + 16/blk 0.051 / 6.88 |
| Wicked (398, 20.7 MiB) | 0.111 / 27.7 | 0.035 / 446 | 16/blk 0.062 / 38.4 | base 4p + 16/blk **0.047 / 45.0** | base 8 + 16/blk 0.043 / 82.7 |
| Xenia (92, 1.11 MiB) | 0.180 / 1.69 | 0.039 / 23.8 | 2/blk 0.122 / 2.84 | base 4p + 4/blk **0.063 / 5.02** | base 4 + 8/blk 0.044 / 9.40 |
| Sascha glsl (350, 0.84 MiB) | 0.149 / 1.52 | 0.073 / 20.1 | 4/blk 0.126 / 1.99 | base 16p + 16/blk **0.089 / 2.67** | base 16 + 16/blk 0.085 / 3.92 |
| mobile_nerf (12, 0.23 MiB) | 0.069 / 0.69 | 0.031 / 3.96 | 2/blk 0.058 / 1.55 | base 2p + 2/blk **0.039 / 2.13** | base 2p + 16/blk 0.033 / 3.56 |
| Blender (9 large modules) | 0.133 / 4.08 | 0.097 / 14.1 | 2/blk 0.116 / 7.16 | base 2 + 2/blk 0.104 / 11.5 | base 4 + 2/blk 0.097 / 12.0 |

Findings:

- **Solid streams do not parallelize.** At 16 workers they are 10–16×
  slower than ARX2. They are fine where packs are already decoded
  concurrently (many packs per load) or small, and wrong for one large pack
  on an otherwise idle job system.
- **A small parallel base recovers most of the solid gain at 16 workers.**
  - On the large application sets (Dota 2, 0 A.D., Wicked, Sascha) it
    costs 1.4–1.8× ARX2's latency.
  - On sets with few or large modules (Xenia, mobile_nerf) it costs about
    3×.
  - Against plain blocks of similar latency, it cuts bytes by about 25%
    (Wicked) to 50% (Dota 2, Xenia).
- **Blender is limited by module size.** Even ARX2 bottoms out at 4 ms, the
  largest module. A few large modules leave little parallelism for any
  scheme.
- Policies are not tuned. Block membership is consecutive in manifest order,
  and the base is chosen by a single heuristic. Clustering blocks by
  similarity, and using a multi-level base, are the obvious next levers.

## 4. New independent application corpora

A separate agent searched for real SPIR-V sets. Its full report is
[`pack-corpus-search.md`](pack-corpus-search.md). It covers sources,
revisions, licenses and dead ends. The sets used here are now pinned and
rebuilt byte-identically into `corpus/packs/` by
`scripts/build_pack_corpus.py` ([benchmark corpus](../benchmark-corpus.md)).
They were staged under `build/corpus-search/raw/` at the time. No repository was found that commits a large,
complete, application-derived `.spv` set. The best sources are:

- release artifacts: Wicked Engine (zstd frames carved from the Linux
  editor binary, MIT), and 0 A.D. 0.28 `mod.zip` (GPL-2+);
- engines that commit generated shaders: Xenia, ANGLE, Quake3e, Lumos.

These sets were not used to choose any policy, so they act as a held-out
check.

Solid AXST (copy+warm, native plan), byte-exact round trips. Ratio =
compressed / original:

| Set | Modules | Original MB | ARX2 | AXST | xz -9e (concat) | AXST vs ARX2 | AXST vs xz |
|---|---|---|---|---|---|---|---|
| Wicked Engine 0.72.139 (DXC, names) | 398 | 21.66 | 0.1113 | 0.0352 | 0.1191 | −68.4% | −70.5% |
| 0 A.D. 0.28 (glslc, stripped) | 339 | 1.72 | 0.1095 | 0.0344 | 0.1016 | −68.6% | −66.1% |
| Sascha Willems, glslang | 350 | 0.88 | 0.1491 | 0.0731 | 0.1187 | −51.0% | −38.4% |
| Sascha Willems, DXC | 308 | 0.77 | 0.1498 | 0.0748 | 0.1230 | −50.1% | −39.2% |
| Sascha Willems, Slang | 347 | 0.93 | 0.1460 | 0.0723 | 0.1113 | −50.4% | −35.0% |
| Xenia (glslang) | 92 | 1.17 | 0.1802 | 0.0385 | 0.0405 | −78.6% | −4.9% |
| ANGLE internal (stripped) | 149 | 0.45 | 0.1398 | 0.0449 | 0.0945 | −67.9% | −52.5% |
| Quake3e | 74 | 0.24 | 0.1430 | 0.0363 | 0.0870 | −74.6% | −58.3% |
| Lumos | 82 | 1.23 | 0.1225 | 0.0652 | 0.1431 | −46.8% | −54.4% |
| Skia SkSL goldens (test suite) | 352 | 0.91 | 0.1261 | 0.0649 | 0.1110 | −48.5% | −41.5% |

Caveats:

- The Wicked modules are carved by magic number. Every module round-trips,
  but not every one is validated SPIR-V.
- The xz column is solid xz of the concatenated modules: a strong generic
  reference, but not the per-pack best-generic minimum used in gate 3.
- Licenses differ (0 A.D. is GPL). Keep the data local, and publish only
  recipes and derived metrics.

## 5. Hardening

- **Research fuzz campaign.** `multi_module_probe.py fuzz-smoke --seconds
  1800 --jobs 12 --axst-plans build/gate3/results/packs` (log:
  `build/gate4/fuzz-long.log`). Both harnesses now also exercise native
  plans, no-adapt, and shared-set base/block splits (round-trip harness).
  - rt: 30 min × 12 jobs, about 185k executions, coverage 6,434 (gate 3:
    4,787).
  - dec: 30 min × 12 jobs, about 2.6M executions, coverage 5,246 (gate 3:
    4,923).
  - No crashes, timeouts or OOMs.
- **New unit tests** (`tests/research_pack_test.cpp`): native planner,
  shared sets, wrong and missing sets, concurrent shared-set decodes, and
  shared-set mutation fuzz. `StructFuzz` also mixes native plans and
  shared splits.
- **Sanitizers.**
  - ASan/UBSan: `research_pack_test --fuzz-iters 1500` passes.
  - TSan: `research_pack_test` passes, and so does `arixv_struct_parallel`
    with shared sets on 4 workers.
- **CTest.** Default preset 7/7, research 9/9; Python 30/30.
- **Not run.** Standard `fuzz-smoke`: the standard TU is unchanged in this
  phase.
- **Before any integration.** AFL++/cmplog-style overnight campaigns on a
  production decoder.

## 6. Pack API prototype (AXSB)

A draft public C API went through two rounds of independent review. It is
in [multi-module-pack-api.md](multi-module-pack-api.md), v3.

The research mirror (`src/research_axsb.inc`) implements it: stateless
two-stage calls over a prototype container. Measurements
(`arixv_pack_api_bench`):

- whole-header validation costs at most 7 µs per call;
- scratch is 3.25× the stage-1 bytes;
- per-job memory is about 3–5× the job's bytes plus 0.3–1.6 MB.

The bitstream is unchanged by two decoder changes that the prototype
needed:

- **Copy tables keyed to the decoding job,** not to the base, so they are
  bounded by job size. They cost about 2% more decode instructions.
- **Shared data read in place** through plain views.

All 470 packs were re-verified byte-identical after these changes.

v4 of the proposal adds two measured changes:
- **A compact header.** Solid packs are within 0.6% of raw AXST, against
  4–9% for the fixed-width prototype.
- **A clustered default policy.** At P=16:
  - 0 A.D. 0.057 → 0.038, Wicked 0.047 → 0.041, Dota 2 0.056 → 0.053,
    at equal modelled latency;
  - large sets decode at 1.3–1.6× ARX2's 16-worker latency.

v5 applies the v4 reviews to the prototype. The format gains stored blocks,
canonical varints and explicit limits, and v5 has the right check order and
memory bookkeeping. It also fixes a header-only-module bug. Large sets now
decode at 1.3–1.5× ARX2's 16-worker latency at 1.8–2.9× smaller sizes, and
0 A.D. and Wicked are within 2–12% of their solid ratio. Blender remains
the policy's weak spot.

The latency-aware default (policy 2) limits stage-1 modules to half the
latency floor and lets stage-2 blocks grow to the largest module.
- Blender goes from 9.2 to 6.3 ms at 16 workers, for 6% more bytes.
- Large sets are within about 5% of the previous ratio, at equal or better
  latency.
- Few-module packs whose hubs are large, like mobile_nerf, now trade most
  of their gain for ARX2-level latency. That remains a size/latency choice.

Scratch now stores frame start offsets: 2.25× the stage-1 bytes.

A `latency_budget_pct` encode option (default 150%) is the developer's
size/speed dial, and measured latency tracks it. On mobile_nerf, 200% gives
a parallel 0.038 at 1.5 ms, against solid's 0.031 at 4.2 ms and ARX2's
0.069. At 300% large sets approach their solid ratio at 2–3× ARX2's
latency.

## Format (2026-11-22)

The pack format is specified as **ARXP v1** in
[`docs/format-arxp.md`](../format-arxp.md).
- **Magic and version:** `"ARXP"`, version 1. The frozen ARX2 decoder
  reports a pack as `UNSUPPORTED_FORMAT`.
- **Codec flags:** the research-only signal bit is dropped from the wire;
  the flags byte is now bit 0 WARM and bit 1 FAST_DECODE.
- **Golden vectors:** eight committed streams in `tests/golden/pack/`. They
  reproduce byte for byte under gcc and under clang
  `-march=native -ffast-math`.

## Decisions (2026-11-22)

The latency profile was decided as **two-stage blocks with an optional
shared stage 1**, with solid packs as the one-block case. The API is
[multi-module-pack-api.md](multi-module-pack-api.md), whose §10 records
each decision. It went through three review rounds (v1, v2, v4), and v5
onwards apply them.

- **Interface:** stateless calls, no session object, stage1/stage2 entry
  points, caller-owned outputs and scratch. The engine's job system owns
  all threading.
- **Encoder controls:**
  - `parallelism`: 0 maps to 8 for the foreseeable future, and automatic
    selection is deferred;
  - `latency_budget_pct`, the size/speed dial; it is an encoder heuristic,
    not a decode-time guarantee;
  - `max_block_bytes`;
  - an optional `FAST_DECODE` (no-adapt) flag.
- **Scratch** stores frame start offsets.
- **The optional per-job workspace** is deferred to 1.x.

Items completed after the original "remaining items" list:
- **Policy tuning:** clustered stage 2, a latency-aware stage 1, and the
  budget dial.
- **Memory bound:** stated as `C + K1 × module_count + K2 × job bytes`,
  with measured peaks.

The hooked `CodeModule` is done (see §1). Still outstanding: RSS
measurement at each concurrency level (see the plan's gate list).

## Rerun

```bash
# builds as in gate 3, plus:
cmake --build build/research-release -j --target arixv_struct_probe arixv_struct_parallel
D=build/gate1-coordination
COMMON="--manifest $D/manifest.json --corpus-dir /path/to/corpus --source-lock /path/to/lock.json"
G3="$COMMON --dump build/gate2/results/structure_dump.bin --gate2-packs build/gate2/results/packs.csv"
python3 scripts/bench/multi_module_structure.py gate3 $G3 --probe build/research/arixv_struct_probe --out build/gate4/results
for i in 1 2; do taskset -c 12 python3 scripts/bench/multi_module_structure.py gate3 $G3 \
  --probe build/research-release/arixv_struct_probe --out build/gate4/timing$i --time 7 --jobs 1 \
  --configs dedup_warm copy_warm copy_warm_noadapt copy_warm_native; done
python3 scripts/bench/multi_module_structure.py decode-gap --results build/gate4/results \
  --timing build/gate4/timing1 build/gate4/timing2 --configs dedup_warm copy_warm copy_warm_noadapt copy_warm_native
python3 scripts/bench/multi_module_structure.py parallel --tool build/research-release/arixv_struct_parallel \
  --lists build/gate3/results/packs --packs coll:smolv/dota2 'coll:smolv/dota2#unique' coll:smolv/blender \
  coll:vks/mobile_nerf/glsl --out build/gate4/parallel
# new corpora: list files named sha256("new:<name>")[:16].list under build/gate4/newcorpus
# (today: python3 scripts/build_pack_corpus.py, then corpus/packs/<set>.list)
python3 scripts/bench/multi_module_structure.py parallel --tool build/research-release/arixv_struct_parallel \
  --lists build/gate4/newcorpus --packs new:0ad new:sascha_glsl new:xenia new:wicked --runs 3 --out build/gate4/parallel-new
```
