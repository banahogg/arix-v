# Multi-module gate 1b: minimal sequence-codec prototype

Research only (Track B, gate 1b of the multi-module research plan). This
document covers a research sequence hook, a size probe, and a driver. It does
**not** define a production container, wire format, or API. Single-module
`ARX2`, the golden vectors, the public API, and ARX2 mode selection are
unchanged. The hook compiles only into opt-in research targets.

Status, 2026-11-21: the code, tests, and fuzzing are done, and the corpus
integration has run against gate 1a's frozen manifest (SHA-256
`0b9e61e58bd8e32fc68dd7d9cd398f8a1b650e6d9240f8cfab5cb33177c786a7`,
`baseline_identity_verified: true`). Every reported size is an exact,
decode-verified round trip; there were **0 verification failures**. See
[Validated corpus results](#validated-corpus-results-gate-1a-manifest).

## Research decision after gate 1

Decision, 2026-11-21: close cross-module adaptive-statistics transfer to further
expansion and retain this prototype as a measured control. The modest gains do
not justify a new pack codec's complexity. The next bounded probe concerns
repeated structure, with all ID restoration and signaling costs charged; generic
compression gains do not prove a custom matcher will win. See the
[updated decision and gates](multi-module.md#decision-after-gate-1-close-statistics-transfer-investigate-repeated-structure).
The observations below are unchanged; this decision supersedes recommendations
to expand ID-slot, literal, or string probability transfer.

## What was added

| File | Role |
|---|---|
| `src/model.cpp` (`#ifdef ARIXV_RESEARCH_PACK` block at the end, plus one gated `#include`) | State transfer, sequence encode/decode, test inspector |
| `src/research_pack.h` | Internal research declarations and header-only `AXRP` framing; `#error` unless `ARIXV_RESEARCH_PACK` is defined, and also if `ARIXV_RESEARCH_TUNING` is defined |
| `tools/bench/pack_probe.cpp` | `arixv_pack_probe` CLI with JSON output |
| `scripts/bench/multi_module_probe.py` | Stdlib-only driver: validates the manifest, then runs packs and cases; also runs the research fuzz smoke |
| `tests/research_pack_test.cpp` | CTest executable. Also builds two libFuzzer harnesses (`-DARIXV_RESEARCH_LIBFUZZER_RT/DEC`) |
| `CMakeLists.txt` | `ARIXV_BUILD_RESEARCH_PACK` option (default **OFF**) and research targets only |

`model.cpp` is still one file. Without `ARIXV_RESEARCH_PACK`, its translation
unit is token-identical to `f0f2af8`. `g++ -E -P` output was compared with
`cmp` for `ARIXV_ENCODER=0/1`, `ARIXV_ENABLE_STATS`, and
`ARIXV_ENABLE_RAW_RESIDUAL`. Standard targets therefore compile exactly the
same model code. Research targets use the frozen default tuning.

## State-lifetime inventory (gate 1b scope)

At each module boundary the encoder and decoder do the same thing:

1. Construct a fresh `ModelState`.
2. If families are selected, walk one shared keyed traversal
   (`research_transfer::Walk`).
3. Copy each allowlisted value from the previous module's final state into
   contexts owned by the destination arena.

A context the previous module never created stays uncreated. It then gets its
constructor or lazy prior on first use, exactly as in a standalone module.
No raw arena pointers, handles, or IDs cross the boundary.

Keys are grammar-derived table indices. Every module computes them the same
way: section × instruction class, core-instruction index, value-enum
ordinal × slot, operand-kind index, and instruction class bucket.

### Transferred (allowlist)

| Family | Member | Key | Contents copied |
|---|---|---|---|
| opcode | `known_opcode[]` | section × prev-known | `Prob` |
| opcode | `opcode_index_trees[]` (lazy) | section × prev class | `OpcodeTree` root/mid CDFs and created leaf groups |
| opcode | `opcode_contexts[]` (lazy) | previous core-instruction index | order-1 `OpcodeTree` and promotion `count` (treated as an age) |
| opcode | `unknown_opcode_tree` (lazy) | — | `BitTree<16>` CDFs |
| count | `word_count_consistent` | — | `Prob` |
| count | `class_count_models[]` (lazy) | instruction class | `same_as_last`; `UintModel` (length CDFs, short rows, lazy long rows) |
| count | `raw_known_count`, `raw_unknown_count` (lazy) | — | `UintModel` |
| count | `InstGroup::has_optional` | core-instruction index | only this `Prob`. The `InstContext` is created value-initialized, as on first use, so every other `InstGroup` field stays fresh |
| enum | `value_enums[]` (lazy) | value-enum ordinal × slot | `known` `Prob`, index `LazyLeafTree8` (root plus created leaves), escape `UintModel` |
| enum | `bit_enums[]` (lazy, heap) | operand kind | per-enumerant bit `Prob`s, `unknown`, residual `UintModel` |

### Always fresh in gate 1b

- **Tracker**: the whole `Tracker`, including the ID/type/import maps,
  section/function/block state, recency lists, and the previous opcode.
- **Per-instruction caches and handles**: `id_slots` and the
  `InstGroup::id_slot` handles, `last_id`, `last_id_list`, `last_id_state`,
  `last_literal*`, `last_word_count` (a predictor, not a probability), and
  `result_type_choice`.
- **ID operand coding**: `cur_*` list state, `last_id_operand_type`,
  `prev_result_id`, `result_*` models, `type_in_list`, `type_rank`,
  `type_escape`, `id_escape`.
- **Literals and caches**: `integer_values`, `enum_literals`,
  `int_const_cache` (keyed by type ID), the context-number and float models,
  `spec_constant_op`, `ext_inst_nums`, `ext_inst_unknown`.
- **Header and bound models**: version, generator, schema, bound,
  `var_sc_matches`, `swapped_flag`.
- **Opaque operand bytes**: `operand_bytes`.
- **All string state**: `StringModel` bytes/order-0/dict/match table/history,
  `string_match`, `string_rank`, `string_canonical`, and the recent-string
  pool.

Candidates for gate 2 include `ext_inst_nums`, `var_sc_matches`, literal
models, ID-slot hit/rank models (they need stable (instruction, slot) keys,
not handle numbers), and strings.

### Counter (age) policies

Values are always carried. Only the age changes. Ages are the packed `Prob`
count (high 4 bits; 0..3 in the frozen tuning, where 3 is steady),
`SymbolCdf::d[0]` (saturates at 30, or 126 for byte-rate trees), and the
opcode order-1 promotion count (promotion happens at 4).

| Policy | Config token | Age after transfer |
|---|---|---|
| carry | `carry` | unchanged |
| reset | `reset` | 0 (fast early adaptation again) |
| intermediate (one predeclared diagnostic point, not a sweep) | `mid` | `min(age, cap)` with caps Prob 1, CDF 6, order-1 count 2 |

`research::InspectTransfer` is a white-box test helper. It encodes one
module, transfers the final state, and checks the result along the same walk:

- value mismatches are 0;
- `age == min(src_age, cap)`;
- a destination context exists exactly when the source context does;
- non-selected families stay uncreated;
- an independent field-by-field check (`NonAllowlistedFresh`) finds every
  non-allowlisted member equal to a fresh model.

## Research stream (`AXRP`, version 1)

All sizes are compact uint32 values (`container.h`):

```
"AXRP" | version=1 | flags | module_count>=1 | body_words[k]...
       | (separate only) payload_size[k]... | payload bytes
flags: bit0 framing (0 separate, 1 continuous), bits1-3 families
       (1 opcode, 2 count, 4 enum), bits4-5 age (0 carry, 1 reset, 2 mid),
       bits6-7 must be 0; age must be carry when families == 0
```

Each module's original size is `20 + 4 * body_words`, so only model-eligible
modules are representable. `EncodeSequence` rejects a sequence containing any
non-eligible member (`kNotModelEligible` plus the member's index); it never
falls back. The probe reports such a pack as `skip` with the reason. It
always reports the ARX2 baselines, so membership never changes silently.

The decoder checks, in order:

- magic, version, and flags;
- canonical compact sizes;
- that the module count can be described by the remaining bytes, before
  allocating;
- the sum of declared sizes against `max_output`, before allocating output;
- separate framing: that the payload sizes tile the rest of the stream
  exactly.

Coder consumption is checked with `ConsumedExactly()`, per payload for
separate framing and once after the last module for continuous framing.
`CodeModule` itself only reports exhaustion, so it never enforces
whole-stream termination early. The 1.0 decoder rejects `AXRP` with
`kBadContainer` (checked on every probe result).

There is no checksum, index, or random-access table, and decode is
whole-sequence only, which is acceptable for this research question. The
format is a research framing, not a proposal.

## Controls (plan §3.A) and how they are checked

| # | Config | Meaning | Check |
|---|---|---|---|
| 1 | baseline `arx2_standard_bytes` | Independent `arixv::Encode` blobs with normal mode selection | Each blob decoded and byte-compared |
| 1 | baseline `arx2_forced_structural_bytes` / `structural_payload_bytes` | Forced structural ARX2 blobs / bare `EncodeModule` payloads | Decoded and compared |
| 2 | `sep:none:carry` | Separately framed, empty transfer | **Bit-identical** to `EncodeModule` for every member (`empty_transfer_identity`; also tested for all-policy single-module sequences) |
| 3 | `cont:none:carry` | Fresh model per module, one continuous coder stream | Decoded and compared |
| 4 | `cont:oce:{carry,reset,mid}` | Continuous, opcode+count+enum transfer | Decoded and compared |
| x | `sep:oce:{carry,reset,mid}` | Separately framed transfer cross-check | Decoded and compared; member 0 identical to `EncodeModule` |

Any other `<sep|cont>:<none|subset of oce>:<carry|reset|mid>` can be given
with `--configs` (for example, per-family ablations), but they are not part
of the predeclared gate-1b set.

Every reported stream is checked the same way: decode it, compare the parsed
policy, byte-compare all members, and confirm the ARX2 decoder rejects it.
If any check fails, the probe sets `verified_all: false` and exits 1. The
driver then marks the pack `error`.

**Marginal costs.** `--curve` encodes complete, finalized prefixes `1..k` for
each configuration and verifies every prefix. The incremental cost is
`S_k − S_{k−1}`. Both coders defer output: rANS buffers symbols and the range
coder holds carry bytes. In-flight buffer growth is therefore never used, nor
are entropy statistics.

For separate framing, `member_payload_bytes` gives each module's exact flushed
cost. The research header (`header_bytes`) is reported separately, and it is
included in every `stream_bytes` total.

**Conditional target costs** (`case`) report:

- `whole_stream_bytes` (prefix plus target), which is the deployable total;
- `prefix_stream_bytes` (the prefix alone, finalized);
- `conditional_target_bytes` = whole − prefix, a diagnostic that is never a
  deployable total;
- `target_alone_stream_bytes`, the no-prefix comparison;
- for separate framing, the target member's exact payload.

Prefix bytes are never hidden.

**Self-duplicate diagnostic** (`--self-dup`): `[m]` versus `[m, m]` for each
member and configuration. This is a synthetic capability diagnostic, kept
outside representative aggregates.

## Validated corpus results (gate 1a manifest)

### Interlock

`multi_module_probe.py validate` (and `run`) checked, before any comparative
run:

- the ready marker (contract version 1);
- the exact manifest SHA-256;
- schema version 1;
- all 1,340 module paths, SHA-256 values, and byte sizes;
- 470 packs and 25,954 prefix cases;
- the snapshot hashes of `manifest.tsv`, `duplicates.tsv`, `sources.tsv`,
  and `benchmark-sources.lock.json` (all four match).

Packs and prefix cases were consumed verbatim. 12,071 cases are unavailable
in the manifest and stay unavailable.

### Coverage

| Item | Count |
|---|---|
| Packs measured under all 8 configs, each with `--curve` and `--self-dup` | 467 / 470 |
| Packs skipped as `not_model_eligible` | 3 (see below) |
| Empty-transfer (control-2) identity | true for all 467 measured packs |
| Available prefix cases run | 13,744 / 13,883 |
| Available prefix cases skipped | 139 |

The skipped packs are `coll:vks/fragment_shader_barycentric/hlsl`,
`pair:vks/fragment_shader_barycentric/hlsl:0`, and
`control:cross-frontend:vks/fragment_shader_barycentric`: those HLSL members
are not model-eligible, and ARX2 itself uses byte mode 1 for them. The 139
skipped cases reference the same members.

The whole run took 98 s wall time with 4 jobs. Timing is informational only.

### Artifacts

These are generated, non-distributed outputs in `build/research/gate1b-corpus/`:

- `results.json`: raw probe JSON per pack/case, plus tool and probe SHA-256,
  git HEAD, and the manifest/ready/snapshot records;
- `packs.csv` and `cases.csv`;
- `summary.txt`;
- `analysis.md` and `analysis.json`, the full tables from
  `multi_module_probe.py analyze`.

### Whole-set totals

Deployable bytes, including all research headers. Each cell is the total and
the change versus summed standalone ARX2 blobs.

| Group (manifest tier) | Packs | ARX2 | Control 2 `sep:none` | Control 3 `cont:none` | `cont:oce:carry` | `cont:oce:reset` | `cont:oce:mid` | `sep:oce:carry` |
|---|---|---|---|---|---|---|---|---|
| rep same_collection | 153 | 499,453 | −0.22% | −0.72% | **−6.01%** | −4.64% | −5.71% | −5.47% |
| ↳ no exact duplicates | 144 | 306,462 | −0.19% | −0.75% | **−4.96%** | −4.16% | −4.87% | −4.37% |
| ↳ with exact duplicates | 9 | 192,991 | −0.27% | −0.66% | −7.67% | −5.39% | −7.05% | −7.21% |
| rep stage_pair | 201 | 151,141 | +0.01% | −0.83% | **−4.14%** | −3.83% | −4.09% | −3.29% |
| rep variant_family | 14 | 33,134 | −0.04% | −0.41% | **−4.53%** | −3.88% | −4.48% | −4.17% |
| diag control | 52 | 213,058 | −0.60% | −1.51% | −9.13% | −7.24% | −8.89% | −8.14% |
| diag same_collection | 47 | 508,388 | −0.43% | −1.09% | −10.55% | −7.52% | −9.72% | −9.83% |

Per-pack medians, change versus ARX2 for `cont:oce:carry`:

| Group | Median |
|---|---|
| rep same_collection | −5.84% |
| rep stage_pair | −4.36% |
| rep variant_family | −5.79% |

The modeling-only gain (control 3 → 4, `cont:oce:carry` versus `cont:none`)
has medians of −4.85%, −3.36%, and −5.39% for the same three groups.

**No measured multi-member pack got larger from transfer under continuous
framing**: 0 of 467 packs have `cont:oce:*` above `cont:none`.

### Age policy

Carry or intermediate is usually best, and reset is never best overall.

| Group | Packs where reset is uniquely best |
|---|---|
| rep same_collection | 8 / 153 |
| rep stage_pair | 23 / 201 |

### Comparison with gate 1a's generic baselines

Gate 1a's `pack_baselines.csv` (read only) covers the same packs and members:

| Group | Zero-modeling shared header | ARX2 + exact dedup | smol-v + solid zstd-22 w27 | smol-v + solid xz −9e | Original + solid xz −9e |
|---|---|---|---|---|---|
| rep same_collection | −0.15% | −4.46% | −9.16% | **−13.70%** | +6.37% |
| ↳ no exact duplicates | −0.12% | +0.49% | +17.73% | +12.51% | +46.01% |
| ↳ with exact duplicates | −0.18% | −12.31% | −51.86% | −55.31% | −56.57% |
| rep stage_pair | +0.03% | +0.90% | +39.00% | +40.45% | +70.71% |
| rep variant_family | −0.02% | +0.34% | −9.48% | **−15.80%** | +63.00% |

Packs where `cont:oce:carry` is smaller than the best generic method:

| Group | Packs |
|---|---|
| rep same_collection, no exact duplicates | 140 / 144 |
| rep same_collection, with exact duplicates | 6 / 9 |
| rep stage_pair | 201 / 201 |
| rep variant_family | 8 / 14 |

### Prefix cases

The median conditional target cost divided by the no-prefix target cost
measures how much a prefix helps one target. Continuous configs use
finalized whole minus finalized prefix; `sep:oce:carry` uses the target's
exact member payload. This is a diagnostic: deployable totals include the
prefix.

| Tier | Cases run | `cont:none` | `cont:oce:carry` | `cont:oce:reset` | `sep:oce:carry` |
|---|---|---|---|---|---|
| none | 1,048 | 1.000 | 1.000 | 1.000 | 1.000 |
| unrelated | 9,002 | 0.972 | 0.892 | 0.918 | 0.923 |
| unrelated_toolchain_matched | 234 | 0.997 | 0.973 | 0.978 | 0.976 |
| same_collection | 3,009 | 0.973 | **0.835** | 0.874 | 0.867 |
| stage_pair | 416 | 0.967 | 0.894 | 0.903 | 0.925 |
| variant_family | 35 | 0.981 | **0.865** | 0.886 | 0.882 |

Paired per-target differences in the `cont:oce:carry` ratio (negative means
the first tier is cheaper):

| Comparison | Targets | Median |
|---|---|---|
| same_collection − unrelated | 884 | −0.048 |
| variant_family − unrelated | 35 | −0.039 |
| stage_pair − unrelated | 409 | −0.005 (≈ none) |
| stage_pair − same_collection | 260 | +0.030 |
| toolchain-matched unrelated − unrelated | 26 | +0.006 |

The toolchain-matched cases have much larger prefixes (median 45.6 kB versus
21 kB), so that comparison is not byte-matched. These aggregate ratios
do not isolate toolchain similarity from content similarity. Gate 2 needs
paired controls with comparable prefix bytes and enough distinct origins
before making that causal claim.

### Incremental cost and self-duplicates

Incremental cost uses finalized prefix encodes in representative
same_collection packs, as sum of marginal bytes divided by sum of standalone
payloads:

| Config | Position 1 | Position 2 | Positions 3–4 | Positions 33+ |
|---|---|---|---|---|
| `cont:none` | 1.018 (header) | 0.999 | — | 1.000 |
| `cont:oce:carry` | — | 0.952 | 0.926 | 0.932 |

Savings appear from the second module and stay roughly flat with position;
there is no sign of growth with set size.

**Self-duplicate** (synthetic, 1,209 module ids): a second identical copy
costs this fraction of a single copy:

| Config | Second copy / single |
|---|---|
| `cont:none` | 0.986 |
| `cont:oce:carry` | 0.891 |
| `cont:oce:reset` | 0.910 |

### Findings (validated; gate-1b scope)

1. **Framing is small, modeling is real.** Shared headers, continuous
   streams, and one flush save 0.4–0.8% on representative tiers. Opcode,
   count, and enum warm starts add about 3.3–5.4% (per-pack medians) on top,
   with no pack regressing.
2. **Relationship tiers matter.** Same-collection and variant-family prefixes
   reduce target cost about 4–5 points more than unrelated prefixes. Stage
   pairs are no better than unrelated prefixes for these families. An
   unrelated prefix still helps (0.892), because the transferred priors are
   largely grammar and usage statistics shared across SPIR-V.
3. **This model cannot exploit strong similarity.** An exact duplicate is
   only 11% cheaper, and IDs, literals, types, and strings stay fresh. Where
   modules are near-identical, smol-v + solid xz wins by large margins:
   - packs with exact duplicates (about −55% versus −7.7%);
   - `mobile_nerf` variants (for example 6,244 versus 9,925 bytes).

   Gate 2 now investigates exact dedup and cross-module structural/token
   reuse (C), including the cost of differing IDs. Further ID-slot, literal,
   or string probability-transfer sweeps are closed. This is a research
   prioritization decision, not a measured rejection of every such family.
4. **ARX2 leads the generic references on the measured duplicate-free
   collections and stage pairs:** stage pairs are +40% with smol-v + xz, and
   same-collection packs without duplicates are +12.5%. Gate-1b
   `cont:oce:carry` is the best measured method on 140 of 144
   duplicate-free same-collection packs and on all 201 stage pairs.
5. **Use-case signal (provisional).** Gains are modest and consistent across
   the measured full-set packs. They are largest for collections with internal
   duplicates or near-identical variants, which is exactly where the gate-1b
   mechanism is weakest.

Limits of these findings: most representative collections are Vulkan-Samples
sample directories, and the application-derived Dota 2 and Blender collections
are single packs. Folds were not used to choose any policy. These results do
not establish general application suitability.

## Fixture diagnostics (tool validation, not representative)

These are explicit file lists, not packs:

- the five golden-derived modules (`tests/golden/04..08`):
  `build/research/fixture-results/golden.json`;
- the first 16 corpus files in SHA-256 filename order:
  `build/research/fixture-results/hash16.json`.

On the hash-order list, `cont:oce:carry` was −9.0% versus ARX2, and a
self-duplicate second copy cost 84.6%. These lists were used only to bring
the tools up before the manifest existed.

## Validation performed

All builds used at most 4 jobs; fuzzing used 4 processes. The parent research
worktree also rebuilt both default and research targets after integration and
passed their 7/7 and 8/8 CTests plus all 24 gate-1a Python tests. The model,
header, probe and C++ test sources are identical to the fuzz-validated child
sources. The parent broadened the analysis reference list to include every
available generic baseline; the per-pack win counts did not change. A diagnostic
rerun with the integrated binary reproduced the complete probe JSON for four
frozen packs (Dota 2, a stage pair, a variant family, and an unsupported-member
skip). The original full run remains the source of the reported corpus totals.

| Check | Result |
|---|---|
| `cmake --preset default && cmake --build --preset default -j4 && ctest --preset default` | 7/7 pass. Run with a worktree-local symlink `corpus/spv → /path/to/arix-v/corpus/spv` (read-only use; the symlink was removed afterwards). `check_generated` is absent because `grammar-deps/` is not present. |
| Research build `build/research` (`-DARIXV_BUILD_RESEARCH_PACK=ON`), `ctest --test-dir build/research` | 8/8 pass, including `research_pack_test` (300 deterministic fuzz iterations) |
| `research_pack_test --fuzz-iters 3000` (RelWithDebInfo) and `--fuzz-iters 2000` under ASan/UBSan (`build/research-asan`) | Pass |
| ASan/UBSan probe on 8 corpus modules with `--curve --self-dup` | `verified_all` true, identity true |
| Preprocessed standard `model.cpp` versus `f0f2af8` | Identical (4 macro configurations) |
| Standard fuzz-smoke (`FUZZ_SMOKE_JOBS=4 FUZZ_SMOKE_SECONDS=300 ARIXV_CORPUS=<main corpus/spv>`, `build/fuzz-smoke`) | Clean. rt: 82,134 execs, cov 2735; dec: 270,268 execs, cov 1925; 0 crash/timeout/OOM |
| Research fuzz smoke (`multi_module_probe.py fuzz-smoke --seconds 300 --jobs 4`, `build/fuzz-research`) | Clean. rt: 61,173 execs, cov 3086 (500 seeds: 400 modules + 100 ordered pairs); dec: 298,706 execs, cov 3103 (199 emitted `AXRP` seeds over 6 policies; one seed pair skipped as not model-eligible); 0 crash/timeout/OOM |

`research_pack_test` covers:

- single-module identity under every policy (continuous framing included);
- the control-2 identity over fixture orderings and duplicates;
- round trips for every canonical policy (2 framings × 7 family subsets ×
  3 ages, plus empty) over several sequences, including duplicates, swapped,
  header-only, golden, and synthetic modules;
- determinism;
- that transfer changes the stream and that carry and reset differ;
- `InspectTransfer` for every family subset and age;
- rejection of unsupported members (with the member index);
- header errors: magic, version, reserved flags, age 3, non-canonical age
  with no families, zero/oversized/undescribable counts, `0xFF` and
  non-canonical compact sizes, a body larger than 32 bits, `max_output`;
- truncation at every prefix length, trailing bytes, and payload-size
  tiling;
- a swapped size table and a flipped policy byte, which never silently
  reproduce the original modules;
- bounded mutated round trips and hostile decodes.

The libFuzzer harnesses:

- **`fuzz_research_rt_lf`** splits its input into modules at aligned SPIR-V
  magic words. It checks the control-2 identity, then round-trips under a
  selected policy and under `cont:oce:<age>`, checking determinism and that
  the ARX2 decoder rejects the stream.
- **`fuzz_research_dec_lf`** decodes arbitrary bytes with
  `max_output = 16 MiB`. Anything it accepts must re-encode under the parsed
  policy and decode back to itself.

The legacy fuzzers do not exercise the `AXRP` metadata or transfer path;
these do.

## Usage

Configure research targets in a separate build directory; the default build
is unchanged:

```bash
cmake -S . -B build/research -DCMAKE_BUILD_TYPE=RelWithDebInfo -DARIXV_BUILD_RESEARCH_PACK=ON
cmake --build build/research -j4
ctest --test-dir build/research

build/research/arixv_pack_probe info
build/research/arixv_pack_probe pack --id X [--configs default|CFG,...] [--curve] [--self-dup] -- A.spv B.spv ...
build/research/arixv_pack_probe case --id C --target T.spv [--prefix P1.spv ...]
build/research/arixv_pack_probe emit --config cont:oce:carry --out s.axrp -- A.spv B.spv
build/research/arixv_pack_probe decode s.axrp
```

Driver commands:

```bash
python3 scripts/bench/multi_module_probe.py validate --manifest M.json [--ready R.json] \
    [--corpus-dir CORPUS --source-lock LOCK]
python3 scripts/bench/multi_module_probe.py run --manifest M.json --probe build/research/arixv_pack_probe \
    --out build/research/gate1b-corpus [--curve] [--self-dup] [--jobs 4]
python3 scripts/bench/multi_module_probe.py fixtures --probe build/research/arixv_pack_probe \
    [--curve] [--self-dup] [--out FILE.json] A.spv B.spv ...
```

`validate` and `run` refuse to proceed (exit 3) unless all of these hold:

- `manifest.ready.json` exists;
- its `contract_version` is 1;
- its `manifest_sha256` equals the SHA-256 of the exact manifest bytes;
- `baseline_identity_verified` is a bool;
- `schema_version` is 1 and the snapshot holds four hex SHA-256 values;
- every module path is absolute and its size and SHA-256 match;
- packs, prefix cases, and tiers are well-formed;
- every unavailable case has a reason.

With `--corpus-dir` and `--source-lock`, the driver also re-hashes
`manifest.tsv`, `duplicates.tsv`, `sources.tsv`, and the source lockfile
against the snapshot.

Packs run in manifest order with members in frozen order. Unavailable cases
are recorded as unavailable and are not run. Summary groups follow the
manifest's `representative` flag and tier.

`run` writes `results.json` (tool versions, probe SHA-256, git HEAD and
dirty count, manifest SHA-256, ready marker, snapshot, options, raw
per-pack/per-case probe JSON), plus `packs.csv`, `cases.csv`, and
`summary.txt`.

Research fuzz smoke, with the corpus used read-only:

```bash
cmake --preset fuzz-smoke -B build/fuzz-research -DARIXV_BUILD_RESEARCH_PACK=ON
cmake --build build/fuzz-research -j4 --target fuzz_research_rt_lf fuzz_research_dec_lf arixv_pack_probe
python3 scripts/bench/multi_module_probe.py fuzz-smoke --build-dir build/fuzz-research \
    --corpus /path/to/arix-v/corpus/spv --seconds 300 --jobs 4
```

Crash inputs are written to `build/fuzz-research/fuzz-research/<rt|dec>/artifacts/`.

## Corpus integration (exact commands run)

```bash
cd /path/to/other-worktree
D=/path/to/research-worktree/build/gate1-coordination
cmake -S . -B build/research -DCMAKE_BUILD_TYPE=RelWithDebInfo -DARIXV_BUILD_RESEARCH_PACK=ON
cmake --build build/research -j4 --target arixv_pack_probe
python3 scripts/bench/multi_module_probe.py validate --manifest $D/manifest.json \
  --corpus-dir /path/to/arix-v/corpus \
  --source-lock /path/to/arix-v/deps/benchmark-sources.lock.json
python3 scripts/bench/multi_module_probe.py run --manifest $D/manifest.json \
  --corpus-dir /path/to/arix-v/corpus \
  --source-lock /path/to/arix-v/deps/benchmark-sources.lock.json \
  --probe build/research/arixv_pack_probe \
  --out build/research/gate1b-corpus --curve --self-dup --jobs 4
python3 scripts/bench/multi_module_probe.py analyze \
  --results build/research/gate1b-corpus/results.json --out build/research/gate1b-corpus \
  --manifest $D/manifest.json \
  --gate1a-baselines $D/gate1a-results/pack_baselines.csv \
  --gate1a-cases $D/gate1a-results/conditional_costs.csv
```

The main checkout's lockfile and this worktree's committed
`deps/benchmark-sources.lock.json` have the same SHA-256 (`7d2e0508…856e`).

`analyze` refuses a manifest whose SHA-256 differs from the one recorded in
`results.json`. `--curve` is O(n²) encodes per pack; the full curve run still
took only 98 s here.

## Limitations and deferred work

- Only opcode/count/enum families transfer. ID-slot, literal, type, ext-inst,
  and string transfer, ID permutation, string sharing, token/ID matching,
  ordering search, and independent-block sweeps are deferred to gates 2 and
  3.
- The intermediate-age policy is one fixed point (Prob 1, CDF 6, order-1
  count 2), not a tuned sweep.
- Transfer reconstructs contexts by value with fresh arena allocation each
  boundary. Its memory and time are not optimized or measured; precise
  timing is out of scope.
- Encoding keeps two models alive at a boundary. The rANS encoder buffers
  the whole continuous stream's symbols, and the decoder needs the full
  payload before it starts.
- Only model-eligible SPIR-V members are supported; packs with any other
  member are reported as `skip`. The `AXRP` framing has no checksum.
- Corpus results cover gate 1a's frozen tiers only. Most representative
  collections are Vulkan-Samples directories; Dota 2 and Blender are single
  application-derived packs; and only 35 variant-family cases are available.
  These results do not establish general application suitability.
- Exact-duplicate packs are included in the `rep same_collection` totals as
  frozen. They are split out above, but exact dedup was not combined with
  transfer. "ARX2 + dedup + transfer" is an untested combination.
- The self-duplicate aggregate deduplicates by logical module id, not by
  content hash (1,209 ids).
- The `fragment_shader_barycentric/hlsl` members are not model-eligible, so
  their 3 packs and 139 cases are reported as `skip`. They were not dropped
  silently, and their ARX2 baselines are in `results.json`.
