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
