# Compression ratio backlog

Companion to `performance.md`. Goal: better compression without giving back
the decoder speed and memory work. Started 2026-11-18 on branch
`ratio/experiments`.

## Knowledge policy

ARIX-V ships no corpus-trained tables. All shipped knowledge must be
derivable by code from pinned Khronos sources:

- the machine-readable grammars (`spirv-headers`, already pinned), and
- **text in the SPIR-V specification that code can match and extract**
  (decided 2026-11-18): the spec's phrasing is standardized enough ("Result
  Type must be ...", "must be the same type as ...") that type relationships
  can plausibly be extracted mechanically. Anything that cannot be
  auto-extracted that way is out of bounds until revisited.

Hand-set scalar constants (`src/tuning.h`) remain allowed. Preference order
for new modeling: learn adaptively from the module itself first; use
extracted spec knowledge for priors or candidate structure when adaptivity
alone is too slow to learn (cold start on small modules). The spec is not yet
a pinned source: using it means adding SPIRV-Registry to
`deps/grammar-sources.lock.json` and an extraction step to the generator.

## Where the bits go (main @ 049e8ad)

Encoder entropy by field (`ARIXV_STATS=1`), full corpus: 1,000,222 bytes of
entropy in 1,013,012 container bytes.

| field | bytes | share |
|---|---|---|
| id_ref (ID operands) | 332,328 | 33.2% |
| opcode | 154,474 | 15.4% |
| result_id | 145,823 | 14.6% |
| string | 130,674 | 13.1% |
| result_type | 69,606 | 7.0% |
| word_count | 47,657 | 4.8% |
| literal_int + literal_cdn | 73,088 | 7.3% |
| value_enum | 33,441 | 3.3% |
| other (header, bit enums, ext-inst numbers, ...) | ~13,100 | 1.3% |

Container overhead (header, size varint, range flush, rANS state) is the
remaining ~12.8 KB, ~12 bytes per module.

## Evaluation rules

- Exact corpus encoded bytes (total ratio), plus mean and median ratio
  (small modules weigh more there; the median module is 2.4 KB).
- Heuristics designed while looking at the corpus risk overfitting: check
  each change on the train/holdout split `scripts/bench/tune_scalars.py`
  uses (hex names 0-7 train, 8-f holdout).
- Track stateless decode instructions (callgrind per pass) and batched
  page faults per decode so ratio wins don't quietly undo the speed work.
- Format changes are fine (ARX2 is unreleased on this branch's base) but
  must update the golden vectors.

## Experiments

Size estimates are guesses until measured.

### Tier 1 — largest targets

1. **Code the result type after the operands** (`result_type`, 7%). Code
   operand IDs first, then predict the result type as "the type of operand
   k" (or a component/pointee of it), learned adaptively per opcode; output
   word order is unchanged. Exact for arithmetic, comparison, and composite
   ops; `OpLoad` is the pointee of its pointer operand's type. Needs per-ID
   result types in the tracker. Instructions whose operand parsing depends
   on the result type (context-dependent literals) must keep it first.
2. **Type-filtered ID candidates** (`id_ref`, 33%). With per-ID types, rank
   an operand among recent IDs whose type matches what that (opcode, slot)
   takes — learned per slot. Shares the type tracking with 1.
3. **Kind-split recency lists** (`id_ref`). Softer alternative to 2: code
   the operand's ID kind (value, constant, variable/pointer, label,
   function, type) as a small symbol, then rank within that kind's list.
4. **More result-ID predictors** (`result_id`, 15%). Beyond "previous + 1",
   forward-reference rank, and delta: "highest ID seen + 1", "smallest
   pending forward reference"; code which predictor holds.
5. **Match model for strings** (`string`, 13%). Predict the next byte from
   the longest earlier match in string history, falling back to the order-1
   byte model; member names, paths, and debug names repeat heavily.

### Tier 2 — solid, medium effort

6. **Recency-list cap 32** (`id_ref`). 64 → 16 cost +2.5% size for speed in
   July; a 32-entry list is one more nibble symbol with rANS. Speed/ratio
   tradeoff → branch.
7. **Grammar/spec-derived priors for cold start** (all fields; mean and
   median ratio). Section legality for opcode tables, word-count bounds,
   enum ranges instead of uniform starts.
8. **Order-2 opcode contexts** (`opcode`, 15%), falling back to order-1
   below a count threshold.
9. **Literal modeling** (7%): float sign/exponent/mantissa split per type,
   a recency list of constant values, literal contexts keyed by the
   preceding enum value (Location/Binding/Offset deltas).
10. **Type-driven word counts** (`word_count`, 5%): composite/call/struct/
   access-chain lengths follow from types; learned once types are tracked.

### Tier 3 — smaller or generic

11. Two-rate or mixed probability estimates for binary flags.
12. Re-tune rate caps and warm-up schedules with `tune_scalars.py`
    (train/holdout).
13. Container overhead (~1.3%): e.g. omit the rANS state when a module's
    symbol stream is tiny.
14. Value-enum contexts conditioned on the target ID's kind / the opcode.
15. Is a cheaper opcode context nearly as good as order-1? (The
    `size/tradeoff-no-order1-opcode` branch puts order-1 at 1.07%.)

Suggested order: 1 + 2 (one type-tracking change serves both, ~40% of the
output), then 5, then 4 and 7; measure 6 along the way.

## Log

### 1. Result type coded after the operands — landed (2026-11-18)

**Oracle first** (throwaway encoder instrumentation): for each of the
191,777 result-type instructions, candidates were each ID operand's Result
Type, then that type's element type (pointee, component, element, return
type). 54% matched (47,046 at candidate 0, 56,777 at candidate 1, 506
beyond); the per-opcode choice is nearly deterministic (static entropy
~1.3 KB). Estimated cost 69.6 KB → ~43.1 KB.

**Implementation:** the tracker records each ID's Result Type and, for type
declarations, its first ID operand (the element type). For instructions
whose operand parsing does not depend on the Result Type (everything but
`kInstConstantNumber`), the Result Type is coded after the operands as a
per-instruction 8-symbol choice (7 candidates + none), learned adaptively —
no per-opcode rules; a miss falls back to the ranked recent-types coding.
The decoder writes a placeholder and patches it (byte-swap aware). The
decoder builds candidates only up to the decoded index, with operand types
taken from the lookup `CodeIdRef` already does.

| | corpus bytes | Δ | train total | holdout total | decode Ir | wall (quiet host) |
|---|---|---|---|---|---|---|
| main (049e8ad) | 1,013,012 | — | 0.16217 | 0.17205 | 290.0M | 86.7 MiB/s |
| **landed: choice symbol** | **993,748** | **−1.90%** | 0.15912 (−1.88%) | 0.16874 (−1.92%) | 299.6M (+3.3%) | 84.5 MiB/s (−2.4%) |
| variant: "same choice as last" flag first | 996,324 | −1.65% | | | 296.5M (+2.3%) | 83.9 MiB/s (−3.6%) |

`result_type` entropy 69.6 KB → 50.2 KB. Means improve −0.7% / −0.8%
(train / holdout), medians −0.8% / −1.4%. The flag variant ran fewer
instructions but was slower in wall time (an extra dependent step and a
data-dependent branch before the choice), so the symbol-only form wins on
both axes. Decoder cost history: +4.05% instructions with eager candidate
lists → +3.5% with lazy candidates → +3.3% with captured operand types.
Memory: heap per decode 154 → 159 KB (IdInfo +8 bytes per ID), ~2 KB more
touched. Tiny modules can lose a little to the choice symbol's learning
cost (the 19-instruction golden vector grew 86 → 88 bytes).

Checks: `make test` (golden vector regenerated), `make asan`, 3000-iteration
fuzz, valgrind memcheck with full leak checking (corpus decodes,
`arixv_test`, 80 mutated/truncated blobs), scalar/SIMD cross-check.

### 2. Per-type ID recency lists — landed (2026-11-18)

**Oracle first** (`id_ref` was 332.3 KB on main). Variants estimated:
A, an adaptive choice of a per-type list plus per-type lists, ~304.6 KB;
D1, a single list predicted from the type the same (instruction, slot)
referenced last time, ~308.7 KB; B, the global list filtered by predicted
type, worse than today. D1 was implemented because it needs no extra symbol
and no type prediction beyond "same as last time".

**Implementation:** the tracker keeps one recency list per Result Type,
created lazily on the type's first value (`IdInfo::values_list` on the type,
`IdInfo::type_list` on each value). Each instruction group remembers, per
operand slot, the list its last ID came from. `CodeIdRef` codes a flag "in
that list", then a 4-bit rank. On a miss it falls back to the global ranked
coding. A typed hit does not touch the global list (keeping it for IDs the
typed lists miss measured slightly better than touching both, and is
cheaper).

| | corpus bytes | Δ vs item 1 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 1 (8abe05b) | 993,748 | — | 0.15912 | 0.16874 | 299.6M | — |
| typed lists, touch global too | 973,096 | −2.08% | | | | |
| **landed: skip global touch** | **973,376** | **−2.05%** | 0.15536 (−2.4%) | 0.16569 (−1.8%) | 312.5M (+4.3%) | ×0.950 |

−3.9% vs main (1,013,012). The 16-entry cap still binds: a 31-entry global
list on top of this gives 965,653 bytes (another −0.77%), which is item 6
and needs SIMD rank search past 16 entries. Memory: malloc-level heap per
decode 159 → 172 KB (one list per used type), arena unchanged at ~86 KB. The
19-instruction golden vector stays at 88 bytes (content changed).

Checks: `make all`, `make test`, `make asan`, 3000-iteration fuzz, valgrind
memcheck with full leak checking (corpus decodes, `arixv_test`, 120
mutated/truncated/overwritten blobs), scalar/SIMD cross-check.

### 5. String model: grammar dictionary, match model, order-0 starts (2026-11-19)

**Oracle first.** Strings that miss the 64-entry recent-string list are
~177K bytes coding at ~5.9 bits/byte: per-module cold start dominates
(order-1 contexts start uniform in every module). An offline simulator of
today's byte coding (125.9 KB vs 130.7 KB measured) estimated: order-1
contexts starting from a running order-0 model −9.2 KB; whole-string
dictionary of grammar names −12.4 KB; order-2 match model −7.7 KB alone and
more combined; priming the match history with grammar names −4.5 KB more.
Exact 64K-entry match table vs hashed: 4096 entries −0.1 KB, 1024 −0.9 KB.

**Implementation** (all knowledge generated from the pinned grammar by
`scripts/gen_grammar_tables.py`):
- dictionary: every `extensions` name in the core grammar plus every
  extended-instruction-set name, sorted (203 names). After a recent-list
  miss, a flag per instruction class, then an 8-bit index.
- match model: predicts the byte that last followed the same two bytes;
  a flag per current match length (capped at 15); misses fall back to
  order-1 bytes. History = primer, then every string of the module. The
  primer is every core enumerant name (grammar order, first occurrence),
  then the dictionary — a rule with no selection: BuiltIn names alone
  measured 779 bytes better on the full corpus, but picking a subset by
  corpus score is the kind of choice the evaluation rules keep off the
  holdout. The 1024-entry primed table is generated constant data; a decode
  copies it into its own arena on first use (no process-wide state).
  `arixv_test` rebuilds it from the primer to check the hash agrees.
- order-1 string-byte contexts and leaves start as copies (counts included)
  of an order-0 model updated on every string byte.

| | corpus bytes | Δ | train total | holdout total | decode Ir |
|---|---|---|---|---|---|
| item 2 (9803c73) | 973,376 | — | 0.15540 | 0.16575 | 312.5M |
| primer: BuiltIn names + dictionary | 935,823 | −3.86% | 0.14927 | 0.15949 | |
| **primer: all enumerant names + dictionary** | **936,602** | **−3.78%** | 0.14941 (−3.9%) | 0.15960 (−3.7%) | 313.5M (+0.3%) |

`string` entropy 130.7 KB → ~93 KB. Mean ratio −6.9% / −7.0% (train /
holdout), median −6.0% / −5.2%: small modules gain most. The first build
measured +4.7% instructions, almost all from GCC's per-file inlining budget
(model.cpp grew, so `DecodeIO::EmitBodyWord` and `BitTree` chunk coding
were outlined). Both are now force-inlined; with the same pins HEAD measures
311.8M, so the string model's own cost is +0.5%.

**Size.** Library objects (-O2, text+data): 238.4 → 286.9 KB (+48.5 KB):
- generated data +35.7 KB: primer 24.1 KB (7.9 KB with BuiltIn names
  only), dictionary 5.8 KB of characters + 1.6 KB of pointers, match table
  4.1 KB;
- model.cpp code +12.7 KB: +11.7 KB string model (instantiated for the
  encoder, decoder and raw-residual coders), +1 KB the inline pins.

**Decode memory:** heap requested per decode 172.3 → 177.2 KB (+4.9 KB,
+2.8%; arena blocks are part of this figure): the 4 KB match table (nearly
every module has a string that misses), the larger string model state
(~0.85 KB), and the history (~0.3 KB), less ~0.3 KB of byte-tree leaves.

Checks: `make test` (generator in sync, table self-check, golden vector
regenerated, still 88 bytes), `make asan`, 3000-iteration fuzz, valgrind
memcheck with full leak checking (corpus decodes, `arixv_test`, 180
mutated/truncated/overwritten blobs), scalar/SIMD cross-check, all 1080
corpus files round-trip. Wall time not yet measured.

### 4. Result-ID prediction: next unseen ID and run cursors (2026-11-19)

**Oracle first** (encoder dump of every ID definition and use, replayed
offline; the simulator puts today's coding at 144.1 KB vs 145.8 KB
measured). 41% of result IDs (99K of 239K) missed both "previous + 1" and
the forward-reference list, and paid a zigzag delta of up to 18 bits. Two
patterns account for most of that:
- compilers skip IDs already used elsewhere (constants and types created
  while a body is built): "smallest never-seen ID above the previous
  result" catches 23% of the escapes; −4.0 KB;
- 86 modules (8%) have shuffled-looking ID spaces (most results jump by
  ≥256) and carry 43% of result-ID cost. Their IDs come in interleaved runs,
  so "one past a recent run end" (16 runs) catches many; −6.9 KB on them.
Rejected: coding the rank among unseen IDs instead of the delta (−1.4 KB,
needs an order-statistics structure over the ID space); a cursor plus delta
(the index costs more than it saves); direct `bitlen(bound)` coding for
shuffled modules (the adaptive length coding already matches it);
filtering seen IDs out of the run candidates (unstable positions, worse).

**Implementation:** prediction = smallest ID above the previous result
that the tracker has never seen, scanning at most `kResultScanCap` = 64 IDs
(a bound on decode work for hostile input; 64 measured the same as
unbounded, 16 worse). On a miss: a flag "continues a recent run" plus that
run's position in a 16-entry list of run ends (a `RecencyList`; continuing
a run replaces its entry at the front via the new `RecencyList::Replace`),
then the forward-reference rank as before, then the delta, now from the
prediction.

| | corpus bytes | Δ | train total | holdout total | decode Ir |
|---|---|---|---|---|---|
| item 5 (659f60f) | 936,602 | — | 0.14941 | 0.15960 | 313.5M |
| first version (run update = Remove + Touch) | 921,880 | −1.57% | | | 323.7M (+3.3%) |
| **landed (in-place `Replace`)** | **921,880** | **−1.57%** | 0.14685 (−1.7%) | 0.15732 (−1.4%) | 320.3M (+2.2%) |

`result_id` entropy 145.8 → 131.1 KB. Decode cost: the run-list update and
the scan, partly offset by fewer escape deltas (`CodeUint` −18M Ir per
3 passes). Memory: +124 B heap per decode. Library: +1.8 KB. The
19-instruction golden vector shrinks 88 → 85 bytes.

Checks: `make test`, `make asan`, 3000-iteration fuzz, valgrind memcheck
with full leak checking (corpus decodes, `arixv_test`, 180
mutated/truncated/overwritten blobs), scalar/SIMD cross-check, all 1080
corpus files round-trip.

Wall time (quiet host, load 0.2, `taskset -c 4`, `arixv_decode_bench
--iters=10` on all 1080 files, 12 interleaved rounds): median 77.59 →
77.47 MiB/s, a median paired difference of about −0.3%. This is within run
noise and far below the +2.2% Ir, because the scan and the run-list update
are cheap, well-predicted work next to the coder's dependent chains.

### 7. Grammar-derived cold-start priors (2026-11-19)

**Oracle first** (encoder cost split by context age: the CDF's or flag's
warm-up count when the decision is coded). Decisions in never-used contexts
cost 132.9 KB of 909 KB (14.6%), and contexts with 1–3 updates another
138.5 KB. The grammar-shaped part of the fresh cost: opcode 34.5 KB,
value enum 14.1, word count 11.2, header 4.1, bit enum 1.9, ext-inst
number 0.9. The rest (ID ranks, result types, literals) has no grammar
prior to offer.

**Implementation.** A prior is a SymbolCdf proportional to integer
weights, with every symbol at frequency ≥ 1, so nothing becomes uncodable.
The generator builds all of them, mirroring `SymbolCdfSetPrior` (now in
`symbol_coder.h`) in Python, into a deduplicated pool of 313 tables. A
decoder only copies a table when it creates a context: no runtime
construction and no process-wide state. A runtime-built prototype cost
+38% Ir. `tests/grammar_tables_test.cpp` rebuilds every table with the C++
function and compares.
- **Opcode index trees**, per module section: instructions the logical
  layout allows in the tracker's current section get weight 64
  (`kOpcodePriorLegalWeight`), all others weight 1. Rule
  (`opcode_legal_in_section` in the generator): before the function
  sections, instructions of this or a later section. In the function
  sections, body instructions, `OpFunction*`, `OpVariable` and `OpUndef`.
  Anywhere: `OpNop`, `OpLine`, `OpNoLine`, `OpExtInst*`. Only 25 opcodes in
  the corpus break the rule (mostly ARM graph instructions after the
  types). Weight 16 and weight 1024 were both worse by about 0.15%. Seeding
  the shadow-trained order-1 trees saved 86 bytes and was dropped.
- **Enum and extended-instruction indices**: uniform over the grammar's
  n entries instead of all 256 values.
- **Header**: major version is the grammar's and minor is uniform over
  0..the grammar's minor. The generator's tool ID (high 16 bits) is now a
  "registered" flag plus an index over the contiguous IDs registered in
  `spir-v.xml` (52), with an escape. The tool's own version keeps the two
  order-1 bytes.
- **Validity flags** start at P(invalid) = 1/32 (`kValidFlagPrior`):
  known opcode, known enumerant or ext-inst, word count consistent with
  the grammar, no unknown bit-enum bits, canonical version, schema 0, tight
  bound, canonical string. Sizes are flat from 1/32 to 1/2048 (±0.03%), and
  the weakest is kept.

| | corpus bytes | Δ vs item 4 | train total | holdout total | train mean | holdout mean |
|---|---|---|---|---|---|---|
| item 4 (bcd30e2) | 921,880 | — | 0.14685 | 0.15732 | 0.19711 | 0.19604 |
| + index priors | 916,574 | −0.58% | 0.14599 | 0.15642 | 0.19460 | 0.19365 |
| + opcode section priors | 906,180 | −1.70% | 0.14431 | 0.15467 | 0.18981 | 0.18910 |
| + header priors | 904,426 | −1.89% | 0.14403 | 0.15438 | 0.18880 | 0.18817 |
| **+ validity flags (landed)** | **895,010** | **−2.92%** | 0.14251 (−3.0%) | 0.15279 (−2.9%) | 0.18496 (−6.2%) | 0.18444 (−5.9%) |

Median ratio: train 0.19059 → 0.18112 (−5.0%), holdout 0.18902 → 0.17991
(−4.8%). As intended, small modules gain most. By field: opcode 154.5 →
141.1 KB, value enum 33.4 → 26.9, word count 47.7 → 43.5, header 4.9 → 3.1.
The golden vectors: the 19-instruction module 85 → 73 bytes, and the
header-only vector stays 14 bytes with new contents.

Costs: decode Ir 320.3M → 321.5M (+0.37%). Heap +133 B per decode.
Library 288.8 → 303.9 KB (+15.1 KB, mostly the prior pool). A first
version kept a prior pointer in every `OpcodeTree` (+2.1 KB heap); the
section prior is now passed to `CodeValue` instead.

Wall time (quiet host, `taskset -c 4`, `arixv_decode_bench --iters=10`
on all 1080 files, interleaved): first version −1.65% (11 rounds, every
round slower). Hardware counters showed L1 instruction-cache misses
1.3M → 2.5M per run, from leaf creation and prior copies inlined into
every tree-coding site. Moving leaf creation out of line
(`ARIXV_NOINLINE` `MakeGroup` / `MakeLeaf`) restores the miss count and
halves the extra instructions. Landed version: median 76.50 → 75.30 MiB/s,
median paired −1.26% (11 rounds, −0.24% to −2.59%). The remainder is
lower IPC, not instructions or cache misses. The profile shows
`OpcodeTree::CodeValue` at 9.0 → 10.1% of cycles and LTO inlining moving
between `Tracker::Ensure` and `OnInstructionEnd`, so part of it is likely
code layout.

Not done: Capability (300 enumerants) overflows the 8-bit enum index and
always escapes, but the corpus has only 1,750 `OpCapability` operands
(worth ≤ 0.2%).

Checks: `make test` (including the new prior-table tests), `make asan`,
3000-iteration fuzz, valgrind memcheck with full leak checking (corpus
decodes, `arixv_test`, 180 mutated/truncated/overwritten blobs),
scalar/SIMD cross-check on 108 files, all 1080 corpus files round-trip.

### 16. Per-(instruction, slot) ID hit and rank models (2026-11-20)

Proposed by a separate model's review of ID-operand coding. That review
reported 873,594 bytes from a prototype patch; this is a reimplementation,
not the patch. Item 2 already picks an operand's *candidate list* by its
exact (instruction, slot). The "in this list?" flags and "which rank?" trees
were still shared per operand position across instructions, by
`IdCategoryBucket` × `Slot8`. Now each (instruction, slot) gets its own,
created fresh on first use: `IdSlotModel` holds two flags and two 16-way
rank trees. The candidates, the escape path and the shared escape models
are unchanged. All of it is learned per module; no shipped knowledge.

| variant | corpus bytes |
|---|---|
| item 7 (311e2dc) | 895,010 |
| **private flags and both rank trees, from first use (landed)** | **873,594** |
| shared global-list rank tree | 878,348 |
| shared rank trees (private flags only) | 884,828 |
| global-list path fully shared | 883,194 |
| promoted after 4 uses, copied from the shared models | 878,351 |
| promoted after 4 uses, fresh | 876,175 |
| promoted after 16 uses, copied from the shared models | 881,500 |

Every part contributes. "Promote hot slots" (code with the shared models
first, then switch) loses ratio at every threshold. Starting fresh beats
starting from the shared statistics, so the shared models were not a
useful prior for a particular slot.

Train total 0.14251 → 0.13895 (−2.5%), holdout 0.15279 → 0.14929 (−2.3%).
Mean ratio −0.7% train and −0.7% holdout; median train 0.18112 → 0.18013,
holdout 0.17991 → 0.17865. `id_ref` entropy 312.0 → 290.8 KB.

**Memory.** Pointers to the slot models in each `InstGroup` (16 × 8 B per
instruction context) cost more than the models (~37 per decode, 80 B
each). They also pushed decodes into the next doubled arena block:
+17.0 KB heap per decode and batched faults 0.74 → 1.03. Landed layout:
16-bit handles (32 B per instruction) into a `ModelState` vector reserved
for 32 models. Heap 177.5 → 184.1 KB per decode (+6.6 KB); batched faults
per decode 0.74 → ~0.86 (`faults.sh`: (faults at 21 iterations − at 1) / 20
in 64-file processes; ±0.05 between runs).

Costs: decode Ir 321.5M → 323.5M (+0.61%). Library +1.3 KB. Wall (8 min
after boot, load ~1.5, `taskset -c 4`, 12 interleaved rounds): median
76.79 → 76.44 MiB/s, a median paired difference of −0.77%; noisy, one
round −4%. The 19-instruction golden vector stays 73 bytes with new
contents.

Checks: `make test`, `make asan`, 3000-iteration fuzz, valgrind memcheck
with full leak checking (corpus decodes, `arixv_test`, 180
mutated/truncated/overwritten blobs), scalar/SIMD cross-check on 108 files,
all 1080 corpus files round-trip.

Open follow-ups from the same review: a short per-slot history of recent
IDs (−1.6% more on top, but about 6% slower in its prototype: a speed/ratio
branch); copying a repeated `OpPhi` parent-label column (small ceiling,
~1K label operands); predicting an operand's offset from the latest
result ID (train-only so far, overlaps the history).

### 9. Literal modeling: float32 split, per-type int deltas, enum-keyed strides (2026-11-20)

**Train-only analysis** (`literal_int` 18.8 KB + `literal_cdn` 16.9 KB on
train; 73.1 KB full corpus, untouched since `main`). Two independent sources
of waste stood out:
- `literal_cdn` (`OpConstant` / `OpSpecConstant`): 32-bit floats accounted
  for 73% of `literal_cdn` bytes (25.0 bits/float) because raw `uint32_t`
  coding sees IEEE-754 exponent bits in bits 23–30 and high mantissa bits in
  bits 20–22 as huge 30-bit integers. 35.6% of train float32s have zero
  mantissa and 51.7% have ≤ 4 nonzero top mantissa bits. Integer constants
  were coded as raw `uint32_t` without delta prediction, losing small
  step sequences per integer type.
- `literal_int`: half the bytes came from `OpDecorate` / `OpMemberDecorate`
  (`Offset`, `Location`, `Binding`, `DescriptorSet`, `ArrayStride`,
  `MatrixStride`), which all collided in `InstGroup::last_literal[slot]`.
  Other instructions with enum-preceded literals (`OpExecutionMode`,
  `OpTypeImage`, `OpSource`) share the same grammar pattern.

**Implementation** (all learned per module; no shipped knowledge, no
hardcoded opcodes):
- **Float32 constants** (`actual_span == 1`, `TypeClass::kFloat`, width 32):
  zero flag (`hint == 0`, preserving `-0.0f` on the nonzero path), sign bit,
  8-bit exponent coded as a zigzag delta from 127 (`float_exp`), zero-mantissa
  flag, and the 23-bit mantissa bit-reversed (`ReverseBits23`) into a shared
  `UintModel` (`float_mant`) so leading mantissa bits code as small integers.
  The 1 + 8 + 23 split is a lossless bijection over all `uint32_t` (including
  NaNs, subnormals, `-0.0f`, and hostile inputs). `float16` is left on the
  generic path because non-canonical SPIR-V can carry nonzero upper 16 bits
  (and splitting it was worth only ~24 B on train).
- **Integer constants** (`actual_span == 1`, `TypeClass::kInt`): delta from
  the last value of the same `result_type` ID via a lazily allocated 8-entry
  `IntConstCache` and a shared `UintModel` (`context_int`).
- **Enum-preceded literal integers**: `CodeModule` tracks the most recent
  `OperandCategory::kValueEnum` word within each instruction. When present,
  `CodeLiteralInteger` looks up `(inst_index, slot, last_enum)` in a lazily
  allocated 32-entry direct-mapped `EnumLiteralCache` (`{key, last, stride}`),
  codes a per-slot stride-match flag (`value == prev + stride`), and on a miss
  falls back to `CodeDelta` with the existing shared `integer_values` model.
  Rejected on train: recency-16 list for constants, stride flag on all
  literals (worse than enum-preceded only), per-`(opcode, slot, enum)`
  `UintModel`s (cold-start regression on small modules), and 64-entry
  `EnumLiteralCache` (byte-identical to 32 entries on train).
- `context_number` is now a lazy `ArenaPtr<UintModel>` instead of an inline
  `ModelState` member. `CodeLiteralInteger` and `CodeContextNumber` are
  marked `ARIXV_NOINLINE` (like `CodeString`) to keep `CodeModule`'s hot loop
  compact.

| | corpus bytes | Δ vs item 16 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 16 (`7f24981`) | 873,594 | — | 0.13895 | 0.14929 | 323.4M | 76.58 MiB/s |
| **landed: item 9** | **859,850** | **−1.57%** | 0.13675 (−1.58%) | 0.14695 (−1.56%) | **323.3M (−0.05%)** | 76.52 MiB/s (−0.08%) |

Train and holdout match almost identically. Mean ratio: train 0.18365 →
0.18041 (−1.76%), holdout 0.18310 → 0.17980 (−1.80%). Median ratio: train
0.18013 → 0.17737 (−1.53%), holdout 0.17865 → 0.17493 (−2.08%). Field entropy:
`literal_int` 40.2 → 35.9 KB (−10.8%), `literal_cdn` 32.9 → 23.6 KB (−28.3%),
combined 73.1 → 59.4 KB (−18.7%). The 19-instruction golden vector shrinks
73 → 68 bytes.

**Speed, memory, size.** Without `ARIXV_NOINLINE` on `CodeLiteralInteger` and
`CodeContextNumber`, decode Ir rose to 327.9M (+1.37%) from `CodeModule`
inlining bloat; outlining both drops Ir to 323.3M (−0.05% vs baseline, driven
by fewer `CodeUint` calls on floats and stride-matched decorations). Wall
time (load ~0.5, `taskset -c 4`, `arixv_decode_bench --iters=10` on all 1080
files, 12 interleaved rounds): median 76.58 → 76.52 MiB/s, median paired
−0.08% (−1.76% to +0.42%). Memory (`memory_breakdown`): heap per decode
184.1 → 185.2 KB (+1.0 KB, +0.56%); bytes touched +484 B (`ModelState` inline
−176 B from lazy `context_number`, `UintLongRows` −176 B, plus lazy
`EnumLiteralCache` 532 B and literal `UintModel`s); batched minor page faults
0.87 → 0.90 (within ±0.05 run noise). Library objects (−O2, text+data):
305.2 → 310.3 KB (+5.0 KB, all in `model.o`).

Checks: `make all`, `make test` (golden vector regenerated, new literal
round-trip tests in `model_test`), `make asan`, 3000-iteration fuzz, valgrind
memcheck with full leak checking (40 corpus decodes, `arixv_test`, 180
mutated/truncated/overwritten blobs), scalar/SIMD cross-check on 108 files,
all 1080 corpus files round-trip (default and `--force-coded`).

### 8. Order-1 opcode fast-shadow training and section-prior seeding (2026-11-20)

**Train-only analysis** (`opcode` 141.1 KB full corpus). Two findings:
- With `kOpcodeOrder1MinCount = 64` and steady-rate `SymbolCdfTrain`, 67.6%
  of instructions (83% of opcode bytes) never promoted out of order-0, and
  promoted order-1 trees started from uniform CDFs instead of section priors.
- Adding order-2 opcode contexts (`o2 >= 8`) on top of fast-shadow order-1
  only saved another −0.20% on train while multiplying trees 2.4× (+30–40 KB
  per decode) and shadow updates 3×, so order-2 was dropped and only the
  order-1 improvement (8A) was implemented.

**Implementation** (all in `src/model.cpp`, zero extra fields in `OpcodeCtx`
or `OpcodeTree`):
- `kOpcodeOrder1MinCount` lowered `64 → 4` (train optimum over
  `{0, 1, 2, 3, 4, 6, 8, 16, 64}`).
- `OpcodeTree::Train` uses `SymbolCdfUpdate` instead of steady-rate
  `SymbolCdfTrain` so shadow training follows `CodeValue`'s warm-up schedule
  and continues its warm-up count seamlessly when promoted.
- `CodeOpcode` seeds each newly created `OpcodeCtx::tree` with
  `GetOpcodeSectionPrior(section)` via `SetPrior(prior)` and passes the
  current section's `prior` to `Train` and `CodeValue` for lazy leaf groups
  (measured slightly better on train than storing a creation-time section
  byte, and −1,262 B better than uniform order-1 starts).
- Once `order1.count >= kOpcodeOrder1MinCount`, `CodeOpcode` skips the
  order-0 `opcode_index_trees` lookup/allocation and count write.

| | corpus bytes | Δ vs item 9 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 9 (`5915201`) | 859,850 | — | 0.13675 | 0.14695 | 323.3M | 76.47 MiB/s |
| **landed: item 8A** | **852,694** | **−0.83%** | 0.13549 (−0.92%) | 0.14585 (−0.75%) | **320.1M (−0.98%)** | 75.97 MiB/s (−0.65%) |

Mean ratio: train 0.18041 → 0.17981 (−0.33%), holdout 0.17980 → 0.17920
(−0.33%). Median ratio: train 0.17737 → 0.17647 (−0.51%), holdout 0.17493 →
0.17421 (−0.41%). `opcode` entropy 141.1 → 134.0 KB (−5.0%). Both golden
vectors in `tests/arixv_test.cpp` remain byte-identical (all 19 instructions
in `GoldenSpirvModuleEncoding` have distinct opcodes, so none promote).

**Speed, memory, size.** Decode Ir drops 323.3M → 320.1M (−0.98%, from fewer
shadow `Train` calls and skipping order-0 lookups once promoted). Wall time
(load ~0.4, `taskset -c 4`, `arixv_decode_bench --iters=10` on all 1080
files, 12 interleaved rounds): median 76.47 → 75.97 MiB/s, median paired
−0.65% (−2.92% to +2.31%) — promoting earlier spreads opcode lookups across
more per-opcode trees in the arena, trading a little cache locality for the
fewer instructions. Memory (`memory_breakdown`): heap/decode 185.2 → 184.3 KB
(−0.9 KB, fewer order-0 trees and leaf groups); batched minor page faults
0.90 → 0.87 (−0.03). Library (−O2, text+data): 310.3 → 310.7 KB (+0.4 KB).

Checks: `make all`, `make test` (new `Order1OpcodePromotionModules` in
`model_test`), `make asan`, 3000-iteration fuzz, valgrind memcheck with full
leak checking (40 corpus decodes, `arixv_test`, 180 mutated/truncated/
overwritten blobs), scalar/SIMD cross-check on 108 files, all 1080 corpus
files round-trip (default and `--force-coded`).


