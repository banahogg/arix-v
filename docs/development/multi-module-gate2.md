# Multi-module gate 2: repeated-structure opportunity report

Research only (Track B, gate 2 of the [multi-module plan](multi-module.md)).
This is a size-only, encoder-side opportunity estimate. It is **not** a
validated compressed size, a container, or an API proposal. Single-module
`ARX2`, the golden vectors, and standard builds are unchanged.

Status, 2026-11-21: complete on gate 1a's frozen manifest (SHA-256
`0b9e61e5…786a7`, `baseline_identity_verified: true`; snapshot sources
re-hashed and matching). All 470 packs were parsed in manifest order, and
**every parse was reconstructed byte-exactly by an independent decoder
simulation**. That check validates the copy/ID-restoration parse. It does not
validate the entropy-coded sizes; those remain estimates until gate 3.

## Question

Can decoder-reproducible reuse of earlier modules' structure produce
materially larger net savings than statistics transfer, and a useful
advantage over exact deduplication and the strongest generic references on
the same packs?

**Answer (estimate): yes, for related collections.** Charging signaling and
ID restoration explicitly, ID-abstracted instruction-run copies are estimated
to save:

- −18.3% versus standalone ARX2 on duplicate-free representative
  collections. The gate-1b warm start saved −5.0% on the same packs.
- −65% on Dota 2.
- −27% on variant families.

The estimate beats the strongest generic method (smol-v or original + solid
xz/zstd) on 152 of 153 representative collections. Gains on stage pairs
(−5.9%) and unrelated mixes (−9.1%) are about the same as the warm start's.

## What was added

| File | Role |
|---|---|
| `src/model.cpp` | One gated hook in `CodeModule`'s instruction loop plus `research::EncodeModuleInstCosts`, compiled only with `ARIXV_RESEARCH_INST_COST` (which `#error`s unless `ARIXV_RESEARCH_PACK` and `ARIXV_ENABLE_STATS` are also defined). Standard and gate-1b research translation units are unchanged |
| `src/research_pack.h` | Declaration of `EncodeModuleInstCosts` (same gating) |
| `tools/bench/structure_dump.cpp` | `arixv_structure_dump`: per module, the standalone structural payload size and, per body instruction, the words, a grammar-derived ID-role mask, and the unchanged model's in-context entropy |
| `scripts/bench/multi_module_structure.py` | `run` parses every frozen pack and verifies reconstruction; `analyze` writes the tables below |
| `tests/multi_module_structure_test.py` | Parser and cost tests, reconstruction-failure detection, and a dump-tool mask/cost check on synthetic SPIR-V |
| `CMakeLists.txt` | `arixv_internal_research_cost` and `arixv_structure_dump`, only under `ARIXV_BUILD_RESEARCH_PACK` |

### Dump checks

- For all 1,078 structural modules, the dump's payload equals the gate-1a
  ARX2 blob minus its header.
- The summed per-instruction entropy is 99.74% of the actual payload bytes.
  The remaining 0.26% is coder and flush overhead, and it stays charged.
- Two modules are not model-eligible. One malformed
  `fragment_shader_barycentric/hlsl` module frames differently in the model
  than in the grammar walk. All three are kept whole-literal at their
  standalone ARX2 size.
- ID-role masks come from `OperandCursor`, the codec's own walk, including
  ext-inst sets and constant widths. An instruction is all-literal if:
  - its opcode is unknown;
  - its word count is grammar-inconsistent;
  - its walk is degraded; or
  - its walk leaves operand words unconsumed.

  An all-literal instruction can only be copied word-for-word. The mask is
  always evaluated on the base module, which the decoder already holds.

## Method

Each pack is processed in frozen manifest order. Only earlier members of the
same pack can serve as a base, and nothing is sorted. Stage pairs and other
packs that reuse members are parsed independently.

1. **Exact module dedup.** A module identical to an earlier member costs one
   adaptive "duplicate" flag plus `log2(#prior unique)` bits. Every member pays
   the flag.
2. **Greedy, cost-based parse.** Each remaining module is parsed into literal
   instructions and copies of instruction runs from prior unique members. A
   copy is taken only where its explicitly charged cost is below the ARX2
   in-context bits of the instructions it replaces.
   - Candidates come from a hash of 3-instruction n-grams, most recent first.
   - The predeclared budget is a chain depth of 8. Depth 256 is a
     search-sensitivity diagnostic. Neither is an exhaustive oracle.
   - The continuation of the previous copy ("rep") is also tried.
3. Two token modes:
   - **exact**: whole instruction words must match.
   - **ID-abs**: opcode, word count, and all non-ID words must match, and ID
     words are restored.

### Charged costs (ideal arithmetic-coding bits; adaptive state persists through the pack)

| Item | Model |
|---|---|
| Segment start | One adaptive flag per instruction position, with context "previous instruction copied" |
| Copy source | Adaptive rep flag when a rep candidate exists. Otherwise Elias-γ(module distance) + `log2(base instruction count)` |
| Copy length | Elias-γ(L) for a rep, Elias-γ(L − 2) otherwise |
| ID restoration, per ID word | A per-(target, base) correspondence map and a running delta. If the map has the base ID: adaptive hit flag. Otherwise, or on a miss: adaptive "base + delta" flag. Otherwise an explicit ID at a uniform `log2(bound)` |
| Literal instruction | The unchanged ARX2 model's in-context bits from the standalone encode |
| Per-member flush, ARX2 headers, MMF framing | As in `arx2_framed`, charged unchanged |

Estimated pack bytes = `arx2_framed` − duplicate blobs + duplicate references
− Σ (standalone model bits − estimated bits) / 8, rounded up.

**Central assumption, to be validated in gate 3:** copied instructions pass
through the ARX2 model and tracker updates ("train-through"), so literal
instructions keep their standalone in-context cost. A decoder can do this,
because it knows the copied words and can run the encoder's update path
without coding. It costs roughly encoder-speed work per copied instruction,
so this design should not be expected to speed up decode. Without
train-through, literal costs would rise by an unmeasured amount.

Two choices are probably pessimistic:

- explicit IDs cost a uniform `log2(bound)`, while ARX2's ID model is much
  cheaper;
- every member is flushed separately, and no warm-start statistics are
  combined.

## Results

The full generated tables are in `build/gate2/results/analysis.md`. The
selections below are byte-weighted ratios of serialized bytes to original
bytes; lower is better.

- `*` marks an encoder-side estimate.
- Every other column is a verified gate 1a/1b byte count on identical
  members.
- "Warm start" is gate 1b's `cont:oce:carry`.
- "Best generic" is the per-pack minimum over original and smol-v with solid
  zstd-19, zstd-22 w27, and xz -9e.

### By source

| Group | Packs | Original B | ARX2 | ARX2+dedup | Warm start | Exact copy* | ID-abs copy* | Best generic | ID-abs vs ARX2 | vs best generic | ID-abs < generic |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Dota 2 | 1 | 1,099,348 | 0.149 | 0.130 | 0.139 | 0.054 | **0.045** | 0.050 | −69.6% | −9.4% | 1/1 |
| Dota 2 (#unique) | 1 | 954,372 | 0.149 | 0.149 | 0.139 | 0.062 | **0.052** | 0.057 | −65.0% | −8.7% | 1/1 |
| Blender | 1 | 775,316 | 0.133 | 0.133 | 0.131 | 0.133 | **0.108** | 0.143 | −18.9% | −24.6% | 1/1 |
| Shadertoy | 1 | 1,170,540 | 0.091 | 0.091 | 0.089 | 0.091 | **0.087** | 0.113 | −4.5% | −22.6% | 1/1 |
| Naga remaps (#unique) | 1 | 190,588 | 0.138 | 0.139 | 0.108 | 0.119 | **0.080** | 0.094 | −42.2% | −15.1% | 1/1 |
| GL CTS (zink/angle) | 20 | 712,460 | 0.123 | 0.124 | 0.103 | 0.090 | **0.068** | 0.094 | −44.7% | −27.6% | 20/20 |
| Vulkan-Samples collections (rep) | 151 | 1,690,280 | 0.137 | 0.137 | 0.128 | 0.121 | **0.109** | 0.151 | −20.7% | −28.1% | 150/151 |
| Stage-pair packs | 201 | 1,011,700 | 0.149 | 0.151 | 0.143 | 0.146 | **0.141** | 0.206 | −5.9% | −31.8% | 201/201 |
| Variant-family packs | 14 | 398,984 | 0.083 | 0.083 | 0.079 | 0.072 | **0.061** | 0.070 | −26.6% | −12.4% | 12/14 |
| Control: unrelated mixes | 4 | 269,680 | 0.144 | 0.145 | 0.132 | 0.138 | **0.131** | 0.171 | −9.1% | −23.7% | 4/4 |
| Control: cross-frontend | 45 | 991,512 | 0.154 | 0.152 | 0.138 | 0.143 | **0.125** | 0.163 | −19.0% | −23.7% | 45/45 |
| Control: singletons | 2 | 256,260 | 0.075 | 0.075 | 0.075 | 0.075 | 0.075 | 0.081 | +0.0% | −7.1% | 2/2 |

One Vulkan-Samples pack, one stage pair, and one cross-frontend control
contain the non-eligible barycentric HLSL members. Gate 1b skipped them, so
they are excluded from these common-subset rows. They are still parsed in
`packs.csv`.

### By manifest tier (gate 1b grouping)

| Tier | Packs | ARX2 | Warm start | Exact copy* | ID-abs copy* | Best generic | ID-abs vs ARX2 | Per-pack median (IQR) vs ARX2 | Median vs warm start | ID-abs < generic |
|---|---|---|---|---|---|---|---|---|---|---|
| rep same_collection | 153 | 0.140 | 0.132 | 0.103 | **0.089** | 0.118 | −36.4% | −9.5% (−16.9…−4.2) | −4.4% | 152/153 |
| ↳ no exact duplicates | 144 | 0.134 | 0.128 | 0.126 | **0.110** | 0.150 | −18.3% | −8.9% (−15.5…−3.8) | −4.2% | 143/144 |
| ↳ with exact duplicates | 9 | 0.150 | 0.139 | 0.063 | **0.052** | 0.061 | −65.2% | — | — | 9/9 |
| rep stage_pair | 201 | 0.149 | 0.143 | 0.146 | **0.141** | 0.206 | −5.9% | −4.2% (−8.8…−0.9) | −0.8% | 201/201 |
| rep variant_family | 14 | 0.083 | 0.079 | 0.072 | **0.061** | 0.070 | −26.6% | −23.7% (−39.9…−15.9) | −17.5% | 12/14 |
| diag same_collection | 47 | 0.122 | 0.109 | 0.089 | **0.075** | 0.097 | −39.0% | — | — | 47/47 |

**No pack is estimated larger than framed ARX2 by more than 1 byte.** Against
the bare ARX2 sum, the worst case is +8 B, which is the framing. Where nothing
matches, the overhead is the per-instruction "no copy" flag, which adapts to
almost zero.

The generic method wins only on:

- `mobile_nerf` (1 collection and 2 of its variant packs). smol-v + xz is
  10–16% smaller there. These shaders hold large near-identical constant
  arrays whose instructions differ in a few literal words, and whole-instruction
  tokens cannot match them.
- The 3 barycentric packs with non-eligible members.

### Mechanism attribution (ID-abs, bytes)

| Group | ARX2 | Exact dedup | Exact-copy net | ID-abs increment | Gross covered | Debug | Annotation | Global decl | Function | Signaling | ID restoration | Deep − bounded |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Dota 2 | 163,831 | 21,638 | 82,987 | 9,666 | 113,863 | 553 | 14,025 | 26,594 | 72,690 | 4,622 | 16,585 | 2,873 |
| Blender | 103,217 | 0 | 146 | 19,367 | 59,308 | 0 | 2,223 | 2,852 | 54,233 | 1,906 | 37,888 | 970 |
| GL CTS | 87,365 | 0 | 23,855 | 15,627 | 55,319 | 10,197 | 8,232 | 19,204 | 17,686 | 5,097 | 10,727 | 241 |
| Vulkan-Samples (rep) | 233,655 | 2,596 | 25,576 | 21,172 | 66,052 | 21,528 | 5,236 | 14,377 | 24,910 | 6,673 | 12,525 | 139 |
| rep same_collection, no dups | 307,712 | 0 | 20,271 | 36,695 | 113,229 | 16,800 | 6,223 | 14,220 | 75,987 | 7,402 | 48,757 | 1,073 |
| rep stage_pair | 151,945 | 0 | 4,180 | 5,689 | 16,246 | 6,429 | 1,398 | 3,788 | 4,630 | 2,779 | 3,460 | 4 |
| Control: unrelated mixes | 38,820 | 0 | 1,782 | 1,906 | 7,261 | 1,608 | 836 | 1,946 | 2,872 | 1,469 | 2,102 | 21 |

"Gross covered" is the ARX2 model bytes of the copied instructions, split by
section. Net ID-abs saving = gross − signaling − ID restoration. Global decl
includes the preamble.

### Uncharged coverage (diagnostic, not a size)

This is the share of non-duplicate model bytes whose instruction token appears
anywhere in a prior member, compared with what the charged parse actually
covers.

| Group | Exact token seen | ID-abs token seen | Charged ID-abs covered |
|---|---|---|---|
| Dota 2 (#unique) | 70.1% | 97.3% | 80.7% |
| Blender | 0.7% | 79.5% | 57.5% |
| GL CTS | 39.1% | 81.2% | 65.3% |
| Vulkan-Samples (rep) | 15.3% | 50.5% | 29.5% |
| rep stage_pair | 5.7% | 26.8% | 11.1% |
| Control: unrelated mixes | 9.2% | 65.4% | 19.2% |

High token coverage in unrelated controls (65%) shrinks to 19% once runs and
costs are charged. This is why coverage alone is not a result.

### Incremental cost: estimated / standalone ARX2 bytes by member position (non-duplicates, ID-abs)

| Group | 1 | 2 | 3–4 | 5–8 | 9–16 | 17–32 | 33+ |
|---|---|---|---|---|---|---|---|
| Dota 2 (#unique) | 1.001 | 0.281 | 0.204 | 0.768 | 0.535 | 0.465 | 0.256 (n=64) |
| GL CTS | 1.001 | 0.737 | 0.586 | 0.447 | 0.459 | 0.388 | 0.478 |
| Vulkan-Samples (rep) | 1.002 | 0.811 | 0.751 | 0.650 | 0.444 | 0.440 | — |
| Variant families | 1.001 | 0.637 | 0.444 | — | — | — | — |
| Stage pairs | 1.002 | 0.896 | 0.722 (n=21) | — | — | — | — |
| Control: unrelated mixes | 1.002 | 0.994 | 0.958 | 0.945 | 0.829 | 0.865 | — |

These are per-member attributions from the estimate, not finalized-prefix
stream sizes. For comparison, gate 1b's warm start was flat at about 0.93
from position 2 onward. In related collections the structural cost keeps
falling with set size. In unrelated mixes it stays near 0.85–0.95.

## Findings

1. **Repeated structure is the opportunity that statistics transfer missed.**
   On duplicate-free representative collections, the estimated saving is
   −18.3% total and −8.9% per-pack median. The warm start gave −5.0% total.
   The estimate beats the warm start on 97 of 144 of these packs.
   Dota 2 drops from 0.149 to 0.045, below solid xz at 0.050. GL CTS, Naga
   and variant families save 27–45%.
2. **It beats the strongest generic reference** on 152/153 representative
   collections, 201/201 stage pairs and 12/14 variant packs. That includes
   duplicate-free Dota 2 (−8.7% versus smol-v/original + xz). The exception
   is `mobile_nerf`'s near-identical constant arrays (see 6).
3. **Most of the gain needs ID abstraction, apart from Dota 2.** Exact-ID runs
   already give Dota 2 most of its saving: 83 kB net of 114 kB gross. For
   Blender, Naga and Vulkan-Samples, ID-abs adds as much as or more than exact
   copies. Blender gets essentially nothing from exact copies (146 B) but
   19.4 kB from ID-abs copies.
4. **ID restoration is the dominant cost and the main lever.**
   - It takes 43% of the gross covered bytes in duplicate-free collections
     and 64% in Blender.
   - In duplicate-free collections, 26k of 130k ID words are explicit. Those
     use a deliberately crude uniform `log2(bound)` code.
   - Coding explicit IDs with ARX2's ID model is the obvious improvement;
     this is likely pessimistic.
   - Signaling (flags, sources, lengths) costs only 4–7% of the gross in
     related collections. In stage pairs and unrelated mixes it rises to
     17–20%.
5. **Search budget is not a constraint.** Depth 256 instead of 8 saves at
   most 2.9 kB (Dota 2). That is 1.8% of its ARX2 size and 6% of its
   estimate. Every other group changes by under 1.2% of ARX2. The bounded hash search in
   manifest order captures the opportunity, so ordering and sorting
   infrastructure is not needed.
6. **Limits of the mechanism:**
   - **Unrelated mixes and stage pairs gain about what the warm start gives**
     (−9.1% and −5.9%). Cross-origin reuse is mostly generic boilerplate. A
     pipeline partner offers little structure beyond its siblings, matching
     gate 1a's generic finding.
   - **Whole-instruction tokens cannot exploit partial literal similarity**
     (`mobile_nerf`). Per-word literal residuals inside copies would address
     this, but are not modeled.
   - **Debug names add modestly.** Debug instructions are about one third of
     the covered bytes in Vulkan-Samples and stage pairs, and under 1% in the
     stripped Dota 2, Blender and Naga sets. String reuse beyond this does not
     need a separate dictionary mechanism yet.

## Candidate use cases (provisional)

- **Application shader libraries with permutation families, decoded as a
  whole set.** Dota 2 is the clearest case, and its savings grow with set size
  (0.26 marginal at 33+ members). The evidence is one application dump, not a
  confirmed shipping set. Blender is heterogeneous, yet still −19%.
- **Variant families and test/CTS-like corpora** (−27% to −45%).
- **Not compelling:** stage pairs, small diverse sample directories, and
  unrelated sets. These save about 5–9%, close to the warm start, and do not
  justify a pack codec on their own.

## Limitations

- All structural sizes are estimates under the train-through assumption. Only
  the parse's exact reconstruction is validated. Gate 3 must produce real
  coded sizes with matching sequence decode.
- Train-through means a decoder runs model updates for copied instructions,
  so decode time will probably be close to independent ARX2, not faster. The
  structural mode may then be decode-slower than solid xz on decompression
  throughput. That is unmeasured.
- There are two application-derived origins, each a single pack. Most
  representative packs are Vulkan-Samples directories. Folds were not used to
  choose any parameter. G = 3 and depth 8 were predeclared and not tuned.
- Copies come only from earlier members, in manifest (lexicographic) order.
  There are no within-module copies, no per-word literal residuals inside
  copies, and no combination with the warm start or with a continuous stream.
  Each of these could add to the gain, but none is measured.
- Debug and stripped categories are attributed by section. No stripped copies
  of debug-bearing workloads were generated.

## Proposed gate-3 target (for agreement — not yet agreed)

These are suggested thresholds for building the minimal round-trip matcher.
They are not a decision.

- **Mechanism:**
  - instruction-run copies from prior members, with rep and hash-chain depth
    8;
  - ID correspondence through map, delta, and explicit, with explicit IDs
    coded by the ARX2 ID model where possible;
  - literals coded by the unchanged model, with train-through;
  - one continuous stream;
  - exact dedup.

  Nothing else: no sorting, no blocks, no string dictionary.
- **Proceed past gate 3 only if the validated real bytes reach:**
  - at least −12% versus standalone ARX2 on duplicate-free representative
    collections (estimate: −18.3%);
  - smaller than the best generic on Dota 2 (#unique) (estimate: −8.7%);
  - no representative pack worse than framed ARX2 by more than 0.5%.
- **Decode budget:** full-set decode time at most 2× independent ARX2 on the
  same members, single worker. This is measured only after the size gate
  passes.
- **Close Track B** if the validated bytes fall short. Also close it if
  train-through proves infeasible without losing most of the gain.

## Rerun

```bash
cmake -S . -B build/research -DCMAKE_BUILD_TYPE=RelWithDebInfo -DARIXV_BUILD_RESEARCH_PACK=ON
cmake --build build/research -j --target arixv_structure_dump arixv_pack_probe
D=build/gate1-coordination            # frozen manifest + ready marker + gate-1a results
python3 scripts/bench/multi_module_structure.py run --manifest $D/manifest.json \
  --corpus-dir /path/to/corpus --source-lock /path/to/benchmark-sources.lock.json \
  --structure-dump build/research/arixv_structure_dump \
  --gate1a-baselines $D/gate1a-results/pack_baselines.csv \
  --gate1a-per-module $D/gate1a-results/per_module.csv \
  --gate1b-packs build/research/gate1b-corpus/packs.csv \
  --out build/gate2/results
python3 scripts/bench/multi_module_structure.py analyze --results build/gate2/results
python3 -m unittest tests/multi_module_structure_test.py
```

The dump takes under 1 s and the parse of all 470 packs about 50 s on 24
processes. The parse is deterministic. Outputs (`results.json`, `packs.csv`,
`modules.csv`, `analysis.{md,json}`, `structure_dump.bin`) are generated and
not committed.

## Validation performed

| Check | Result |
|---|---|
| Reconstruction of every parse (3 modes × all non-duplicate members of 470 packs) | Exact; any mismatch aborts the run |
| Dump payload = gate 1a blob − header (1,078 structural modules) | 1,078/1,078 |
| `python3 -m unittest tests/multi_module_structure_test.py` | 5/5 (including the dump tool on synthetic SPIR-V) |
| `python3 -m unittest tests/multi_module_baselines_test.py` | Pass |
| `cmake --preset default && cmake --build --preset default && ctest --preset default` | 7/7 (with a temporary read-only `corpus/spv` symlink; `check_generated` absent without `grammar-deps/`) |
| `ctest --test-dir build/research` | 8/8 |

`fuzz-smoke` was not rerun. The model change is a research-only hook that
standard and fuzz builds do not compile. Run it before pushing, per
`AGENTS.md`.
