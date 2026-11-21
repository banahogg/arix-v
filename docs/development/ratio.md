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

### 3. Kind recency lists for types, ext-inst sets, variables, and functions (2026-11-20)

**Train-only analysis** (`id_ref` 290.8 KB full corpus). Replacing Item 2's
per-type lists with kind-split lists regressed ratio, but *complementing*
Item 2 through the existing `IdInfo::type_list` / `Tracker::TypedList()`
dispatch — with zero extra flags and zero edits to `src/model.cpp` — fixed
two remaining gaps:
- Untyped IDs (`kType`, `kExtInstSet`) had `type_list == 0`, so slots
  referencing types (`OpTypePointer`, `OpTypeVector`, `OpTypeStruct`,
  `OpTypeArray`, `OpTypeFunction`) or extended instruction sets (`OpExtInst`)
  always missed the typed list and fell back to `recent_ids_`.
- Variables (`OpVariable`) and functions (`OpFunction`) were split across
  per-pointer-type and per-return-type lists, missing whenever a slot
  switched between variables of different pointer types or functions of
  different return types.

Train ablations (534 files, on top of item 9 `417,525 B`):
- `kType` aliasing `recent_types_` (−0.15%) beat a separate `kType` list
  (−0.12%) while allocating 0 bytes and avoiding duplicate `Touch()` calls,
  because `OnResultTypeUse` keeps active types warm.
- `kExtInstSet` alone −0.21%; unified `kVariable` alone −0.47%; unified
  `kFunction` alone −0.07%.
- `kLabel` hurt (+0.005% alone, +14 B combined, +3,300 flags) because
  branch targets are mostly forward refs and `OpPhi` alternates values and
  labels in the same slot; left at `type_list = 0`.

**Implementation** (entirely in `src/tracker.h` and `src/tracker.cpp`):
- `kType` assigns sentinel `Tracker::kRecentTypesList` (`~uint32_t{0}`),
  resolved by `TypedList()` to `&recent_types_`.
- `kVariable`, `kFunction`, and `kExtInstSet` use lazily allocated 1-based
  handles in `typed_lists_` via `Tracker::KindListFor()` (cleared in
  `Tracker::Reset()`).
- `OnInstructionEnd` unconditionally assigns `info.type_list = list` so
  hostile ID redefinitions cannot leave stale handles.

| | corpus bytes | Δ vs item 8A | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 8A (`fdcf2c6`) | 852,694 | — | 0.13549 | 0.14585 | 320.1M | 75.76 MiB/s |
| **landed: item 3** | **844,347** | **−0.98%** | 0.13429 (−0.89%) | 0.14430 (−1.06%) | **320.2M (+0.02%)** | **76.93 MiB/s (+1.62%)** |

Mean ratio: train 0.17981 → 0.17861 (−0.67%), holdout 0.17920 → 0.17793
(−0.71%). Median ratio: train 0.17647 → 0.17472 (−0.99%), holdout 0.17421 →
0.17285 (−0.78%). `id_ref` entropy 290.8 → 282.4 KB (−2.9%), adaptive
`id_ref` decisions 1,883K → 1,830K (−53.7K). Both golden vectors in
`tests/arixv_test.cpp` remain byte-identical (68 B and 14 B).

**Speed, memory, size.** Decode Ir is flat at 320.1M → 320.2M (+0.02%).
Wall time (load ~0.5, `taskset -c 4`, `arixv_decode_bench --iters=10` on all
1080 files, 12 interleaved rounds): median 75.76 → 76.93 MiB/s, median paired
**+1.62%** (−0.50% to +3.42%, 11/12 rounds faster) — higher typed-list hit
rates skip global `recent_ids_.Touch()` calls and drop ~54K adaptive coder
decisions. Memory (`memory_breakdown`): heap/decode 184.3 → 183.6 KB (−0.7 KB,
fewer per-pointer-type and `OpTypeVoid` lists); batched minor page faults
0.87 → 0.85 (−0.02). Library (−O2, text+data): 310.7 → 311.3 KB (+0.6 KB).

Checks: `make all`, `make test` (new `KindRecencyListTests` in `model_test`),
`make asan`, 3000-iteration fuzz, valgrind memcheck with full leak checking
(`arixv_test`, `model_test`, plus 40 corpus decodes and 180 mutated/truncated/
overwritten blobs on branch), scalar/SIMD cross-check on 108 files, all 1080
corpus files round-trip (default and `--force-coded`).

### 17. Cold-start priors for `UintModel` length and `StringModel` order-0 (2026-11-21)

**Train-only analysis** (`[0-7]*.spv`, 534 files, `410,017 B` at item 3):
- `UintModel::length` uses a 6-bit `BitTree<6>` (symbols `0..63`), even though
  `uint32_t` bit lengths only span `0..32` (`float_exp` `0..9`, `float_mant`
  `1..23`). Starting uniform wasted cold-start probability on impossible bit
  widths `33..63` across every freshly created `UintModel`.
- `StringModel::order0` (which seeds every newly touched order-1 byte context)
  started uniform over all 256 byte values, even though the grammar's string
  primer already defines a grammar-derived byte distribution (ASCII letters,
  digits, `_`, NUL). In addition, `CodeString` attempted an order-2 match at
  byte 0 using `(prev_string_last_char, '\0')`, which crossed unrelated
  strings and added noisy miss flags.

**Implementation:**
- `src/prob.h`: compile-time `MakeSymbolCdfPrior` and `MakeUintLengthTreePrior`
  seed `UintModel::length` root and leaf-32 tables over `0..32` (`0..9` for
  `float_exp`, `1..23` for `float_mant` via `EnsureUintWithPrior`). Zero
  runtime construction cost and pure-math bounds only.
- `scripts/gen_grammar_tables.py`, `src/grammar_tables.*`: emit a grammar-
  derived `order0` root + 16-nibble leaf prior pool from the existing string
  primer (`GetStringOrder0RootPrior()`, `GetStringOrder0LeafPrior()`).
  `grammar_tables_test` rebuilds every table with `SymbolCdfSetPrior` and
  checks exact equality.
- `src/model.cpp`: `StringModel` loads the `order0` root prior on construction
  and leaf priors lazily (`MakeStringOrder0Leaf`), eagerly ensures the 4 KB
  match table before the first byte-tree allocation so it fits `bytes.arena`'s
  first block without stranding, and skips match lookup at byte 0 (`!coded.empty()`).

| | corpus bytes | Δ vs item 3 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 3 (`e76155b`) | 844,347 | — | 0.13429 | 0.14430 | 320.2M | 77.44 MiB/s |
| **landed: B2+B3** | **833,282** | **−1.31%** | 0.13251 (−1.33%) | 0.14243 (−1.30%) | **320.4M (+0.05%)** | 76.81 MiB/s (−0.93%) |

Mean ratio: train 0.17861 → 0.17388 (−2.65%), holdout 0.17793 → 0.17340
(−2.55%). Median ratio: train 0.17472 → 0.17154 (−1.82%), holdout 0.17285 →
0.16976 (−1.79%). Small modules gain heavily from eliminating cold-start
uniformity. Golden vectors: `GoldenSpirvModuleEncoding` shrinks 68 → 66 B;
header-only vector unchanged at 14 B.

**Speed, memory, size.** Decode Ir is virtually flat: 320.2M → 320.35M
(+0.05%). Wall time (quiet host, `taskset -c 4`, `arixv_decode_bench
--iters=10` on all 1080 files, 12 interleaved rounds): median 77.44 → 76.81
MiB/s, median paired −0.93% (−1.59% to −0.04%). Memory (`memory_breakdown`):
heap/decode 183.6 → 183.6 KB (+78 B); batched minor page faults 0.85 → 0.85
(0.848). Library (−O2, text+data): 311.3 → 314.5 KB (+3.2 KB).

Checks: `make all`, `make test` (generator `--check`, new prior tests in
`grammar_tables_test` and `model_test`), `make asan`, 3000-iteration fuzz,
valgrind memcheck with full leak checking (`arixv_test`, `model_test`),
scalar/SIMD cross-check on 135 files, all 1080 corpus files round-trip.

### 18. Adaptive same-instruction and adjacent-SSA ID list source (2026-11-21)

**Train-only analysis** (`id_ref` ~282 KB full corpus). Item 2 always picked
a slot's candidate recency list as "the list this `(instruction, slot)` used
last time" (`own_last`). Polymorphic instructions (`OpFAdd`, `OpFMul`,
`OpSelect`, `OpStore`, `OpPhi`, `OpCompositeConstruct`, `OpExtInst`) switch
types frequently between calls, causing repeated misses even when the type
matches an earlier operand in the same instruction (`op0_list`,
`prev_op_list`, `elem(prev_op)_list`) or the immediately preceding in-block
instruction's result (`prev_res_list`).

**Implementation** (all in `src/model.cpp`, zero extra bytes in `InstGroup`):
- Repurposed `InstGroup::last_id_valid[16]` as `last_id_state[16]`: bit 0 =
  valid, bits 1–3 = adaptive `id_idx > 0` mode (`own_last`, `op0_list`,
  `prev_op_list`, `elem(prev_op)_list`, `prev_res_list`), bits 4–5 = disjoint
  2-bit hysteresis counter (`0..2`, init `1`, selects `prev_res_list` iff `2`)
  for `id_idx == 0`. Keeping `op0`/`gt0` bitfields disjoint prevents variadic
  instructions from clobbering each other's mode.
- Added 4 `uint32_t` per-instruction fields on `ModelState` (`+16 B`) with
  lazy `PrevResList` resolution and structural cold-start fallback
  (`InitialIdListForSlot`) on a slot's first occurrence. Cold helpers are
  `ARIXV_NOINLINE`. Zero extra entropy bits are coded — list-source selection
  is 0-bit adaptive.

| | corpus bytes | Δ vs item 17 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 17 (`015b68c`) | 833,282 | — | 0.13251 | 0.14243 | 320.4M | 77.06 MiB/s |
| **landed: B1** | **819,900** | **−1.61%** | 0.13046 (−1.55%) | 0.14006 (−1.66%) | 327.4M (+2.21%) | 74.54 MiB/s (−3.20%) |

Holdout generalizes even better than train (−1.66% vs −1.55%). Mean ratio:
train 0.17388 → 0.17264 (−0.71%), holdout 0.17340 → 0.17197 (−0.82%). Median
ratio: train 0.17154 → 0.16955 (−1.16%), holdout 0.16976 → 0.16739 (−1.40%).
Golden vector `GoldenSpirvModuleEncoding` stays 66 B (3 bytes updated).

**Speed, memory, size.** Decode Ir 320.35M → 327.43M (+2.21%). Wall time
(quiet host, `taskset -c 4`, `arixv_decode_bench --iters=10` on all 1080
files, 12 interleaved rounds): median 77.06 → 74.54 MiB/s, median paired
−3.20% (−3.98% to −1.06%). Memory (`memory_breakdown`): heap/decode 183.6 →
183.5 KB (−176 B, fewer escape `UintModel` allocations); batched minor page
faults 0.848 → 0.846. Library (−O2, text+data): 314.5 → 319.9 KB (+5.4 KB,
`model.o`).

Checks: `make all`, `make test` (new `AdaptiveIdListSourceTests` in
`model_test`), `make asan`, 3000-iteration fuzz, valgrind memcheck with full
leak checking (`arixv_test`, `model_test`), scalar/SIMD cross-check on 135
files, all 1080 corpus files round-trip.

### 19. Pointer-derived `OpVariable` `StorageClass` and type-algebra deferred `ResultType` (2026-11-21)

**Train-only analysis** (`[0-7]*.spv`, 534 files). Three structural gaps
remained in `result_type` and `value_enum` coding, all derivable directly
from SPIR-V spec type rules with zero shipped corpus knowledge:
- **B4 (`OpVariable` `StorageClass`):** `OpVariable`'s `StorageClass` operand
  matches the storage class of its `OpTypePointer` `ResultType` 100% of the
  time on valid modules (`−2,314 B` on train alone).
- **B5 (`ElementTypeOf` on type-ID operands):** `CodeDeferredResultType` only
  checked `ElementTypeOf(TypeOf(operand))`, missing instructions like
  `OpFunction` whose operand *is* a `Function` type whose element type is the
  return type (`−824 B` on train alone).
- **A2 (Type-algebra `ResultType` candidates):** `OpAccessChain` /
  `OpInBoundsAccessChain` (`−5,198 B`), `OpCompositeConstruct` (`−2,582 B`),
  comparisons → `bool`/`bvecN` (`−1,346 B`), `OpVectorShuffle` (`−895 B`),
  scalar/vector conversions (`−901 B`), `OpFunctionParameter` (`−441 B`),
  image sampling → `vec4` (`−315 B`), `OpCompositeExtract` (`−124 B`), bool
  0-op constants, `OpSampledImage`, and matrix-vector/matrix-matrix products
  all have spec-defined result types computable from tracked type declarations.

**Implementation:**
- `src/tracker.h`, `src/tracker.cpp`: kept `Tracker::IdInfo` at 32 bytes
  (reusing union `members`/`ext_set`, `aux_flag`, and `kImage`→`OpTypeSampledImage`
  backlink in `width`), added bounded reverse type tables (`kMaxMemberTypes =
  4096`, `kMaxDerivedTypeEntries = 256`), inline `scalar_types_[3][4]` peer
  table, function-header parameter tracking, integer constant value tracking,
  and depth-capped (`kMaxWalkDepth = 32`) composite walks.
- `src/model.cpp`: B4 predicts `OpVariable` `StorageClass` via
  `ModelState::var_sc_matches` (`ValidFlag()`, lossless fallback on mismatch
  or non-pointer), B5 adds `ElementTypeOf(id_operands[i])`, and non-template
  `BuildDeferredResultTypeCandidates` emits shape-first candidates into the
  existing `BitTree<3>` (`BitTree<4>` regressed +1,024 B on train) with an
  inline `choice == 0` decode fast path.

| | corpus bytes | Δ vs item 18 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 18 (`61f4ac2`) | 819,900 | — | 0.13046 | 0.14006 | 327.4M | 74.64 MiB/s |
| **landed: A2+B4+B5** | **788,893** | **−3.78%** | 0.12546 (−3.83%) | 0.13483 (−3.74%) | 333.0M (+1.69%) | 72.49 MiB/s (−2.85%) |

Mean ratio: train 0.17264 → 0.16692 (−3.31%), holdout 0.17197 → 0.16625
(−3.33%). Median ratio: train 0.16955 → 0.16332 (−3.67%), holdout 0.16739 →
0.16137 (−3.60%). Golden vector `GoldenSpirvModuleEncoding` shrinks 66 → 64 B.

**Speed, memory, size.** Decode Ir 327.43M → 332.97M (+1.69%). Wall time
(quiet host, `taskset -c 4`, `arixv_decode_bench --iters=10` on all 1080
files, 12 interleaved rounds): median 74.64 → 72.49 MiB/s, median paired
−2.85% (−3.69% to −2.24%). Memory (`memory_breakdown`): heap/decode drops
183.5 → **181.5 KB (−2.0 KB)** and arena blocks/decode drops 93.0 → 90.4 KB
(−2.6 KB) because skipping `OpVariable` `ValueEnumModel` and fallback
`UintModel` allocations outweighs the compact tracker tables; batched minor
page faults drop **0.846 → 0.612 (−27.7%)**. Library (−O2, text+data): 319.9
→ 330.2 KB (+10.4 KB: +5.8 KB `model.o`, +4.5 KB `tracker.o`).

Checks: `make all`, `make test` (new `TypeAlgebraAndVariableStorageClassTests`
in `model_test`), `make asan`, 3000-iteration fuzz, valgrind memcheck with
full leak checking (`arixv_test`, `model_test`), scalar/SIMD cross-check on
135 files, all 1080 corpus files round-trip.

### 20. Grammar-implied and optional-tail instruction `word_count` deferral (2026-11-21)

**Train-only analysis** (`word_count` ~43.5 KB full corpus). Many variable-length
SPIR-V instructions have lengths determined by the grammar plus their already-
coded operands:
- Trailing-string ops (`OpString`, `OpName`, `OpMemberName`, `OpExtension`,
  `OpSourceExtension`, etc.), enum-determined ops (`OpDecorate`,
  `OpMemberDecorate`, `OpExecutionMode`, `OpLoopMerge`, etc.), and
  `OpConstant`/`OpSpecConstant` (width from `ResultType`) all terminate
  unambiguously when `OperandCursor` reaches `kEnd` (`kInstGrammarImpliedWordCount`).
- Fixed `OpExtInst` calls (e.g. `Sin`, `Fma`, `DebugBreak`) have a fixed
  operand count once the extended instruction number is decoded, falling back
  to `CodeVarWordCount` only for variable or unknown ext-insts.
- Single-optional-tail `?` ops (`OpVariable`, `OpLoad`, `OpStore`,
  `OpTypeFloat`, `OpImageFetch`, etc., marked `kInstOptionalTail`) only need
  a 1-bit `has_optional` flag (seeded with `ValidFlagZero()` since optional
  tails are absent by default) and retain the `fixed_layout` fast path when
  absent or single-word.

**Implementation:**
- `scripts/gen_grammar_tables.py`, `src/grammar_tables.*`: emit
  `kInstGrammarImpliedWordCount` and `kInstOptionalTail` flags mechanically
  from the pinned Khronos grammar (`--check` verified zero diff).
- `src/operand_cursor.h`: added `OperandCursor::SetRemaining` for `OpExtInst`
  variable/unknown fallback mid-instruction.
- `src/model.cpp`: `ValidateGrammarImplied` and `ValidateExtInstConsistent`
  verify exact encoder/decoder `OperandCursor` equivalence before deferring,
  falling back losslessly to explicit `word_count` on any non-canonical or
  truncated instruction. Moved `word_count_consistent` from per-class
  `CountModel` to `ModelState` so fixed, grammar-implied, `OpExtInst`, and
  optional-tail classes skip allocating `CountModel` altogether (`11.7 → 4.8`
  per decode). `CodeModule` patches `word0` before `CodeDeferredResultType`
  and `Tracker::OnInstructionEnd`.

| | corpus bytes | Δ vs item 19 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 19 (`e5e81ec`) | 788,893 | — | 0.12546 | 0.13483 | 333.0M | 72.42 MiB/s |
| **landed: A1** | **763,727** | **−3.19%** | 0.12137 (−3.26%) | 0.13061 (−3.13%) | **330.7M (−0.67%)** | 71.82 MiB/s (−0.94%) |

Mean ratio: train 0.16692 → 0.15875 (−4.90%), holdout 0.16625 → 0.15834
(−4.76%). Median ratio: train 0.16332 → 0.15703 (−3.85%), holdout 0.16137 →
0.15432 (−4.37%). Golden vector `GoldenSpirvModuleEncoding` shrinks 64 → 59 B.

**Speed, memory, size.** Decode Ir drops 332.97M → **330.74M (−0.67%)**. Wall
time (quiet host, `taskset -c 4`, `arixv_decode_bench --iters=10` on all 1080
files, 12 interleaved rounds): median 72.42 → 71.82 MiB/s, median paired
−0.94% (−1.63% to −0.02%; outlining `CodeVarWordCount` was tested and
regressed Ir to 337.8M / wall to 69.79 MiB/s, so it was reverted). Memory
(`memory_breakdown`): heap/decode drops 181.5 → **178.6 KB (−2.9 KB; −5.0 KB
vs item 3)** and arena blocks/decode drops 90.4 → 88.0 KB (−2.4 KB) from lazy
`CountModel` avoidance; batched minor page faults 0.612 → 0.629. Library
(−O2, text+data): 330.2 → 338.0 KB (+7.8 KB, `model.o`).

Checks: `make all`, `make test` (generator `--check`, new tests in
`grammar_tables_test`, `operand_cursor_test`, and `DeferredWordCountModules`
in `model_test`), `make asan`, 3000-iteration fuzz, valgrind memcheck with
full leak checking (`arixv_test`, `model_test`), scalar/SIMD cross-check on
135 files, all 1080 corpus files round-trip.

### 13. Container and coder framing overhead reduction (2026-11-21)

**Train-only analysis** (`[0-7]*.spv`, 534 files). Container and entropy-coder
framing overhead was `11.01 B/module` above raw entropy (`4 B` `"ARX2"` magic,
`1 B` mode byte, `2.05 B` uncompressed size varint, `~1.5 B` range-coder flush,
`~2.45 B` rANS final state).

**Implementation** (keeping `"ARX2"` magic and byte-4 mode byte intact):
- `src/range_coder.h`: adaptive minimal flush in `RangeEncoder::Flush` (`0`
  bytes when `range_ == 0xFFFFFFFFu`, `2` bytes when `range_ >= 0x01FFFFFFu`,
  `3` bytes otherwise) with matching `RangeDecoder::TailSlack()` derived from
  `range_`.
- `src/symbol_coder.h`: lowered `kRansL` from `2^16` to `2^15` with variable
  3/4-byte final state encoding (`y = x - kRansL`, MSB of last byte selects 3
  vs 4 bytes with canonical/bounds validation), plus deferred renormalization
  in `RansDecoder::DecodeSymbol` paired with seeding the final symbol at
  `x = kRansL + lo` in `RansEncoder::Flush`.
- `src/arixv.cpp`: canonical 1..5-byte prefix-coded `AppendCompactUint32` /
  `ReadCompactUint32` (`0..191` in 1 B, `192..15,551` in 2 B, preserving the
  ≤10 B max header guarantee), coding SPIR-V body-word count `(size - 20) / 4`
  for `kModeSpirv` with overflow/OOM checks, plus a bounds check fixing a
  1-byte OOB read on exact 4-byte `"ARX2"` inputs.

| | corpus bytes | Δ vs item 20 | train total | holdout total | decode Ir | wall |
|---|---|---|---|---|---|---|
| item 20 (`f9be40c`) | 763,727 | — | 0.12137 | 0.13061 | 330.7M | 71.72 MiB/s |
| **landed: item 13** | **761,259** | **−0.32%** | 0.12098 (−0.32%) | 0.13019 (−0.32%) | 332.2M (+0.44%) | **72.20 MiB/s (+0.55%)** |

Saves **2.29 B/module** across the 1,080-file corpus (`−2,468 B` total).
Mean ratio: train 0.15875 → 0.15735 (−0.88%), holdout 0.15834 → 0.15697
(−0.87%). Median ratio: train 0.15703 → 0.15641 (−0.39%), holdout 0.15432 →
0.15340 (−0.60%). Golden vectors shrink: `GoldenSpirvModuleEncoding` 59 → 56 B,
`GoldenSpirvHeaderOnlyEncoding` 14 → 11 B.

**Speed, memory, size.** Decode Ir 330.74M → 332.20M (+0.44%). Wall time
(quiet host, `taskset -c 4`, `arixv_decode_bench --iters=10` on all 1080
files, 12 interleaved rounds): median 71.72 → **72.20 MiB/s (+0.55%, 9/12
rounds faster)**. Cumulative vs item 3 (`e76155b`): corpus bytes **844,347 →
761,259 B (−9.84%)**, mean **0.17827 → 0.15715 (−11.85%)**, median
**0.17333 → 0.15430 (−10.98%)**, heap/decode **183.6 → 178.6 KB (−5.0 KB)**,
batched minor page faults **0.85 → 0.628 (−26.1%)**, wall **77.61 → 72.25
MiB/s (−6.92%)**. Library (−O2, text+data): 338.0 → 340.3 KB (+2.3 KB).

Checks: `make all`, `make test` (new `CompactSizeEdgeCases`, expanded
`HostileInputs`, `AdaptiveFlushBoundaries`, and `RansMinimalFlushAndHostile`),
`make asan`, 3000-iteration fuzz, valgrind memcheck with full leak checking
(`range_coder_test`, `arixv_test`, `model_test`), scalar/SIMD cross-check on
135 files, all 1080 corpus files round-trip.



