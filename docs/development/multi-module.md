# Multi-module / shader-pack compression research proposal

Initial proposal, 2026-11-21. Track B of the
[1.0 roadmap](v1-roadmap.md#track-b-multi-module--shader-pack-compression-parallel-research),
starting from `f0f2af8` on `research/multi-module`.
Updated after gate 1, 2026-11-21. This document records the research direction
and decision gates; measured results live in the linked experiment reports.
Block sizes and budgets below are starting hypotheses, not format commitments.
Full-set decode and compression ratio take priority; identifying suitable use
cases is a research output.

Completed experiment reports are kept separately from this proposal:
[gate 1a: frozen corpus and size baselines](multi-module-gate1a.md),
[gate 1b: validated sequence-codec prototype](multi-module-gate1b.md),
[gate 2: repeated-structure opportunity report](multi-module-gate2.md), and
[gate 3: validated structural prototype](multi-module-gate3.md), and
[gate 4 (in progress): decode gap, planner, parallel blocks, new corpora](multi-module-gate4.md).

## Gate 4 (2026-11-22): two-stage pack design chosen, research prototype validated

Gate 3 was accepted as worth pursuing, with this constraint: any
multi-worker design must keep the interface simple and leave threading to
the engine's job system. Results: [gate 4 report](multi-module-gate4.md).
Proposed API: [multi-module-pack-api.md](multi-module-pack-api.md), with
its decisions record in §10.

- **Real application sets.** On independent sets not used for any policy,
  solid packs are 47–79% smaller than ARX2 and well below xz -9e:
  Wicked Engine −68%, 0 A.D. −69%, Xenia −79%, Sascha Willems −50%.
- **Design:** two-stage blocks.
  - Optional stage-1 blocks publish shared copy sources into caller
    scratch; independent stage-2 blocks read them.
  - Stateless calls; the engine's job system schedules them.
  - A solid pack is the one-block case.
- **Measured at 16 workers, default budget:** large sets are 1.8–2.9×
  smaller than ARX2 at 1.3–1.6× its latency. A `latency_budget_pct` encode
  option trades toward solid while keeping parallel decode.
- **Shipped in library 1.1:** the `arixv_pack_*` API over ARXP v1
  ([spec](../format-arxp.md), [API record](multi-module-pack-api.md)),
  ported from the research prototype byte for byte.
  - Four review rounds, conformance and golden tests, sanitizer runs, and
    coverage-guided fuzzing (`fuzz-smoke` includes pack harnesses).
  - The single-module ARX2 format and API are unchanged.

## Gate 3 result (2026-11-21): validated, awaiting a size/speed decision

The gate-2 thresholds were agreed as **quick-reject gates only**. Acceptance
weighs size and decode speed together, so neither gate guarantees
acceptance. The AXST research prototype is validated on every frozen pack
(1,880 byte-exact round trips). It uses:

- copies of instruction runs from earlier members;
- ID correspondence restoration;
- train-through of copied instructions in the unchanged model;
- exact deduplication and gate-1b warm statistics;
- one continuous stream.

Results against standalone ARX2 and the strongest generic references:

- −24.1% versus ARX2 on duplicate-free representative collections, and
  −41.2% including duplicates.
- Dota 2 is 19% smaller than xz -9e.
- AXST beats the best generic method on every representative pack.
- Full-set single-worker decode is about 1.20× independent ARX2 (1.13× on
  stage pairs).
- Independent 64-member blocks cost Dota 2 24% of its whole-set size;
  smaller collections saturate by 8–16 members.

A format/API decision (gate 4) needs the user's judgment on this size/speed
point and on the remaining work in the report.

## Gate 2 result (2026-11-21)

These figures are encoder-side estimates with signaling and ID restoration
charged. Every parse reconstructs exactly; the entropy-coded sizes are not
validated yet.

- ID-abstracted instruction-run copies from prior members save −18.3% versus
  standalone ARX2 on duplicate-free representative collections. The warm
  start saved −5.0%.
- Dota 2 falls to 0.045, below solid xz at 0.050. Variant families save
  −26.6%.
- The estimate beats the strongest generic method on 152/153 representative
  collections.
- Stage pairs (−5.9%) and unrelated mixes (−9.1%) gain about what the warm
  start gives.
- ID restoration is the dominant cost. Search depth barely matters.
- The estimate assumes copied instructions train the model without being
  coded.

The proposed thresholds were agreed as quick-reject gates (see gate 3).

## Decision after gate 1: close statistics transfer, investigate repeated structure

Decision, 2026-11-21: stop expanding cross-module adaptive-statistics transfer.
Refocus Track B on whether repeated shader structure supports a worthwhile
lossless pack codec. Keep the validated warm-start prototype and its results as
controls. Ordinary within-module adaptive coding is unchanged. This closes a
research workstream; it does not establish that every possible transferred
probability family would fail.

The [gate-1b measurements](multi-module-gate1b.md#validated-corpus-results-gate-1a-manifest)
show roughly 4–6% total savings over independent ARX2 blobs in the representative
tiers, including framing. Duplicate-free same-collection packs save 4.96%.
Even an identical second module still costs 89.1% of the first under the tested
transfer policy. Those are real gains, but we judge them insufficient to justify
a new pack codec's state-transfer, format, and maintenance costs. Further
probability-family sweeps, counter tuning, and combinations of transferred
priors are no longer the next step.

The [gate-1a baselines](multi-module-gate1a.md) show a different opportunity:
Dota 2 compresses to a ratio of about 0.149 with standalone ARX2, 0.130 with
exact deduplication, and 0.050 with solid xz. Generic compressors can exploit
substantial redundancy in some sets that the warm-start prototype largely
misses. This supports investigating repeated declarations, helper bodies, and
instruction/operand sequences, including copies with different IDs. It does
not prove that a custom structural matcher can beat the strongest generic
method. The evidence is limited: most measured packs are small sample sets,
and application-derived Dota 2 and Blender each contribute only one pack.

### Next bounded question and decision

Can decoder-reproducible reuse of earlier modules' structure produce materially
larger net savings than statistics transfer, and a useful advantage over exact
deduplication and the strongest generic references on the same packs?

- Reuse the frozen manifest and orders. Prioritize Dota 2, provenance-supported
  variants, and available application-derived sets; retain duplicate-free and
  less-related controls. Report each source and tier separately.
- First separate exact-module deduplication, exact block/string reuse, and
  instruction/operand similarity with IDs abstracted. Exclude exact duplicates
  when assessing near-duplicate opportunity. Match coverage alone is not a
  compressed-size result.
- Use prior modules only and predeclare a small candidate-search budget. Label
  exhaustive best-base or future-aware results as oracles. Do not build sorting
  infrastructure before the existing manifest order shows an opportunity.
- Cost base selection, copy spans, ID correspondence or exact ID restoration,
  literal residuals, escapes, boundaries, order maps, and all other metadata.
  Charge base data once in the full set. Any lower bound or estimated cost
  remains diagnostic until exact sequence decoding validates it.
- Deliver a per-source/tier opportunity table, incremental-cost curves, and a
  recommended use case or a recommendation to stop. Compare against standalone
  ARX2, its zero-modeling wrapper, exact deduplication, the retained warm-start
  control, and the strongest available generic method on identical members.

Review that evidence and agree a worthwhile-gain/complexity target before
building a minimal round-trip structural matcher. No numeric threshold is
agreed yet. Close Track B if the structural opportunity is weak, disappears
under credible signaling/restoration costs, or a generic wrapper serves the
useful cases without a compelling reason for a custom codec. A narrow useful
case is acceptable, but cannot support a general application claim.

Production pack-format/API work, further statistics-transfer sweeps, and block
or parallel-performance engineering are deferred. Sections A and B below retain
historical context or conditional mechanisms; the active sequence is the
repeated-structure probe in C followed by the revised gates in §4.

## Objective and constraints

Determine whether cross-shader redundancy can materially improve on the already
strong single-module compression baseline. Assume the application decodes the
entire selected set. Applications needing fewer modules can decode the set and
discard unwanted outputs or use single-module mode. Per-module random access,
sparse loading, and checkpoints are outside the initial research scope and
must not constrain compression experiments.

Measure the strongest practical full-set compression first, then quantify any
ratio cost of bounded memory or parallel decoding. A sequential solid stream
is a valid candidate. Do not give up substantial compression gains for access
features the workload does not require. Start with small reversible prototypes
before choosing a container or public API. A pack is a collection of separate
SPIR-V modules, including permutations; it is not a module with multiple entry
points or a serialization of an opaque driver pipeline-cache blob.

Do not assume one target application in advance. The research should establish
which relationships among shaders produce savings and use that evidence to
recommend useful set boundaries and application use cases.

- Keep single-module `ARX2` bytes, golden vectors, and compatibility unchanged.
  Experimental pack streams use a distinct magic; `ARXB` / `ARX3` are candidates,
  not allocated formats. Track B does not block the 1.0 release.
- Preserve every input byte, original module identity, and lookup mapping.
  Stripping debug data, optimizing modules, or renumbering IDs without a
  reversible mapping cannot count as lossless compression savings.
- Retain the [knowledge policy](ratio.md#knowledge-policy): adaptive learning
  from the encoded pack and grammar-derived knowledge, with no corpus-trained
  shipped tables. Any pack-derived dictionary must be reconstructible from
  earlier encoded data or explicitly stored and charged to that pack.
- Keep experiments in isolated diagnostic targets and scripts. Leave
  `src/model.cpp` as one file and localize necessary hooks to avoid conflicts
  with productization. Do not extend the public single-module API yet.

### Parallel-track integration boundary

Phase 0c (`f0f2af8`) supplies the shared CMake, raw-buffer decode, hardening,
and golden-vector baseline. Track A's A1–A4 work is expected to stay in the
public API/wrappers, CLI, tests, documentation, and grammar generator; it no
longer requires changes to `src/model.cpp` or `src/tracker.cpp`.

A1 will replace the public C++ `arixv::` entry points with the C API. Build
Track B's research harnesses and prototypes against internal interfaces such as
[`src/model.h`](../../src/model.h), or dedicated new research files/tools,
rather than depending on the existing public C++ encode/decode signatures.
Keep any wrapper needed for the independent `ARX2` baseline narrow so it can
follow A1's API transition without changing the pack experiments. Coordinate
overlap in build targets, tests, docs, and generator changes when integrating.

Gate 1b added research-only sequence encode/decode in `src/model.cpp`, gated
by `ARIXV_RESEARCH_PACK`, with declarations in `src/research_pack.h`. An isolated
CMake target compiles the model with that macro; normal libraries and public
headers do not acquire it. Retain this localized hook as a validated control,
not an A1 API extension or a commitment to the eventual structural mechanism.
Encoder-only intermediate output remains diagnostic; any new compression claim
requires matching sequence decode and exact byte reconstruction.

## Starting point in this checkout

`EncodeModule()` and `DecodeModule()` in [`src/model.cpp`](../../src/model.cpp)
create a fresh `ModelState` for each call. That state owns probability models,
the semantic `Tracker`, operand predictors, and string history together. Reusing
the whole object is not a valid warm-start experiment: SPIR-V IDs, imported
instruction-set IDs, recency lists, and type relationships belong to one module.
`Tracker::Reset()` exists, but resetting the tracker alone leaves module-local
predictors and handles in `ModelState` / `InstGroup`.

The string path already has grammar-derived priors and a primer, adaptive byte
models, a match-history buffer, and a 64-entry recent-string pool. Measure the
incremental benefit of sharing these components rather than comparing against
a hypothetical string coder with no dictionary.

Existing tools provide useful components, but no pack evaluation:

- [`docs/benchmark-corpus.md`](../benchmark-corpus.md),
  [`benchmark-sources.lock.json`](../../deps/benchmark-sources.lock.json), and
  [`build_spv_corpus.py`](../../scripts/build_spv_corpus.py) pin sources and
  generate SHA-256-named, globally deduplicated modules plus provenance TSVs.
  Hash filename order is not application pack order. The corpus and dependency
  checkouts are absent from this new worktree. A generated corpus exists in
  the main checkout; the first probe may read an explicitly supplied corpus
  path after checking its provenance revisions and content hashes. Do not
  require a fresh download or copy as a prerequisite.
- [`benchmark_spv_compressors.py`](../../scripts/bench/benchmark_spv_compressors.py)
  compares individual files, including per-file zstd and smol-v + zstd. Its
  64-file process batches are measurement batches, not shared compression
  blocks; it does not implement the solid archive baselines below. Its smol-v
  runner needs a dependency checkout. Allow an explicitly supplied checkout
  path and verify it against the source lockfile, just as for corpus reuse.
- `arixv_bench` supplies field entropy with `ARIXV_ENABLE_STATS`;
  `arixv_decode_bench --raw-residual` isolates structural decode work, not a
  compression-ratio oracle. Diagnostic targets include `memory_breakdown`.
  Follow the allocator and warm-heap lessons in
  [performance.md](performance.md) and [perf-post-ratio.md](perf-post-ratio.md),
  but remeasure this revision rather than adopting their historical numbers.

## 1. Corpus and reproducible pack definitions

Define a small manifest-driven evaluation set for the first probes. Recover
source/project paths from `manifest.tsv` and `duplicates.tsv`, with revisions
from `sources.tsv`. Retain content hashes as identity checks, not grouping
features. For each pack record ordered logical module IDs, content hashes,
source revisions, stage, known variant family, compiler/version/options where
available, and how the grouping was obtained. Unknown metadata stays unknown.

Use three explicitly labeled tiers:

1. **Existing-corpus proxy packs:** group by source/project and narrower sample
   or directory where provenance supports it. Label test collections and
   synthetic cases separately; being in the same upstream repository does not
   establish a real permutation family.
2. **Real application packs:** seek additional independent projects with actual
   variant families and reproducible build manifests alongside the initial
   experiments, without blocking them. Record redistribution terms before
   committing artifacts; keep restricted binaries
   local and publish acquisition/build recipes and permitted metadata. Label
   application-derived dumps separately from confirmed complete shipping sets;
   do not generalize from one application to all shader packs.
3. **Controls and adversarial cases:** unrelated shuffled modules, singleton
   packs, tiny/large modules, exact duplicates, byte-swapped inputs, unknown
   opcodes, and malformed inputs handled by the existing lossless fallback.
   Separate robustness cases from representative ratio aggregates.

Evaluate both unique-content packs and logical packs retaining repeated shader
entries. Reconstruct aliases from duplicate provenance only within their source
pack; never invent cross-project membership. Report exact-file deduplication
savings separately so they cannot masquerade as modeling gains. Test both
debug-bearing and available stripped workloads; if creating stripped inputs,
apply the same preprocessing to every codec and report them as a separate set.

Use an exhaustive rule table, applied to canonical paths and duplicate aliases:

| Source/path | Initial collection and interpretation |
|---|---|
| smol-v `tests/spirv-dumps/dota2/` | Dota 2 application-derived collection; numeric filenames do not establish variant families |
| smol-v `tests/spirv-dumps/blender/` | Blender application-derived collection; do not assume one load set |
| smol-v `tests/spirv-dumps/shadertoy/` | Collection of shader examples; do not assume related content |
| smol-v `tests/spirv-dumps/{glslang,dxc}/` | Separate compiler/test collections, not application packs |
| spirv-database `vulkan/gl_cts/` | Separate CTS zink/angle and other documented sub-collections; test/proxy status |
| spirv-database `vulkan/naga/` | Naga remap/test collection; keep transformation provenance |
| spirv-database `vulkan/{dawn,tint,clspv,glslang}/` | Separate compiler/test collections; label OpenCL/kernel inputs separately from graphics stages |
| spirv-database `vulkan/SaschaWillemsVulkan/` | Sample/frontend collections; stage-pair relationships require sample evidence |
| Vulkan-Samples `shaders/<sample>/` | Sample plus source language/frontend; root-level support shaders are a separate control collection |
| SPIRV-Tools `test/fuzzers/` | Fuzz/test collection, not an application set |
| yari-v duplicate aliases | Attach to evidenced origin collections; not independent project observations |
| Any unmatched source/path | Unclassified; retain in inventory, exclude from representative aggregates until classified |

Verify debug/stripped status from actual modules. Do not combine GLSL/HLSL/Slang
alternatives into an assumed shipping set; cross-frontend groupings are controls.
Before any probe, publish group member counts, unique/logical bytes, duplicate
counts, stage coverage, and available prefix/target pairs for every rule,
including unclassified exclusions. Small samples cannot support a 32- or
64-member within-family sweep.

Define gate-1 relationship tiers using provenance, not inferred similarity:
same collection (including Dota 2); vertex/fragment or other stage pairs supported
by a sample's pipeline metadata; and filename-stem variants corroborated by
sample/build sources, such as `compute_buffer` / `_fp16` / `_fp16_fallback`.
Stage pairs are a candidate pipeline-bundle use case, distinct from variants.
Use a true same-family tier only where family metadata exists. Numeric IDs,
common generators, or shared directories alone do not establish one. Mark
unavailable tiers unavailable; do not require new family labels for gate 1.

Define a fold's project unit as the documented originating producer/application,
not the repository mirroring its binaries: all Vulkan-Samples samples share
one origin; Dota 2 and Blender are separate origins despite sharing a smol-v
mirror. Combine related compiler producers when their provenance establishes
common origin. Record `origin_id`, `collection_id`, evidence, and `fold_id` in
the manifest. Where origin is unknown, use a labeled collection holdout and
make no claim of independent-project validation.

Use leave-one-project-out evaluation when independent projects are too few for
one meaningful development/holdout split. Choose policies using only the other
projects in each fold; keep duplicate content and related variants in the same
fold, merging connected groups or excluding overlaps as necessary. Report each
project/family rather than treating directories as independent applications.
Freeze manifests, folds, ordering seeds, and parameter-selection rules before
tuning. Repeatedly inspecting folds while tuning makes them development data;
reserve newly acquired projects for final validation where possible. The
single-file hash split from ratio tuning would leak related shaders here.
With limited application-derived sets, collection holdouts cannot establish
general application suitability. Gate-4 recommendations remain provisional
until validated on additional independent application sources.

### Discovering useful groupings and use cases

Compare nested source-supported groupings: a permutation family, related
materials or passes, and a broader project shader library. Include unrelated
controls. Sweep set size in both module count and original bytes, and measure
where savings saturate or unrelated additions dilute them. Feature similarity
can guide encoder grouping, but inferred similarity alone does not establish
that an application would use those modules together.

Tie each measured mechanism to a candidate use case. For example, delta/shared
declaration gains concentrated within variant families would motivate bundles
of material or pipeline permutations; repeated helpers across a library could
favor broader project sets; debug-string-only reuse could suit developer
artifacts without helping stripped shipping shaders. Statistics-transfer gains
from gate 1 do not justify an application recommendation. These structural
use cases remain hypotheses to test. The
report should identify which sets benefit, the required set sizes, and whether
decoding all members together is plausible. Validate promising candidates with
real project metadata; load traces can refine those recommendations later.

Make the incremental-cost curve a central artifact: coded cost of module k
after k−1 predecessors, cumulative pack savings, and gain versus set size, per
relationship tier and debug/stripped category. Show framing and exact-dedup
contributions separately. For continuous streams, obtain marginal bytes by
encoding complete prefixes with the same policy, including final flush. Both
coders defer output: rANS buffers symbols and the range coder holds carry bytes.
A buffer's growth at a module boundary is not that module's complete coded
cost. Field entropy is complementary attribution, not a byte count or a
guaranteed monotonic curve.

## 2. Size-first baselines and later performance measurement

Start with a small size-measurement script, reusing existing runners where
appropriate. Give every method exactly the same input multiset, logical module
identities, and full-set outputs. Pin zstd/smol-v versions and flags; disable
debug stripping. Store commands, versions/settings, manifest hashes, and raw
per-pack bytes. No timing framework, production container, or new corpus is
required to answer the initial opportunity question.

Required baselines:

| Baseline | Purpose |
|---|---|
| Independent `ARX2` blobs + minimal set framing | Current codec; also report the sum of bare blobs so framing cannot inflate the apparent gain |
| Unchanged `ARX2` payloads + one experimental set header and shared minimal framing | Zero-modeling pack baseline; removes repeated standalone headers while retaining mode choices and all entropy flushes |
| Minimally framed original modules + solid zstd -19 | Primary generic comparison without tar's member headers/padding |
| Minimally framed smol-v modules + solid zstd -19 | Lossless structural transform followed by solid generic compression |
| Minimally framed per-file zstd -19 | Separates independent versus solid generic compression |
| Independent `ARX2` blobs + exact-file deduplication | Cheap alternative to explain duplicate-only gains |

Use the same logical count, keys, member sizes, and order-restoration semantics
in minimal framing for every method, recording the exact encoding and which
metadata is compressed. Method-specific framing still counts. Add the original
`tar + zstd -19` and `smol-v + tar + zstd -19` baselines as secondary rows;
canonicalize order, names, timestamps, owners, and padding policy. Their
compressed tar overhead must not be attributed to better ARIX-V modeling.

The zero-modeling wrapper keeps each original payload byte, per-member mode,
compressed/uncompressed sizes, and logical identity needed to reconstruct the
standalone blob and decode it exactly. Charge its one set header and all member
metadata; do not simply subtract standalone header bytes and omit replacement
framing. It requires no model hook. Header savings belong to this simple
wrapper; compare specialized pack gains against it as well as bare `ARX2`
totals, and attribute adaptive modeling using A's fresh-versus-warm controls.

Pin effective zstd window parameters as well as level and library version.
Compare the strongest initial candidates with larger-window zstd and
`--ultra -22`, and `xz -9e`, recording effective dictionary/window and memory
settings rather than assuming defaults from level alone. All methods receive
the same ordering. Chunked generic baselines join the comparison in gate 3 if
independent blocks are evaluated. `ARX2 + solid zstd` is a cheap sanity control,
not a mechanism expected to recover much from already entropy-coded payloads.
For shared-dictionary/independent-block designs in gate 3, add zstd with a
dictionary trained only from that pack, charging dictionary bytes, training,
and loading costs. Do not add corpus-trained shipped knowledge.

An optional ID-canonicalized input + solid zstd row can diagnose sensitivity to
ID noise, but is non-bit-exact unless restoration is encoded. Label it as such;
it is neither a lossless baseline nor a proven upper bound on another model.

Report unsupported inputs per tool and paired results on a frozen common subset; keep
full-corpus ARIX-V results separately. Do not silently drop failing members or
change a pack's membership between methods.

The primary size metric is `total serialized pack bytes / original SPIR-V
bytes`, including framing, keys, sizes, offsets/index if needed, order maps,
checksums if present, dictionary/base data, alignment, and coder flush overhead.
Include compressed tar overhead in archive totals. Use the same logical key
convention across methods and break out payload versus metadata. Lead with
per-origin/collection and per-relationship-tier results and medians, then give
absolute and byte-weighted totals so large collections cannot hide regressions.
Report paired target-cost differences and their spread over prefix samples.
Size-2 groups may allow only one non-self prefix; singleton groups allow none.
Show these counts rather than implying repeated independent observations.

### Gate 1a conditional probes without codec changes

For the available relationship tiers, measure zstd target compression with a
raw prefix used as dictionary, pinning its interpretation/settings and using
no trained external dictionary. Also measure fully finalized
`size(xz(prefix + target)) - size(xz(prefix))` with consistent member framing.
Apply the same matched-prefix selection as A, including toolchain controls.
These are conditional-cost diagnostics, not equivalent probability models or
guaranteed bounds on a specialized codec. Separate compressed target bytes
from external-prefix costs; report full encoded prefix/seed plus target totals
when claiming a deployable result. This stage precedes the model hook and may
show that generic compression already captures the available opportunity.

### Performance contract, applied after a promising size result

In gate 3, measure full-set decode wall time and original-output MiB/s, including
framing, state initialization, dictionary loading, and reconstruction. Count each
logical output once. Establish single-worker results first, then measure
parallel-capable candidates and independent `ARX2` at 2, 4, 8, and available
physical-core counts. Report block count and speedup saturation. Keep solid
streams in the comparison even if they cannot parallelize internally; quantify
the compression cost of introducing independent blocks. No random-request or
sparse-burst harness is required for this phase.

The current solid rANS encoder buffers symbols for the whole set and the
two-ended decoder requires the full payload span, including its tail, before
decoding. Measure symbol-buffer peak memory and input residency explicitly;
do not imply incremental delivery or streaming decode from the first bytes.

Run resident-input codec timings separately from end-to-end file I/O timings;
label cold process/allocator and repeated warm runs. Encoding is offline but
still report time, peak memory, and sorting/search cost. Decode reports peak
RSS, scratch and retained state per worker, allocations, and total memory at
each concurrency level. Interleave paired runs on an idle pinned host, record
SMT/allocator policy, and report run spread. Verify full byte equality outside
timing; use output consumption during timing to prevent optimized-away work.

## 3. Oracle experiments before architecture selection

Start with a reset-every-module control. Change one mechanism at a time, then
combine promising mechanisms to test interactions. Initial probes record coded
bytes, estimated field entropy, and overhead; log elapsed time and state size
for feasibility without requiring precise performance comparisons yet. Entropy
estimates, match coverage, and free-metadata oracles are diagnostics, not
demonstrated lossless pack sizes. Use matching sequence decode and exact full
byte comparison before promoting encoder-only results to validated compression.
`ARIXV_ENABLE_STATS` uses global totals: snapshot/reset at each module boundary
in a single-threaded diagnostic run; do not use it for concurrent attribution.

### A. Adaptive warm starts — completed, closed to expansion

The following protocol documents the gate-1 experiment. Its measurements remain
controls; broader transfer families and per-family tuning are not scheduled.
See the decision above and the [completed report](multi-module-gate1b.md).

Write a member-by-member state-lifetime inventory. Start each module with a
fresh `ModelState` and transfer an explicit allowlist of adaptive families,
reconstructing owned arena contexts by value. This is a correctness-first
research implementation, not the final allocation strategy. Never copy raw
arena pointers or blindly preserve handle numbers: map per-(instruction, slot)
probabilities by stable keys into the fresh destination. Tracker ID/type/import
maps, section/function/block state, type-ID-keyed constant caches, previous
IDs/list references, and operand validity state start fresh. Literal caches and
last-word-count predictors remain fresh unless tested as separately specified
transfer policies. String state is fresh in A. Both encoder and decoder must
execute the same deterministic boundary operation.

Build the initial research target with default frozen tuning. In that build,
`Prob` packs its warm-up count into four bits; alternate research tuning has
another layout and is outside the initial sweep. Treat adaptation age
separately from probability values; `SymbolCdf::d[0]` also stores a counter.
Compare carrying both,
carrying learned values with counters reset to zero, and a small predeclared
intermediate-age sweep. Define this for every selected family, including lazy
contexts; do not zero distribution values when resetting counters. Prior
[ID-slot experiments](ratio.md) show a mismatched shared prior can lose to a
fresh model, so a carry-everything result alone is inconclusive.

Use the following controls from the first sequence probe:

1. Standard independent `ARX2`, including its ordinary mode selection, plus a
   separately framed structural-mode control for model-eligible inputs.
2. Empty transfer with separate module streams: every structural payload must
   be bit-identical to the existing forced structural path. This identity
   requirement is exact; continuous streams cannot have per-module byte identity.
3. Fresh state per module inside one continuous coder stream, charging module
   boundaries/sizes. The difference from 2 measures stream/framing effects.
4. The same continuous stream with allowlisted state transfer, counters carried
   or restarted. The difference from 3 isolates modeling; also retain separate
   module streams as a diagnostic cross-check.

Gate 1b probed opcode/count/enum families. Its matched targets use no prefix,
an unrelated origin, an unrelated origin with matching SPIR-V generator word
and SPIR-V version, the same collection, and a provenance-supported stage-pair
or variant family. Match original prefix bytes and record symbol counts/stages.
Header matching is a cheap producer-style control, not proof of identical
compiler options or complete toolchain identity; refine it with build metadata
where available. If a matched unrelated prefix does not exist, label that
comparison unavailable rather than silently replacing it.

Use multiple fixed prefix samples where the tier has enough candidates to
avoid one favorable order. Conditional target costs answer which
relationships help; they are not deployable pack totals. Whole-set totals must
include each warm-up prefix or stored seed once, including any prefix used in
decoder initialization. Never present a free unrelated prefix as shipped gain.

Gate 1b includes self-duplicate probes. An ID-permuted duplicate diagnostic
may help C distinguish ID noise from absent structural reuse, but is not a
reason to resume statistics-transfer sweeps or a prerequisite for the first
coverage probe. If used, identify ID operands safely with a validated rewrite
tool, preserve references, and validate the transformation. Report synthetic
capability diagnostics outside representative aggregates.

### B. Shared strings — conditional structural reuse

Do not run the originally proposed string-probability transfer and history-size
sweeps. First measure exact string repetition as part of C's structural
inventory. Pursue an explicit string dictionary or match reference only if it
accounts for a meaningful remaining opportunity after existing per-module
string coding, separating stripped shaders from debug-heavy artifacts.

Charge dictionary contents, references, escapes, selection, and reconstruction.
A dictionary built with future modules is an oracle until its full cost is
included; never rely on an external uncharged corpus dictionary. If a later
round-trip prototype reuses the current string match implementation, retain its
associated history/table and primer-relative positions together, avoid priming
twice, and validate position lifetimes. Bound history in gate 3 only after size
results justify that work.

### C. Repeated structure — active next probe

Start with the bounded size-only question in the decision above. The mechanisms
below are conditional follow-ups, not a commitment to implement every variant.


Begin with manifest order. If it shows sufficient opportunity, compare seeded
random order, stage/compiler grouping, and cheap encoder-side similarity
features such as opcode histograms and declaration fingerprints. Preserve a reverse map to original module keys and charge it.
Apply the same order to generic baselines. An exhaustive best-order/base search
is a small-pack oracle; measure practical bounded search separately.

Measure exact-module and exact-byte-block repetition first, particularly
type/constant/decoration declarations and helper bodies. Add a token-level
similarity oracle: opcode-plus-literal sequence match coverage against prior
modules with IDs abstracted, using bounded workloads for expensive alignment.
Report coverage, not estimated lossless bytes without a coding-cost model.

If this shows opportunity warm-state models miss, prototype cross-module token
prediction: predict opcode/operands at a prior-module position and learn an ID
correspondence. It must use decoder-available evidence or encode match/base
selection, correspondence ambiguity, escapes, and mismatches. Charge those
choices and prove exact reconstruction; no claim of metadata-free mapping is
assumed. Compare this with a simple byte-delta/base reference rather than
replacing a cheap baseline with an unproven complex model.

Equivalent declarations with different IDs are not byte-identical blocks:
any canonicalization needs exact
ID/order restoration data, and unknown/malformed instructions need a safe
fallback. Attribute savings after reference tables, base material, patches, and
reconstruction costs. Start with pack-wide, bounded-depth references and charge
all base retention or reconstruction costs. When evaluating independent blocks,
restrict references to each block or explicitly account for shared immutable
base data and its effect on independence. Free-base or unbounded best-base
results remain oracles until their full costs are measured.

### D. Full-set compression versus independent blocks

Run promising combinations as whole-set solid streams with all required state
and metadata charged. These are practical candidates, not just oracles. Then
compare 1, 4, 8, 16, 32, and 64 modules per independent block, extending the
sweep if gains have not saturated. The roadmap's original 8–32 range is not a
requirement. Sweep an uncompressed-byte cap as well: module count alone does
not bound state memory. Report partial final blocks and small packs.

Measure the bytes sacrificed for parallelism or bounded state, and whether
shared pack-local dictionaries/base data recover those bytes after setup and
retention costs. Choose internal block boundaries for full-set compression,
decode time, and memory. Do not pay for per-module restart points or serialized
checkpoints to provide unrequested random access.

## 4. Gates, deliverables, and implementation order

1. **Gate 1 — completed:** [1a](multi-module-gate1a.md) froze the classified
   manifest, group counts, size baselines, and generic conditional-cost probes.
   [1b](multi-module-gate1b.md) added the isolated sequence hook, empty-transfer
   identity control, fresh/continuous controls, counter-policy comparisons,
   matched prefixes, and self-duplicate probes, with exact byte round trips.
   Preserve the raw CSV/JSON, tool skips, and negative results. Cross-module
   statistics-transfer research is closed; broader families are not gate 2.
2. **Gate 2 — bounded repeated-structure opportunity report (completed
   2026-11-21; [report](multi-module-gate2.md); reject gates agreed):** run C's inventory
   and prior-module token-match diagnostic on the frozen packs, separating exact
   deduplication and ID-abstracted similarity. Estimate signaling/restoration
   costs explicitly and label coverage, lower bounds, and encoder-only estimates
   as diagnostics. Compare bounded search with a labeled oracle only where
   needed. Publish the mechanism by source/tier opportunity table and
   incremental-cost curves, with framing and exact dedup separate. Identify
   viable full-set use cases, limitations, and whether generic methods already
   serve them. Review the evidence with the user and agree a worthwhile-gain/
   complexity target before implementing a minimal round-trip matcher. Stop if
   the opportunity is weak or plausible costs remove it; do not turn a failed
   probe into open-ended statistics, search, or container work.
3. **Gate 3 — conditional validated full-set prototype (completed
   2026-11-21; [report](multi-module-gate3.md); multi-worker scaling, long
   fuzz campaigns and new application sources followed in
   [gate 4](multi-module-gate4.md)):** implement the simplest
   surviving structural mechanism in an isolated research target. Require exact
   reconstruction before reporting realizable compression gains. Judge net
   bytes against the agreed target and strongest generic references on the same
   members, including duplicate-free cases. Only then run D and §2's performance
   contract for total size, full-set decode time, throughput, and memory. Bound
   history/reference state and measure any cost of independent blocks. Select
   policies without held-out-fold feedback; new application sources are needed
   to validate application-level recommendations.
4. **Gate 4 — use-case and design recommendation:** recommend a prototype
   format/API only if a useful structural result survives validation and its
   implementation cost is justified. Retaining independent ARX2, adopting a
   generic wrapper, or closing Track B are valid outcomes. Track B's outcome
   does not weaken the single-module codec or block the 1.0 release.

Compression improvement is the primary selection criterion. Report relative
reduction in already-compressed bytes and absolute bytes saved over bare
single-module `ARX2` totals, minimally framed sets, and the zero-modeling
shared-header wrapper. Header/flush savings, exact deduplication, measured
statistics transfer, and new structural reuse must be reported separately; an
inflated indexed baseline cannot justify a complex pack codec. A numeric
acceptance threshold has not been established.

First establish the realizable full-set savings, then judge whether they
justify complexity, decode time, and state memory for the use cases the results
suggest. Quantify how much compression is lost to each parallelism or memory
constraint rather than imposing those constraints before measuring opportunity.
Report per-project regressions, saturation points, and held-out results so a
few favorable packs cannot conceal weak general applicability. Modest gains
may justify a simple wrapper but not a complex new codec.

## 5. Requirements for a winning prototype

Defer format/API design. A winner needs distinct magic, exact module
identity/boundary restoration, explicit state lifetime, checked lengths and
offsets, bounded allocation/output/work, validated dictionary/copy spans, and
bounded acyclic references. Mutable state is private to independent workers;
any shared dictionary is immutable. Do not require a random-access index.

Before integration, require exact sequence round trips, unchanged `ARX2`
goldens, deterministic scalar/SIMD output, hostile metadata/truncation coverage,
and concurrent independent-block tests if applicable. Verify the intended 1.0
decoder rejects the new magic as unsupported. Follow `AGENTS.md`: CTest after
changes, coverage-guided `fuzz-smoke` for model/tracker work and before pushing,
plus appropriate release, sanitizer, decoder-only, and pack-fuzzer checks.
