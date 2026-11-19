# Decode performance plan

Goal: several-X decode throughput gains, accepting small compression-ratio
losses where necessary. Baseline (2026-07-06): ~18 MiB/s decode vs ~650-1250
MiB/s for smol-v/yari-v/zstd baselines.

## What profiling says

Measured on a 216-file corpus sample (`perf record`/`perf stat` on an
instrumented `tools/bench/decode_bench.cpp` variant; every number below is from
decode of real corpus files):

- **The entropy coder is not the main cost — the model bookkeeping around it
  is.** The decoder performs only **2.47 adaptive bit-decodes per output
  byte** (~10 per SPIR-V word). A well-tuned LZMA-style bit decode costs
  ~15-25 cycles, so coding alone should support ~50-80 MiB/s; we are at 18.
- **Not stall-bound: work-bound.** IPC is 2.55 with 0.86% branch misses. The
  decoder executes ~600 instructions per output byte. This is cuttable fat,
  not memory latency.
- Flat profile of decode:
  - ~11% `strcmp` (per-instruction name-comparison chains)
  - ~15-20% `ModelState` ctor/dtor + `malloc`/`free`/`memset`
  - ~12% Tracker (`OnInstructionEnd`, `RecencyList::Touch`, `Ensure`)
  - ~5% `OperandCursor::Next`/`Consume`
  - ~3.5% lazy `make_unique<BitTree<10>>` inside `CodeOpcode`
  - only ~30% in the actual bit-coding loops
- Throughput is nearly flat across file sizes (14.8 MiB/s for <4 KB files →
  19.9 MiB/s for >64 KB), so steady-state per-word cost dominates, not
  per-file setup alone.

## Re-profile after Tier A (2026-07-07)

Callgrind instruction counts (perf is unavailable in this sandbox;
the decoder is work-bound — IPC 2.55, 0.86% branch misses — so Ir ≈ time),
108-file corpus sample, 30 decode iterations, `-O3 -flto=auto` build:

- **Reusable decoder: the adaptive bit-decode chain now dominates.**
  ~63% of instructions sit in the coding path (`CodeModule<BitDecoder>` body
  44.7%, `CodeUint` 11.5%, `CodeByte` 4.4%, non-inlined `DecodeBit` 2.5%).
  Structure walking is down to ~20%: cursor `Next`+`Consume` 8.8%, tracker
  ~11% (`OnIdUse` 3.7%, `RecencyList::Touch` 3.3%, `OnInstructionEnd` 2.3%,
  `IdMap` 2.2%), memset 7.6%. Per the attack order, items 7+8
  (encoder-snapshotted static probabilities + multi-symbol decode) are now
  the highest-leverage change; item 9 (interleaved streams) attacks the same
  serial-dependency bottleneck.
- **Stateless decoder: `ModelState` construction is the other half of the
  story.** memset alone is 22% of stateless-decode instructions (vs 7.6%
  reusable) plus ~4.5% malloc/free and ~4% ctor/dtor walking — i.e. the
  fresh-model zeroing per `Decode()` call costs roughly a fifth of stateless
  decode. That is the already-known item-2 trade; closing it means either
  shrinking/lazily-allocating `ModelState`'s inline arrays or steering
  callers to the reusable `arixv::Decoder`.
  *Resolved as far as pure statelessness allows (2026-07-07).* A fresh model
  per call allocated+zeroed ~465 KB (131 KB of it an eagerly-built
  `BitTree<16>` for *unknown opcodes*) and — measured with a trivial-module
  microbench — cost 78 us/call, most of it glibc heap-trim churn: freeing
  the model at the top of the heap returns the pages to the kernel, which
  zero-fills them again on the next call (4 `brk` syscalls per decode;
  raising `M_TRIM_THRESHOLD` alone dropped it to 7.5 us). Cached-decoder
  designs (thread_local, then a process-wide single slot) were built,
  measured at ~1 us/call, and *rejected on interface grounds*: the free
  `Decode()` must stay truly stateless (a thread pool where each worker
  decodes once would retain memory per thread; even one process-wide slot
  changes the memory story). Instead the fresh-model cost itself was cut,
  all byte-identical, in three steps (trivial-module us/call → corpus
  MiB/s at each step):
  1. *Lean allocations:* lazy unknown-opcode tree, uninitialized
     stamp-guarded `last_id`/`last_literal`/`last_word_count` value arrays
     (`UninitArray`), deleted the write-only `last_id_seen`. ~465 → ~239 KB
     zeroed; wall time unchanged (the kernel re-zeroes trimmed pages
     regardless) but −15% instructions.
  2. *Two-level stamps:* one `inst_group_stamp` per instruction; the
     per-slot stamp groups (16 slots x 2 arrays, 112 KB) are zeroed lazily
     128 B at a time on an instruction's first touch per generation.
     76.7 → 32.2 us; corpus 42.3 → 43.5 MiB/s.
  3. *Lazy rare models*, scoped by corpus-measured use counts (operand_bytes
     — opaque words — 4/1080 files despite the common-sounding name;
     raw counts 1/1080; spec-op 16/1080; ext-unknown 4/1080; exact-bound
     27/1080; `id_escape`/`type_escape` escape in most files but through
     only a few of their 27/29 slots → per-slot lazy). `string_bytes`,
     `result_delta`, `context_number` measured common and stay eager.
     32.2 → 23.8 us; corpus ~43.5 → ~43.8 MiB/s (runs 43.1-44.1).
  Net: fresh-model overhead 76.7 → 23.8 us/call (~91 KB still zeroed —
  value_enums/opcode_contexts/count_models pointer+stamp arrays and the
  ~30 KB inline model are the remainder); reusable decoder unaffected
  (49.1-49.6 MiB/s before and after) and its BeginDecode floor dropped
  1.1 → 0.4 us. `make test`, `make asan`, 3000-iter fuzz all pass.
- Overall work is down to ~265 instructions per output byte (was ~600 at
  the 18 MiB/s baseline), consistent with the measured ~2.3x.

## Tier A — no format change, no ratio cost (~3-4x combined, realistic)

Note for future items: the container format has not shipped, so maintaining
format compatibility is not required.

1. **[Complete] Kill the strcmp chains (~13% estimated; +28.7% achieved).**
   `ResultClassBucket` (model.cpp:337)
   builds `std::string`s and compares names *per result ID*;
   `TypeClassForOpcode`, `IsTerminator`, `SectionOfOpcode` do strcmp chains
   *per instruction*. All are pure functions of the instruction index —
   precompute them into the generated grammar tables
   (`scripts/gen_grammar_tables.py`): a `uint8_t` per instruction for result
   bucket, type class, terminator flag, section. Single easiest win.
   Achieved on the full 1080-file corpus with 20 decode iterations:
   clean `HEAD` averaged 18.42 MiB/s, patched averaged 23.71 MiB/s.

2. **[Complete] Stop allocating in the decode loop (~15-20% estimated;
   stateless +6.4%, reusable +31.1% achieved).** Three sources:
   - **[Complete]** `ModelState` is fully constructed/destroyed per `Decode` call — expose a
     reusable context object (`arixv::Decoder`), which also
     fits the "link the library, decompress many shaders at load" story.
     *Measured dead end as an eager reset (2026-07-06):* a `Reset()` that
     walks the model costs the same as ctor/dtor (it re-zeros every tree ever
     allocated — the high-water mark, not the file), netting ~0% overall and
     −8% on <4 KB files, while retaining high-water memory per holder (bad
     for thread pools). Only worth revisiting with *lazy* reset (per-context
     generation stamps) and caller-owned decoder objects; consider alongside
     item 3.
     Landed as caller-owned `arixv::Decoder` plus internal `ModuleDecoder`:
     the free `Decode()` remains stateless by constructing a local decoder,
     while reusable decoders bump a generation per decode and lazily reset
     only touched high-water contexts. Full 1080-file corpus, 20 decode
     iterations, byte-identical to `HEAD` (1,011,952 encoded bytes, checksum
     35,796,698,800): clean `HEAD` 23.50 MiB/s, patched stateless
     25.00 MiB/s, patched reusable 30.80 MiB/s. Buckets (`HEAD` →
     stateless → reusable): <4 KiB 19.86 → 20.33 → 32.76 MiB/s;
     4-16 KiB 23.67 → 25.37 → 30.63 MiB/s; >16 KiB 26.12 → 28.50 →
     29.89 MiB/s.
   - **[Complete]** `inst_words`/`coded_words` are fresh `std::vector`s per
     instruction/operand — use fixed stack buffers (operand spans are tiny).
     Landed as inline/spill `WordBuffer` (+ hoisted `string_hints`):
     bitstream-identical, 23.03 → 24.1-24.5 MiB/s on the full corpus
     (~+4.5-6.5%), gains across all size buckets.
   - **[Complete]** Lazy `make_unique<BitTree<10>>` (4 KB zeroed) per new opcode context,
     plus the `TrainBitTree` shadow-training that doubles opcode work during
     warm-up — pool these; consider a smaller tree for order-1 contexts.
     Pooled for reusable decoders with per-context stamps; tree sizes and
     shadow-training unchanged, so there is no format or ratio cost.
     (Note: dropping the order-0 warm-up entirely was measured at +0.94%
     encoded size for only ~+2% decode speed — not a Tier A trade.)

3. **[Complete] Cheapen the per-bit hot path (no explicit estimate;
   stateless +4.8% achieved).** `Prob` was 4 bytes with a saturating
   counter and an `AdaptShift()` branch on every update. Landed as a packed
   16-bit probability for the default tuning: low 12 bits hold the range-coder
   probability and the high 4 bits hold the warm-up count. The packed counter
   saturates at stage 15 because stage 16 already uses the steady
   `kProbShift`, so the default bitstream is unchanged. `RangeEncoder` and
   `RangeDecoder` now read the raw probability state once per bit and pass it
   into the update path.
   Full 1080-file corpus, 100 decode iterations, byte-identical to `HEAD`
   (1,011,952 encoded bytes, checksum 178,987,814,000): clean `HEAD`
   stateless 24.83 MiB/s, patched stateless 26.02 MiB/s. Buckets (`HEAD` →
   patched): <4 KiB 20.50 → 22.69 MiB/s; 4-16 KiB 25.07 → 26.16 MiB/s;
   >16 KiB 28.05 → 28.31 MiB/s. Reusable-decoder scratch bench was neutral
   overall (30.58 → 30.58 MiB/s), with <4 KiB up 32.83 → 34.24 MiB/s and
   larger buckets slightly noisy/down. Measured dead ends: shift table lookup
   was slower than `clz`; forcing an all-steady schedule inflated the corpus
   to 1,248,794 bytes (+23.4%) without improving decode.

4. **[Complete] Leaner recency lists (~5% estimated; +10.2% achieved).**
   `RecencyList::Find` linearly scanned up to 64 entries (128 for value
   lists) and `Touch` memmoved a `std::vector`. Landed as 16-entry rank-list
   caps plus fixed inline storage with explicit size, so `Find`/`Touch` are
   hard-bounded and avoid vector insert/erase/allocation overhead. This is an
   intentional ratio/speed trade now that the format has not shipped.
   Full 1080-file corpus, 100 decode iterations: previous item-3 baseline
   25.47 MiB/s, patched 28.08 MiB/s. Buckets (`baseline` → `patched`):
   <4 KiB 22.09 → 23.50 MiB/s; 4-16 KiB 25.62 → 28.05 MiB/s; >16 KiB
   27.80 → 31.63 MiB/s. Encoded bytes increased 1,011,952 → 1,037,449
   (+2.52%).

5. **[Complete] Flatten `OperandCursor` and `Tracker::OnInstructionEnd`
   (+35.2% stateless, +36.4% reusable achieved).** The tracker re-parsed
   each instruction after decode; much of what it computes (does this opcode
   define a type/constant/label, …) can come from the same precomputed
   per-instruction table as item 1. Replace the `ids_` `unordered_map` with
   an open-addressing flat map. Landed as five pieces, all verified
   byte-identical on the full corpus:
   - `OnInstructionEnd` no longer re-walks operands with an `OperandCursor`:
     the generator now guarantees result-type/result-ID sit at words 1/2 and
     emits a per-instruction `aux_word` (fixed word index of the type width /
     component type / import-name operand), so the semantic update is a few
     direct word reads. Per-operand `Ensure` calls were already subsumed by
     coding-time `OnIdUse`/`OnResultId`.
   - `ids_` is now an open-addressing flat map (linear probing, power-of-two
     arrays, key 0 = empty with a dedicated slot for ID 0, doubling growth;
     `Reserve` still clamped against hostile headers).
   - Deleted tracker state that no model version ever read (confirmed across
     all of git history): the per-result-type value recency lists + LRU
     (`values_of_type_`, hash find + `O(n)` vector churn on every value
     use/def — alone worth ~7%), `recent_labels_`, `recent_values_`, and the
     write-only `IdInfo::type_id`/`component_type` fields (entries shrink
     24 → 16 bytes).
   - `OperandCursor::Consume` reuses `field.category` from `Next()` instead
     of re-fetching the operand-kind desc.
   - Fixed, grammar-consistent layouts now bypass `OperandCursor` entirely:
     the generated operand slots provide each one-word field directly. The
     full corpus has 117,112 such instructions (299,041 operand words, 26%
     of all operand words), so this removes the general cursor state machine
     from a material fraction of the hot loop.
   Full 1080-file corpus, 100 decode iterations, byte-identical to `HEAD`
   (1,037,449 encoded bytes, checksum 178,987,814,000): clean `HEAD`
   stateless 27.11 MiB/s, patched stateless 36.66 MiB/s; reusable decoder
   32.51 → 44.35 MiB/s. Stateless buckets (`HEAD` → patched): <4 KiB
   22.21 → 27.21 MiB/s; 4-16 KiB 27.39 → 38.31 MiB/s; >16 KiB
   30.99 → 45.50 MiB/s. `make test` (incl. generated-table check and 500-iter
   fuzz), `make asan`, and a 2000-iter fuzz run all pass.

6. **[Complete] Build flags (+11.5% achieved).** The library shipped at
   `-O2`; `-O3 -flto` is a likely free 10-20% for this kind of code. Landed
   as `CXXFLAGS ?= -O3 -flto=auto -std=c++17 -Wall -Wextra` (`=auto`
   parallelizes LTRANS and silences the serial-jobs warning); `?=` keeps
   environment overrides working and the ASan flags are unchanged.
   Full 1080-file corpus, 100 decode iterations, byte-identical output under
   every variant: item-5 tree at `-O2` 37.38 MiB/s, `-O3` 40.05 (+7.1%),
   `-O2 -flto` 37.80 (+1.1%), `-O3 -flto` 41.68 MiB/s (+11.5%). Reusable
   decoder 44.35 → 49.59 MiB/s. Buckets (`-O2` → `-O3 -flto=auto`,
   per-file mode): <4 KiB 27.21 → 31.23 MiB/s; 4-16 KiB 38.31 → 42.44 MiB/s;
   >16 KiB 45.50 → 48.97 MiB/s. `make test` passes on a clean rebuild with
   the new flags; no new warnings surfaced at LTO link time.

## Current profile (2026-07-10)

Full 1080-file corpus, core-pinned Ryzen 9 5950X sandbox, `-O3 -flto=auto`,
100 decode iterations. Root `perf` is available in this sandbox; unprivileged
events are blocked by `perf_event_paranoid=4`.

- The reusable decoder runs at 51.8 MiB/s on `HEAD` and 53.1-53.3 MiB/s with
  the fixed-layout cursor path above (+2.6%); stateless runs at 46.5-46.8 →
  47.5-47.7 MiB/s (+~2.0%). Encoded bytes and checksum remain exactly
  1,037,449 and 178,987,814,000.
- The matched reusable `perf stat` run drops 141.1B → 138.0B instructions
  (-2.2%) and 54.2B → 52.8B cycles (-2.5%), with IPC essentially unchanged
  (2.60 → 2.61). The decoder is still work-bound, not branch-bound (about
  1.5% branch misses).
- Reusable flat profile before the fixed-layout path: `CodeModule` body
  46.5%, `CodeUint` 14.0%, `CodeByte` 4.3%, and `RangeDecoder::DecodeBit`
  1.7%; cursor `Next`+`Consume` 11.8%; tracker (`OnIdUse`, instruction end,
  recency touch, ID map) 9.8%. Stateless decode additionally spends roughly
  8% in model allocation/destruction.

Next no-format experiments, in order:

1. **[Complete] Decode-only direct word emission.** `coded_words` is normally a
   one-word staging buffer, then copied to `inst_words` and `out`; its
   `push_back` alone is 1.25% of reusable samples. Special-case the common
   single-word fields in the decoder and retain the buffer only for strings
   and multiword context-dependent numbers. Landed as an explicitly inlined
   `SingleWordStager` for the decode path; the encoder retains the buffer.
   Full 1080-file corpus, core-pinned, 100 iterations: reusable decode
   improved 52.61 → 53.91 MiB/s (+2.5%), with identical encoded bytes and
   checksum. No ratio change.
2. **[Complete] Cheaper reusable model reset (+3.8% reusable, +2.0% stateless
   achieved).** `__memset` was 1.85% of reusable samples under
   `Decoder::Decode`. Attribution (no profiler in this sandbox — perf absent,
   `perf_event_paranoid=4`, no valgrind — so this was done by ablation): the
   dominant memset is `Tracker::IdMap::Clear`, which `std::fill`ed the entire
   open-addressing key array (a power of two grown to hold every distinct ID in
   the module) on every decode; the `BeginDecode` model-array resets are small
   fixed-size fills independent of module size. Landed as generation-stamped
   `IdMap` slots: each slot carries a `slot_gen_` stamp and is occupied iff
   `slot_gen_[i] == generation_`, so `Clear()` is now a single generation bump
   instead of an O(slots) fill (the stamp array is bulk-reset only on the
   ~4-billion-module generation wrap). This was chosen over a touched-slot
   clear list, which would add a per-ID-definition push on the hot path;
   generation stamps keep the reset O(1) and add only a stamp compare per probe
   plus one `uint32_t`/slot of memory. The ablation win directly measures
   `IdMap::Clear`'s former share.
   Full 1080-file corpus, core-pinned Ryzen 9 5950X, `-O3 -flto=auto`, 100
   iterations, byte-identical (1,041,769 encoded bytes, checksum
   178,987,814,000): matched per-file reusable 53.1 → 55.1 MiB/s (+3.8%),
   stateless 47.4 → 48.3 MiB/s (+2.0%). Reusable buckets (base → patched):
   <4 KiB 51.6 → 56.5 MiB/s (+9.4%, the fixed per-module clear is a larger
   fraction of small-file work); 4-16 KiB 53.3 → 55.1 MiB/s (+3.5%); >16 KiB
   53.9 → 54.4 MiB/s (+0.8%). `make test` (incl. 500-iter fuzz) and a
   3000-iter fuzz pass; ASan/UBSan runs are memory-clean (see note below).

   *Sandbox note:* the `make asan` model/arixv-test binaries hit a
   nondeterministic exit-time `DEADLYSIGNAL`/SIGSEGV in this sandbox at ~20-30%
   of runs. It reproduces identically on the pre-change baseline binary (9/40
   vs 11/40 patched), never reproduces under gdb, and one captured trace sat in
   `__lsan::CheckForLeaks`/`_dl_fini`, i.e. the sanitizer's exit-time
   stop-the-world scan under the container's ptrace restrictions — not decoder
   code. Runs that exit cleanly report zero ASan/UBSan errors, so the change
   itself is memory-clean.
3. **[Implemented on branch `static-opcode-tables`, 2026-07-11 — adoption
   pending] Entropy format, not more micro-tuning.** The coding path is
   about two thirds of reusable cycles and remains serial because every
   adaptive bit updates its next probability. A fast optional mode with
   encoder-supplied static probabilities, or a narrow fixed/global
   multi-symbol table for only opcode/uint-length fields, is the remaining
   path to a large gain. Per-file broad rANS tables and four interleaved
   range streams were already measured and reverted (items 8-9).

   Both designs were probed and the surviving form is parked on the side
   branch (two commits; its copy of this file holds the full record):

   - `ee13d1b` codes known-opcode indices with GENERATED corpus-trained
     static probability trees (order-1 by previous instruction, with
     (section, prev-class)/section/global fallbacks). Full corpus:
     encoded bytes 1,041,769 → 990,130 (−4.96%), stateless decode
     48.31 → 56.57 MiB/s (+16.2%), reusable 55.42 → 62.87 MiB/s (+13.4%).
     Validated held-out (2.58 vs 3.68 bits/inst) and leave-one-source-out
     (wins on three of four corpus sources; worst case +3.6% opcode bytes
     ≈ +0.5% total on smol-v).
   - `ad3eb6a` adds a per-module fallback flag: a structural adaptive
     opcode coder (grammar-derived successor states + module/body rank
     alphabets; −1.4% vs the old coder at its speed) that the encoder
     selects per module when smaller, bounding any distribution mismatch.
     Default min-size selection: 984,505 bytes, at a ~4-5% decode
     throughput give-back on the large files that flip adaptive.

   Adoption is pending a design call: the tables abandon the README's
   "no corpus-trained tables / nothing table-shaped from the corpus" rule
   for this one coder. Measured dead ends recorded on the branch: per-file
   probability snapshots (median file touches 1,562 prob slots ≈ 3x its
   encoded bytes) and freeze-at-saturation update skipping (+0.9-1.6%
   speed for +13.6% size) — the per-bit `Update()` was never the cost.

## Tier B — small format/ratio tradeoffs (path to 100+ MiB/s)

7. **[Measured dead end — see item 3 of the no-format experiments and
   branch `static-opcode-tables` (2026-07-11).] Semi-adaptive
   probabilities: let the encoder do the adapting.** The design was: the
   encoder runs the adaptive model as today but *snapshots* converged
   probabilities for hot contexts into the container; the decoder loads
   static probabilities and skips `Update()` — the update was estimated at
   roughly half the per-bit dependency chain. Both halves failed
   measurement: per-file table bytes average ~3x the encoded payload
   (median file touches 1,562 probability slots carrying ~8.5 coded bits
   each), and a zero-table freeze-at-saturation prototype showed update
   skipping is worth only +0.9-1.6% speed while costing +13.6% size. The
   viable form of the idea — binary-baked corpus-trained static opcode
   tables, not per-container snapshots — lives on the branch.

8. **[Reverted experiment] Multi-symbol decode instead of bit trees.**
   A prototype added internal mode 3 (`kModeSpirvMulti`) with
   `table_blob`, `range_payload`, and `symbol_payload`, converting exactly
   known opcode index, known value/ext-inst enum index, and `CodeUint`'s
   6-bit length field to per-file sparse normalized rANS tables at
   `table_log=12`. Functionally it round-tripped and legacy mode 2 decode
   stayed intact, but the performance/size trade was bad enough that the
   implementation was reverted.
   Full 1080-file corpus, 20 decode iterations, `-O3 -flto=auto`: a forced
   mode-3 candidate was ~1.96 MB and beat legacy mode 2 on 0/1080 corpus
   files. The range+symbol payload shrank, but the per-file table blob
   dominated the savings (about 1.06 MB of table bytes across the corpus).
   With a temporary "choose smaller SPIR-V mode" selector, corpus size stayed
   at 1,037,449 encoded bytes and decode measured 43.79 MiB/s stateless,
   49.80 MiB/s reusable, i.e. no meaningful speed win over the range-only
   path. Conclusion: broad per-file multi-symbol tables are not the Tier-B
   perf lever; revisit only with fixed/global tables or a much narrower
   opcode-only design.

9. **[Reverted experiment] Interleaved entropy streams for ILP.** A prototype
   added internal mode 3 (`kModeSpirvInterleaved`) with four round-robin range
   streams and framing as four stream sizes followed by four payloads. It
   preserved old `kModeSpirv` decode and built both SPIR-V candidates before
   choosing the smaller coded blob, but the implementation was reverted after
   measurement showed no speed win.

   Full 1080-file corpus, 20 decode iterations, `-O3 -flto=auto`: the default
   selector chose modes `0,1,2,3 = 0,2,1078,0`, preserving the corpus size at
   1,037,449 bytes and measuring 41.46 MiB/s stateless, 48.06 MiB/s reusable.
   A forced mode-3 candidate chose modes `0,1,2,3 = 0,2,0,1078`, grew the
   corpus to 1,058,531 bytes (+21,082 bytes, +2.0%), and decoded at 38.42 MiB/s
   stateless, 44.20 MiB/s reusable. Conclusion: naive round-robin interleaving
   does not expose useful ILP in this model; dependency and branch overhead in
   the shared grammar traversal still dominates. Revisit only with a semantic
   split that removes work from the hot stream, or with a byte/residual backend
   as in item 11.

10. **Cheaper bit-enum coding.** `CodeBitEnum` decodes one adaptive bit per
    single-bit enumerant of the kind (ImageOperands has ~20). A "zero mask"
    flag first (masks are usually 0 or a tiny common value), or rank-coding
    recent masks as done for IDs, cuts both bits and time with likely zero
    ratio loss — check `ARIXV_STATS` for how much this category matters.

## Tier C — bigger swings (several-X more, real tradeoffs)

11. **Split the design: structural transform → residual stream →
    off-the-shelf back end.** The most valuable knowledge in ARIX-V is the
    grammar-driven prediction (sequential result IDs, rank-coded refs,
    grammar-consistent word counts). Emit those decisions as a compact
    residual *byte* stream and compress it with zstd (or a static-FSE coder).
    Decode would be a fast back end followed by reconstruction with no
    adaptive modeling — essentially "smol-v with a much smarter transform."
    The raw experiment below measures that reconstruction cost directly; it
    shows that the current state machine is substantially more expensive than
    originally assumed, so this architecture alone does not close the gap.

    **[Raw residual ceiling prototype, 2026-07-11.]** The internal benchmark
    now has `--raw-residual`, which runs the same grammar traversal, structural
    predictions, recency lists, ID/literal deltas, operand cursor, and tracker,
    but replaces adaptive range coding with packed fixed-width decisions and
    ULEB128 residuals. It is deliberately not an ARX1 container mode and has no
    external dependency. Adaptive probability contexts are neither updated nor
    reset on this path, so the result measures the retained structural machine
    rather than a no-arithmetic variant of the old bit-model bookkeeping.

    Matched 1078-file word-aligned SPIR-V intersection, core 0 of the same
    Ryzen 9 5950X sandbox, `-O3 -flto=auto`, 100 iterations per file:

    - reusable M2: 52.53, 52.63, 52.59 MiB/s (median **52.59**);
      raw residual: 114.08, 115.58, 114.60 MiB/s (median **114.60**, 2.18x);
    - stateless M2: **46.89 MiB/s**; raw residual: **106.99 MiB/s** (2.28x);
    - 6,061,452 input bytes became 1,040,929 M2 container bytes (0.1717)
      versus 1,945,645 raw payload bytes (0.3210, 1.87x the M2 size). Every
      decode produced the same checksum, 178,987,622,100.

    This puts the current single-stream structural state machine's practical
    speed of light at roughly **115 MiB/s** before adding a back end. A zstd,
    LZ4, Huffman, or FSE stage over this exact transform can improve its size,
    but cannot make end-to-end decode exceed that ceiling; it will normally be
    somewhat slower. Reaching several hundred MiB/s therefore also requires a
    materially cheaper reconstruction pass (direct output, fewer generic
    cursor/tracker operations, or a simpler transform), not just a new entropy
    codec.

### Raw residual reconstruction TODO

These are the next experiments for raising the roughly 115 MiB/s structural
ceiling. Keep the raw residual representation fixed while testing them so that
the result measures reconstruction work, not a simultaneous format change.
For every item, use the matched 1078-file intersection, reusable and stateless
decoders, 100 iterations per file, core pinning, output checksum verification,
and before/after instruction or sampling profiles where available.

- [x] **Emit common decoded words directly instead of staging them in
  `coded_words`.** Most fields produce exactly one word, but the decoder
  currently clears a `WordBuffer`, pushes the word into it, clamps its size,
  copies it into `inst_words`, and then emits it to the output. Add a
  decode-only single-word path that performs the field decode, appends the
  result to the instruction state and output, and advances the cursor without
  constructing `coded_words`. Retain the buffer for strings, multiword
  context-dependent numbers, and malformed layouts that require span
  clamping. Measure the common-path coverage as well as throughput; this is
  successful only if the extra branching does not erase the saved staging
  work.

- [x] **Pre-size the output and store words through a direct pointer (+12.5%
  reusable, +11.1% stateless raw-residual decode achieved, 2026-07-12).** The
  expected output size is already validated before module decode, yet
  `DecodeIO` emits a word through four `vector<uint8_t>::push_back` calls.
  Landed as one `resize(expected_original_size)` before `CodeModule`, a
  bounded write position, and direct four-byte stores. Big-endian
  reconstruction and delayed bound patching remain intact; incomplete or
  overflowed writes cause the existing error path to clear the output.
  Matched 1078-file word-aligned corpus, core 0, `-O3 -flto=auto`, 100
  iterations per file, identical 1,945,645-byte raw payload and checksum
  178,987,622,100: reusable 123.44, 123.72, 124.03 MiB/s (median **123.72**)
  → 138.72, 139.21, 139.23 (median **139.21**); stateless 109.26, 109.48,
  109.69 (median **109.48**) → 121.18, 121.65, 122.59 (median **121.65**).
  This is measured separately from the already-landed direct-word-staging
  path.

- **[Reverted experiment, 2026-07-12] Generate direct layouts for more
  instructions and reduce `OperandCursor` traffic.** A generated fixed-prefix
  + repeated-ID descriptor covered 37,337 corpus instructions / 178,670
  operand words (63 instruction forms) while preserving byte-identical raw
  payloads and M2 size. On the matched 1078-file raw-residual corpus,
  reusable decode improved 138.37 → 139.82 MiB/s (+1.0%), but stateless
  decode was neutral within noise (122.06 → 121.49 MiB/s median). Reverted:
  the small reusable-only gain did not justify the added grammar metadata,
  generator rules, hot-path branch, and exclusions for instruction-selected
  layouts. Revisit only as part of a broader layout-specialization design.

- **[Reverted experiment, 2026-07-12] Dense generation-stamped ID table with
  sparse fallback.** A prototype used a direct-indexed prefix sized from the
  clamped module word count (the SPIR-V bound is decoded only at the end) and
  kept the existing hash map for large/hostile IDs. The raw payload and output
  stayed byte-identical, but the matched 1078-file raw-residual corpus slowed:
  reusable median 138.02 → 134.59 MiB/s (−2.5%); stateless 120.96 → 117.65
  MiB/s (−2.7%). M2 throughput was within noise. At the corpus high-water
  mark it would retain ~1.22 MiB of dense values+stamps versus ~1.50 MiB for
  the sparse arrays, only a ~0.28 MiB saving. Reverted: reduced hashing did
  not offset the denser table's cache footprint. Revisit only with paging or
  an early-bound format.

- [ ] **Build a reconstruction-specific loop instead of routing raw residuals
  through the generic adaptive `CodeModule` template.** Once the smaller
  experiments establish which shared abstractions remain expensive, create a
  decoder specialized for fixed-width/ULEB residual input. It should preserve
  the same grammar decisions, recency predictions, tracker semantics, hostile
  input behavior, and byte-exact output, while deleting probability-model
  parameters, stats hooks, encoder-only branches, and adaptive-format
  scaffolding from the hot loop. Treat this as the largest and riskiest item:
  require differential tests against the shared implementation over the full
  corpus, mutated modules, truncations, trailing data, and sanitizer runs.

12. **Chunked parallel decode.** Reset (or fork) model state every N
    instructions and frame the payload so chunks decode independently →
    thread-parallel decode and better latency on big modules. Ratio cost =
    model cold-start per chunk; measurable today by encoding files split at
    chunk boundaries.

13. **Hybrid mode flag per module.** Keep the current max-ratio mode as `M2`
    and add a fast mode (`M3`: items 7-9 or 11) chosen at encode time —
    tooling picks per asset (huge shader packs get fast mode, tiny embedded
    shaders keep max ratio). The container already has a mode byte, so this
    stages cheaply.

## Experiment backlog (2026-11-17)

Starting point: persistent decode ~57 MiB/s versus the ~139 MiB/s
raw-residual ceiling, so roughly 60% of decode time is entropy coding plus
model work. Items are labelled `E*` to avoid clashing with the tier numbering
above; none repeats an experiment already measured in this file. Suggested
order: E0 → E1-E3 → E4 → E6-E8 → E-A0 → E-A2 (E-A1 only if E-A0
surprises) → E11.

**First pass (2026-11-17): E0, E1, E6 and a new E14 landed — reusable
55.9 → 64.3 MiB/s (+15.0%), stateless 48.7 → 54.6 MiB/s (+12.2%), corpus
1,041,769 → 1,031,921 encoded bytes (−0.95%).** Method: full 1080-file
corpus, `taskset -c 2`, `-O3 -flto=auto`, 30 iterations, 7 interleaved A/B
rounds, medians (the host was shared with a heavy background job, so
absolute MiB/s are lower than earlier sections; every A/B round agreed in
sign). Decode instruction counts from callgrind on every 4th corpus file,
3 iterations, reusable decoder, collection toggled on `Decoder::Decode`:
1,597.3M → 1,513.0M. `make test`, `make asan`, and a 1000-iteration fuzz
run of the `-DARIXV_NO_SIMD` scalar build pass; the scalar and SSE2 builds
produce identical bytes and checksums.

### Measure first

- [x] **E0. Decisions per call site.** `ARIXV_STATS` now also prints adaptive
  decisions per field category, split by coding primitive (plain flag, bit
  tree, uint length, uint mantissa, byte), plus direct bits. Counted at
  encode time, which makes exactly the decoder's decisions. Full corpus
  (before E6): **15.46M adaptive decisions = 2.55 per output byte, and zero
  direct bits.** Largest buckets: opcode bit tree 3.52M (23%, the 10-bit
  index tree for ~352K instructions), id_ref 4.25M (bit tree 1.84M, uint
  1.94M), result_id 1.69M, string bytes 1.42M, result_type 1.40M (bit tree
  1.10M). By primitive: bit trees 7.26M, uint length 2.59M, uint mantissa
  2.01M, plain flags 2.15M, bytes 1.46M.

### Cost of each decision (no format change)

- [x] **E1. Keep coder state in registers (+4.2% reusable, byte-identical).**
  Confirmed in the disassembly: `DecodeBit` loaded `range_`/`code_` from
  `*this` and stored them back on every bit, putting a store→reload on the
  serial chain. Landed as `RangeDecoder::DecodeTree<kBits>` and
  `DecodeSequence`, which copy the state into locals for a whole bit-tree
  walk or uint mantissa run and write it back once; `BitTree::CodeValue` and
  `CodeUint` use them on the decoder path. That covers ~86% of decisions;
  plain flags still go through `DecodeBit`. Instructions 1,597M → 1,564M
  (−2.1%), wall time +4.2% (all 7 rounds faster).
- [ ] **E2. Cheaper renormalization.** *Deprioritized after E0:* the
  payload is ~0.53 bits per decision, so a renormalization byte is read only
  about once per 15 decisions, and the per-bit cost is one predictable
  compare. Replace the byte-at-a-time `while`
  loop (with a per-byte bounds check in `ReadByte`) with a 64-bit
  `code`/`range` that refills 32 bits at once, and pad the input tail so the
  hot path has no exhausted-input check; detect overrun once at the end.
- [ ] **E3. Batch direct bits.** *Moot after E0:* M2 makes no direct-bit
  decisions on the corpus. `CodeBitsDirect` loops one bit at a time
  through `DecodeBitDirect` (8/10/16/32-bit fields, every byte of
  `CodeUint32`). Decode k bits in one step (`range >>= k`, then one
  compare/divide).
- [ ] **E4. PGO and compiler comparison.** `-fprofile-generate/-fprofile-use`
  trained on the corpus, and clang vs GCC. Branchy interpreter-style code
  often gains 5-15% from PGO; worth knowing even if the source-integration
  story can't ship it.
- [ ] **E5. Cache misses.** `perf stat` with L1d/L2 miss counters. The inline
  model plus its trees is near or above the 32 KB L1; if misses are
  significant, pack the hot contexts (opcode, rank slots, uint length)
  together.

### Fewer serial decisions (format change, ratio-neutral or close)

- [x] **E6. Right-size the rank trees (+2.1% reusable, −0.95% size).** The
  original description was wrong: unary `CodeRank` is unused, and ranks are
  coded with `BitTree<6>`. But the ID, type, and forward-ref recency lists
  are capped at 16 entries, so the top two of the six levels were always 0:
  two wasted serial decisions per ranked ID, and not free in size either.
  Landed as `kRankTreeBits = 4` (with a `static_assert` that it covers every
  list cap) for `id_rank`, `type_rank`, and `result_fwd_rank`;
  `string_rank` keeps 6 bits because the string list holds 64 entries.
  Corpus 1,041,769 → 1,031,921 bytes; instructions −2.8%; wall time
  +2.1%.
- [ ] **E7. Shorter uint ladder.** A 20-bit value costs 6 length bits plus 19
  adaptive mantissa bits. Code only `kUintHighBits` adaptively and send the
  rest as bypass bits (E8); measure the ratio cost.
- [ ] **E8. Separate stream for bypass bits.** *Only relevant together with
  E7:* M2 has no bypass bits today (E0). Route direct bits, uint rest
  bits and unknown opcode/literal bytes through a plain bit reader instead of
  the arithmetic coder. It doesn't wait on coder state, so it shortens the
  serial chain without the stream interleaving that already failed (item 9).
- [ ] **E9. Cheaper bit-enum coding.** Item 10 above: code "mask is zero /
  same as last" before the per-bit loop, if E0 shows it is large.
- [ ] **E10. Adopt `static-opcode-tables`.** *Dropped from this
  investigation (2026-11-17): the project is keeping its no-corpus-trained-
  tables rule, and after E-A2 the opcode costs 3 symbols rather than 10
  binary decisions, so the speed case is mostly gone.* Already measured at −5% size and
  +13-16% decode; a decision about the "no corpus tables" rule, not an
  experiment.

### ANS

- [x] **E-A0. Microbenchmark before porting — binary rANS is not faster
  (2026-11-17).** `build/ans_microbench` (`tools/bench/ans_microbench.cpp`,
  built with `-DARIXV_TRACE_DECISIONS`) records every adaptive decision of a
  real corpus encode as (context, bit) and replays it with fresh contexts per
  module through the range coder (state in locals, as E1), adaptive binary
  rANS (32-bit state, 16-bit renormalization), and 2-state interleaved rANS.
  "dep" adds a true data dependency from each decoded bit to the next
  context index. Full corpus, 14.46M decisions: sizes range 1,027,258,
  rANS −0.17%, rANS×2 +0.14%. Per decision: range 5.03 (indep) / 5.57 (dep)
  ns, rANS 5.57 / 5.85, rANS×2 5.62 / 5.92 — rANS 5-10% *slower*, and
  interleaving doesn't help. A per-decision cost is dominated by instruction
  throughput (probability update, renormalization), not by the coder's own
  dependency chain. Original plan text: Decode a synthetic bit stream
  with realistic probabilities using the LZMA-style coder vs adaptive binary
  rANS (32-bit state with 16-bit renorm, or 64/32). Bounds the gain before
  touching the model. Item 9 found the model's dependency chain, not the
  coder, to be the limit, so expect a binary-for-binary swap to be worth
  ~5-15% of the coding share.
- [ ] **E-A1. Adaptive binary rANS swap.** *Not worth doing on E-A0's
  numbers; any rANS gain has to come from multi-symbol decode (E-A2).* Same `Prob` contexts. The encoder
  records `(prob, bit)` pairs in the forward pass and encodes them in reverse
  (fine for offline compression); the decoder still adapts as it goes. Gains:
  very cheap renormalization and 2-state interleaving inside one stream
  (alternate states, not streams), simpler than the reverted 4-stream
  framing. Costs: a more complex encoder and a possible small ratio change.
- [x] **E-A2. Adaptive multi-symbol rANS for small alphabets — landed
  (2026-11-17): reusable 64.5 → 72.7 MiB/s (+12.7%), stateless 55.2 → 64.1
  MiB/s (+16.1%), corpus 1,031,921 → 1,035,489 bytes (+0.35%), container
  magic `ARX1` → `ARX2`.** Matched 1080-file corpus on a quiet host (the
  background job was stopped), core 2, `-O3 -flto=auto`, 30 iterations, 7
  interleaved A/B rounds, medians; every round agreed. Cumulative since the
  backlog was written: reusable 55.5 → 72.7 (+31%), stateless 49.1 → 64.3
  (+31%). Adaptive decisions per corpus encode 14.46M → 7.13M (1.18 per
  output byte).

  *What landed:* every `BitTree<kBits>` is now symbol-coded
  (`src/symbol_coder.h`): MSB-first ≤4-bit chunks, each a 32-byte adaptive
  CDF (`SymbolCdf`) chosen by the value prefix above it, on an rANS stream
  beside the binary range coder. Design points:
  - *Zero means uniform.* Tables store offsets from the uniform CDF and keep
    the warm-up count in lane 0 (C[0] is always 0), so value-initialized
    `BitTree{}` resets, stamped lazy resets, and pooled trees are unchanged.
  - *Two-ended payload, no length field.* Range bytes run forward from the
    front; rANS words (LE16) run backward from the end; a stream that coded
    no symbols emits nothing. Validation: the two must tile the payload
    exactly and the rANS state must finish at `kRansL`. Exhaustion of
    either stream still stops decoding early (bounded output on hostile
    headers); only a forward range-stream overrun reports `kTruncated`, a
    mis-tiled payload (trailing bytes, rANS overrun) reports
    `kBadContainer`.
  - *Shadow training* (`TrainBitTree` → `BitTree::Train`) applies a
    steady-rate CDF update along the value's path.
  - *Per-tree adaptation cap:* `kSymbolRateCap = 5` (best for the corpus),
    but byte trees (`ByteTree`: string, opaque, and byte-fallback bytes) use
    `kByteSymbolRateCap = 7`. With one global cap 5, forced-coded random
    bytes expanded +6.67% (was +2.49% with binary trees), breaking the test
    bound; byte cap 6/7/8 gives +4.32/+3.57/+3.33% for +104/+146/+164 corpus
    bytes.
  - SSE2 lookup/update with a portable scalar fallback; the
    `-DARIXV_NO_SIMD` build produces byte-identical blobs and cross-decodes
    (54-file check).
  - The size cost is almost entirely the 4-byte rANS final state per module
    (~4.3 KB over 1080 modules), which the microbench's tree-only accounting
    did not charge.
  - **[Recovered, 2026-11-17] 3 bytes per module from the range coder:
    1,035,489 → 1,032,249 bytes, now +328 bytes (+0.03%) over the pre-rANS
    1,031,921.** (a) LZMA's leading byte lies above the initial 32-bit
    window and is always 0, so it is no longer stored or read. (b) Short
    flush: after the final normalization range ≥ 2^24, so the interval
    contains `low` rounded up to a multiple of 2^16 with every low-16-bit
    suffix; the encoder emits 3 flush bytes instead of 5, and the decoder's
    last `kRangeTailSlack = 2` reads are don't-care (the start of the rANS
    words, or virtual zeros past the payload end; reads beyond that still
    mark the stream exhausted). The tiling check discounts the slack. Kept
    the unreleased `ARX2` magic and regenerated its golden vector. Speed
    neutral within noise. *Not pursued for the rANS state:* range-coding
    it buys ~4.5 bits (the state is ~log-uniform); byte-wise renormalization
    with a 3-byte state keeps x close to the 2^15 scale and costs coding
    efficiency on every symbol; hiding 16 bits of payload in the encoder's
    initial state needs data the decoder can defer past the last symbol,
    and the range stream's tail cannot be deferred.
  `make test`, `make asan` (clean), a 3000-iteration fuzz run, and a
  1000-iteration scalar-build fuzz run pass.

  *Microbench record (2026-11-17), before the port:* `build/ans_symbol_microbench` records every `BitTree`
  value (tree, depth, value) of a real corpus encode and replays the tree
  events. Each tree is split MSB-first into ≤4-bit chunks, each chunk an
  adaptive 2-16-symbol CDF chosen by the prefix above it: the bit tree's
  context structure with fewer, wider nodes. CDF: 15-bit total, stored in
  coding domain with a +1-per-symbol floor and a 32768 sentinel; SSE2 search
  (`subs_epu16` compare, `ctz(~mask)` — *not* `popcount`, which is a libgcc
  call without `-mpopcnt`) and SSE2 update toward 0 / top with a
  count-warmed shift capped at `--rate-cap` (5 best: 4 is +0.03%, 6 is
  +0.21% with rANS).
  - **Coverage is larger than E0's `bit_tree` column:** uint lengths and
    bytes are bit trees too, so trees are **10.3M of 14.5M decisions
    (71%)**: depth 4 ×511K events (rank trees), 6 ×437K (uint length),
    8 ×264K (bytes, enum indices), 10 ×352K (opcode index).
  - **Size:** binary 672,344 bytes of tree payload; nibble range-coded
    +0.02%, nibble rANS −0.30%. No ratio cost.
  - **Speed** (callgrind instructions, every 8th file, net of context
    reset): binary ~40 per decision ≈ 137 per nibble-equivalent; nibble
    range coder (one 32-bit division per symbol) ≈ 1.25x faster by wall
    time; nibble rANS ~110 per symbol; nibble rANS with compile-time chunk
    walks ("tight") **~83 per symbol, 1.65x fewer**. Reset-adjusted wall
    time agrees (~36 → ~21 ns per tree event, ~1.7x), but treat wall
    numbers as ±15%: the host was saturated by an unrelated job.
  - **Projection:** coding is ~60% of decode time and trees ~71% of
    decisions, so ~43% of decode; 1.65x there is **~1.2x end to end**.
  - **Design questions settled by the port:** two-stream layout chosen;
    shadow training ported; E10 overlap and uint mantissas still open.
    Originally: (1) stream layout — rANS
    for symbols plus the range coder for the remaining binary flags as two
    payloads (encoder buffers symbols and rANS-encodes them in reverse at
    the end), versus all-rANS (E-A0 says binary rANS is 5-10% slower);
    (2) an equivalent of `TrainBitTree` shadow training for opcode CDFs;
    (3) overlap with E10 — the opcode tree is a third of all tree
    decisions and `static-opcode-tables` replaces it; (4) the uint
    mantissa runs (~2M decisions) could become nibble symbols too.
  Original plan text: The LZNA
  approach: adaptive 16-symbol (nibble) frequency tables with a SIMD update.
  With a power-of-two total, rANS gets the slot as `x & mask` and searches
  the cumulative frequencies with no division, which an adaptive
  multi-symbol range coder can't do as cheaply. Candidate sites: rank
  (9 symbols), uint length (33, as 2 nibble steps), opcode index (3 nibble
  steps instead of 10 bits), string/byte models (2 nibbles instead of 8
  bits). Cuts the serial chain 2-4x at those sites, with no per-file tables
  (unlike reverted item 8). Ratio may move either way: frequency-count
  adaptation behaves differently from per-bit shift updates.
- [ ] **E-A3. Static tANS/FSE back end for the residual stream** (item 11).
  Only worth doing with the dedicated reconstruction loop, since structure
  work caps it at ~139 MiB/s.

### Larger structural ideas

- [ ] **E11. Dedicated reconstruction loop** (raw residual TODO above).
  *Sizing measurements (2026-11-17) are under "Third pass" below; they argue
  for targeted structural fixes rather than a full rewrite.* Also
  benefits M2 if the generic `CodeModule` template is what keeps coder state
  out of registers (E1).
- [ ] **E12. Decode 2-4 modules at once in one thread.** Independent
  dependency chains give the CPU parallel work with no format change; fits
  only a batch API (shader packs). IPC is ~2.6 on a core that can do ~5, so
  maybe ~1.3x.
- [ ] **E13. Chunked parallel decode** (item 12). Cuts latency on large
  modules; does not improve single-core throughput.

### Found while profiling

- [x] **E14. SSE2 recency lists (+9.9% reusable, byte-identical).** After E1
  and E6, `RecencyList::Touch` was 7.9% of decode instructions: an early-exit
  scan plus a libc `memmove` call on every ID use. Landed as a fixed-width
  match mask (four `pcmpeqd` + packs + `movemask`, masked to `size_`, then
  `ctz`) and a branch-free move-to-front (lane-shift the 16 entries, blend
  lanes `<= last`). Guarded by `__SSE2__` (baseline on x86-64) with a
  portable fixed-width scalar fallback (`-DARIXV_NO_SIMD`) that produces
  the same output. The scalar fallback alone *raised* instructions 14%
  because GCC didn't vectorize it, for only ~+1.6% wall time; the explicit
  SSE2 path is 58.3 → 64.1 MiB/s.

### Third pass (2026-11-17, quiet host)

- **Force-inline the symbol decode path: kept, neutral.** `DecodeSymbol`
  and friends were out-of-line calls from every tree walk. With
  `ARIXV_FORCE_INLINE`: instructions −1.3%, wall time +0.1% reusable /
  +0.8% stateless (noise). A follow-up that read `C[s]`/`C[s+1]` straight
  from the table instead of a staged 17-entry stack copy was also neutral
  (+0.6% / −0.4%) and was reverted.
- **[Measured dead end] Symbol-coded uint mantissas.** On the corpus, 76% of
  mantissa bits fall in the first `kUintHighBits = 8` below the leading one
  (1.52M of 2.01M; uint bit lengths are mostly 2-9). Prototype: that region
  coded as fixed-width chunks, each `(bit length, chunk index)` with its own
  CDF (the binary model's context structure); deeper bits stay binary.
  | chunk | bytes | Δ size | reusable | stateless |
  |---|---|---|---|---|
  | 4 bits | 1,035,005 | +0.27% | +1.7% | +1.5% |
  | 3 bits | 1,032,857 | +0.06% | +0.0% | −0.6% |
  | 2 bits | 1,032,093 | −0.02% | −4.3% | −7.2% |
  Rate caps for 4-bit chunks: 3 → 1,041,887, 4 → 1,035,597, 5 → 1,035,005,
  6 → 1,035,995, 7 → 1,036,861, 8 → 1,037,297 (mantissas want fast
  adaptation; the size loss is the learning cost of 15 CDF parameters
  versus 4 per-bit probabilities in sparse contexts). The speed case
  failed because E1 already made the binary mantissa run a tight
  register-held loop with no context dependence between its bits, so a
  symbol costs about as much as the 2-3 bits it replaces; narrower chunks
  also grow every `UintModel`, which stateless decode pays to zero.
  Reverted. (The earlier +5-8% estimate for this was wrong.)
- **E11 step 1: the structural ceiling rose to 164.5 MiB/s reusable /
  143.7 stateless** (was 139.2 / 121.7 before E1/E6/E14). Matched 1078-file
  word-aligned set, core 2, 30 iterations, median of 3: M2 72.8 / 63.7
  MiB/s, raw residual 164.5 / 143.7 MiB/s, identical checksums; raw payload
  now 1,818,396 bytes (4-bit rank trees). Per MiB, M2 spends 13.7 ms, of
  which the raw-residual walk is 6.1 ms: structure (including raw
  bit-unpacking) is **at most ~44% of decode time**, entropy coding plus
  model work still ~56%. Even an infinitely fast structural pass would
  only reach ~1.8x.
- **E11 step 2: line-level profile** (callgrind, `-O3 -flto=auto -g`,
  every 4th file, 3 iterations, reusable). By function (self, inlined code
  attributed to its caller):
  | function | share |
  |---|---|
  | `CodeModule` body | 27.0% |
  | `CodeUint` | 12.0% |
  | `BitTree<4>` walk (rank trees) | 11.5% |
  | `RecencyList::Touch` | 10.1% |
  | `RangeDecoder::DecodeBit` (flags) | 6.9% |
  | `BitTree<10>` walk (opcode) | 5.8% |
  | `OperandCursor::Consume` + `Next` | 8.8% |
  | `Tracker::Ensure` + `OnInstructionEnd` + `OnIdUse` | 7.6% |
  | `CodeByte` + `BitTree<8>` | 3.7% |
  Inside the 27% `CodeModule` self cost: 15.7% is `model.cpp` code spread
  across hundreds of lines (no line above 1.1%: operand dispatch, the
  fixed-layout path, stamp checks, `WordBuffer`/`DecodeIO::Append`
  bounds checks), 4.3% is inlined grammar-table lookups, and the rest is
  inlined range/symbol coding. Lazy-reset work is visible but small
  (order-1 opcode tree reset 0.64%, count-model `EnsureStamped` + reset
  ~1.7%). Hottest single coder lines: the symbol rate computation
  (`clz` on the count) 2.1%, rANS state update 1.3%, range `bound` 1.2% +
  1.0%.
  **Reading:** the structural cost is concentrated in identifiable
  subsystems (recency lists ~10%, operand cursor ~9-11%, tracker ~8%,
  grammar lookups ~4%), and the generic `CodeModule` glue itself is
  diffuse. That matches the earlier reverted direct-layout (+1%) and
  dense-ID (−2.5%) experiments. A wholesale specialized loop would mostly
  re-home that glue; the better next E11 steps are targeted: recency-list
  update rules (a format/ratio trade, ~10% at stake), the cursor's
  variable-layout path, and tracker `Ensure`/`OnInstructionEnd`.

### Recency-list update rules (2026-11-17)

Variants were compared with load-independent metrics (full-corpus encoded
bytes and callgrind decode instructions: reusable decoder, every 4th file,
3 iterations) while the host was busy; the landed rule set was timed
afterwards (below).

| variant | bytes | Δ size | decode Ir | Δ Ir |
|---|---|---|---|---|
| baseline (c670473) | 1,032,249 | — | 1,389.9M | — |
| R1: no recency touch in `OnResultId` | 1,030,875 | −0.13% | 1,366.0M | −1.7% |
| R2: result-type uses skip `recent_ids` | 1,026,842 | −0.52% | 1,357.8M | −2.3% |
| **R1 + R2 (landed)** | **1,021,506** | **−1.04%** | **1,330.6M** | **−4.3%** |
| R1 + R2 + R5: new types skip `recent_ids` | 1,029,088 | −0.31% | — | — |
| history ring ×16, R1 + R2 | 1,046,060 | +1.34% | 1,274.1M | −8.3% |
| history ring ×32 (5-bit ranks), R1 + R2 | 1,036,457 | +0.41% | — | — |

- **R1:** result IDs were touched twice: at coding time and again in
  `OnInstructionEnd`. An instruction's later operands don't reference its
  own result, so the first touch only shifted their ranks by one.
- **R2:** result types are coded from `recent_types()`, but `OnIdUse` also
  pushed them onto `recent_ids()`, crowding out the values that ID operands
  actually reference. Now `Tracker::OnResultTypeUse` touches only the types
  list (plus forward refs).
- The two compound: removing both is worth more than the sum of each alone.
  `RecencyList::Touch` dropped 139.6M → 95.2M instructions (7.2% of decode).
- **Rejected:** R5 (type definitions skip `recent_ids()`) — ID operands of
  type declarations (`OpTypePointer`, struct members, …) reference types
  through that list. A history ring (rank = touches ago, O(1) touch, the
  decoder only indexes) saves another ~4% of instructions but costs
  +1.3-2.4% size, since duplicates crowd useful IDs out of the slots.
- **Wall time (measured later on a mostly quiet host, core 2 and its SMT
  sibling idle, full 1080-file corpus, 30 iterations, interleaved A/B
  medians): reusable 72.1 → 74.1 MiB/s (+2.8%, 7 rounds), stateless
  63.6 → 64.6 MiB/s (+1.7%, 9 rounds); every round agreed in sign.**
- Format change within the unreleased `ARX2` (the golden vector has no IDs
  and is unchanged). `make test`, `make asan`, 3000-iteration fuzz, and the
  scalar/SIMD cross-check pass.

### Operand cursor and opcode-context lookup (2026-11-17)

Byte-identical changes, developed with load-independent metrics:
callgrind decode instructions (reusable decoder, every 4th file, 3
iterations) and output identity.

| change | decode Ir | Δ |
|---|---|---|
| baseline (12ff914) | 1,330.6M | — |
| cache the previous instruction's table index | 1,276.5M | −4.1% |
| + inline cursor fast paths | 1,195.9M | −6.3% (−10.1% total) |

- **Previous-opcode key.** The order-1 opcode context (`PreviousOpcodeKey`)
  binary-searched the core instruction table by the previous opcode on
  every instruction: ~3.8% of decode, visible in the E11 profile as
  "inlined grammar-table lookups". The tracker now remembers the previous
  instruction's index (`OnInstructionEnd` takes it; unknown opcodes keep
  the sentinel), so the key is a field read.
- **Cursor fast paths.** `Next()` averaged ~53 instructions per call and
  `Consume()` ~70, as out-of-line calls on every variable-layout field.
  Both are now forced-inline wrappers handling the common case — `Next()`:
  a plain top-level slot with words left, no pending enumerant parameters,
  no composite; `Consume()`: a plain top-level operand whose value does not
  steer the walk (not a value/bit enum, ext-inst number, or spec-op
  opcode) — and fall back to the unchanged `NextSlow()`/`ConsumeSlow()`,
  leaving identical state. Out-of-line cursor cost fell from ~9.5% to
  ~1.5% of decode instructions.
- **Wall time (under moderate load: load average ~6-8, another process
  using ~4 cores and migrating; core 2 and its sibling mostly free; full
  corpus, 30 iterations, 9 interleaved A/B rounds):** reusable medians
  59.9 → 67.6 MiB/s (+12.7%), stateless 52.5 → 57.8 MiB/s (+10.3%); B
  beat A in 17 of 18 rounds, and the median per-round ratio was 1.14
  reusable / 1.11 stateless. The load depressed absolute speeds (the
  quiet-host baseline is ~74 MiB/s reusable) and drifted mid-run, so treat
  the size of the gain as ±3% until re-timed on a quiet host.
- **Correctness:** M2, stateless, and raw-residual outputs are byte-identical
  to 12ff914 (1,021,506 / 1,805,240 bytes, same checksums). `make test`
  (including `operand_cursor_test`), `make asan`, a 3000-iteration fuzz,
  and a differential decode of 720 mutated/truncated/padded blobs (old vs
  new CLI: identical exit status and output in every case) pass.

### Tracker ID map (2026-11-17)

Byte-identical changes, developed with load-independent metrics:
callgrind decode instructions (every 4th file, 3 iterations) for both
decoder modes, plus output identity.

| change | reusable Ir | Δ | stateless Ir | Δ |
|---|---|---|---|---|
| baseline (44f9497) | 1,195.9M | — | 1,358.3M | — |
| uninitialized ID-map arrays + single result-ID insert | 1,176.0M | −1.7% | 1,316.7M | −3.1% |
| (+ inline `Ensure` hit path — reverted) | 1,180.4M | +0.4% | 1,321.1M | +0.3% |

- Profile at 44f9497: `Tracker::Ensure` ~1.55M calls per sample run
  (615K ID-operand uses, 245K result-type uses, 306K `OnResultId`, 383K
  `OnInstructionEnd`) at ~30 instructions each; `IdMap::Rehash` 1.1% in
  only 274 calls; `OnInstructionEnd`'s own lines are diffuse — its weight
  is the recency `Touch` (2.0%) and `Ensure` (0.7%) it calls.
- **Uninitialized slot arrays.** `Rehash` value-initialized three parallel
  arrays (24 bytes per slot) although a slot's key and value are written
  when it is claimed and occupancy is decided by the generation stamp
  alone. Keys and values are now `new T[n]` without initialization; only
  the stamps are zeroed. `IdInfo` became a plain aggregate whose all-zero
  value is the old default (static_assert-checked), always created as
  `IdInfo{}`. Stateless decode, which builds a fresh map per call, gains
  the most.
- **One map insert per result ID.** `OnResultId` inserted the result and
  `OnInstructionEnd` looked it up again. Every reader treats a default
  entry and a missing one the same (`TypeWidth` → 0, ext-set lookup →
  null, `OnIdUse`/`Ensure` create the default), so `OnResultId` now only
  updates `max_seen_id_` and the insert happens once, at instruction end.
- **Rejected:** force-inlining the `Ensure` hit path and the
  `Tracker::Ensure` wrapper added instructions — call overhead was not the
  cost.
- **Wall time (idle host, core 2, full corpus, 30 iterations, 9
  interleaved A/B rounds, medians): reusable 82.3 → 83.4 MiB/s (+1.4%;
  faster in 7 of 9 rounds, the other two within 0.1 MiB/s), stateless
  71.3 → 73.0 MiB/s (+2.4%, all 9 rounds).** The same run re-timed the
  cursor commit (44f9497) at 82.3 MiB/s reusable on an idle host, against
  74.1 MiB/s for 12ff914 in the earlier quiet run: the cursor gain measured
  under load (+12.7%) holds at about +11%.
- **Correctness:** M2, stateless, and raw-residual outputs byte-identical to
  44f9497 (same bytes and checksums); `make test`, `make asan`,
  3000-iteration fuzz, the 720-case mutated-blob differential against the
  previous CLI (0 mismatches), and valgrind memcheck (corpus decodes in
  both modes plus 40 mutated-blob decodes: 0 errors, so nothing reads an
  unclaimed slot) pass.

### Re-profile after the structural passes (2026-11-17, at 76e5823)

Decode instructions by subsystem (callgrind, `-O3 -flto=auto -g`, every
4th file, 3 iterations; inlined code attributed to its source):

| subsystem | reusable | stateless |
|---|---|---|
| entropy coding (rANS symbols, range-coded flags/mantissas, prob updates) | 49.3% | 44.3% |
| `model.cpp` field coders + `CodeModule` glue | 18.0% | 14.4% |
| recency lists | 8.1% | 7.2% |
| operand cursor | 7.8% | 7.0% |
| tracker ID map + instruction end | 6.2% | 5.6% |
| allocation / zeroing (malloc, free, memset) | 3.4% | 12.8% |
| std library inlines | 4.2% | 5.5% |

Stateless decode additionally spends ~2.9% in `ModelState` construction and
destruction, so ~15% of its instructions are per-call model setup and
teardown that the reusable decoder avoids. The adaptation-rate
bookkeeping (count, `clz`, saturate) is ~3.6% for symbol CDFs and ~3% for
binary probabilities.

### [Measured dead end] Folding binary flags into adjacent symbols

Remaining binary flags are ~2.15M of 7.13M decisions, most directly in
front of a symbol they gate. Prototype for the largest group (ID operands
and result types, ~670K flags): the "in recency list" flag and the 4-bit
rank tree share contexts, so they were folded into one 16-symbol table
(ranks 0-14 plus an escape to the delta coder; rank 15 escapes).

- Size: 1,021,506 → 1,023,435 bytes (+0.19%).
- Decode instructions: reusable 1,176.0M → 1,167.0M (−0.8%), stateless
  1,316.7M → 1,307.6M (−0.7%): only ~18 instructions saved per folded flag,
  since a range-coded flag is cheap here and a 16-symbol decode costs more
  than the flag it absorbs.
- Wall time (load rose mid-run; quiet rounds only): reusable about +0-1%,
  stateless neutral. Shortening the serial chain did not show up either.
- Bound for the whole idea: even folding all ~2.15M flags would save at
  most ~2.5% of instructions, with a ratio risk at each site. The earlier
  +5-10% estimate assumed a flag costs as much as a full symbol step; it
  costs much less. Reverted; not extended to the result-ID, opcode, or
  word-count flags.

### Stateless model setup/teardown: arena for lazy contexts (2026-11-17)

Byte-identical; developed with load-independent metrics, then timed. Stateless
instructions are now measured per pass as (Ir at 3 iterations − Ir at 1) / 2
over every 4th file, which cancels the benchmark's encode and verification
work and does not depend on symbol names (LTO inlined the free `Decode()`
into the benchmark loop, defeating the callgrind toggle).

- Before: a stateless decode made **220 heap allocations (544 KB)**, mostly
  lazily created contexts — 55.6 order-1 opcode trees (2,208 B), 49.2
  count models (752 B), 45.4 8-bit trees (544 B), 13.6 uint models — each
  freed individually in `~ModelState`. Setup/teardown (malloc, free,
  memset, `ModelState` ctor/dtor) was **39.3M instructions per pass, 12.0%
  of stateless decode**.
- Change: `src/arena.h` adds a bump allocator owned by `ModelState` (and one
  inside `ByteModel`, which mode 1 also uses standalone) plus a trivially
  destructible `ArenaPtr<T>` replacing the `std::unique_ptr` members of
  lazily created contexts. Arena objects must be trivially destructible
  (static_assert); `ValueEnumModel`/`ExtInstNumberModel` now hold their
  always-created `BitTree<8>` inline, and `BitEnumModel` (owns a
  `std::vector`) stays on `std::unique_ptr`. `BeginDecode`'s drops of rare
  contexts became in-place `T{}` resets, so a reusable decoder does not
  abandon arena memory per decode; its high-water pooling is otherwise
  unchanged.
- Result: **220 → 56 `operator new` allocations per decode; stateless
  329.0M → 307.7M instructions per pass (−6.5%)**; setup/teardown 39.3M →
  17.1M (12.0% → 5.6%). Reusable decode unchanged (1,176.0M → 1,175.5M).
  The remainder is mostly `memset` zeroing of freshly created contexts
  (zero is the uniform model) and `ModelState` construction — inherent to
  the current model layout.
- **Wall time (idle host, core 2, full corpus, 30 iterations, 9 interleaved
  A/B rounds, medians): stateless 73.6 → 82.4 MiB/s (+12.0%, all 9
  rounds); reusable 84.3 → 84.1 MiB/s (−0.2%, within run-to-run spread).**
  The wall-clock gain is nearly twice the instruction reduction: allocator
  calls cost more in cache misses and pointer chasing than in instructions.
  Stateless decode is now within ~2% of the reusable decoder (it was
  ~11-15% slower).
- Correctness: M2 (both modes) and raw-residual output byte-identical;
  `make test`, `make asan`, 3000-iteration fuzz, the 720-case mutated-blob
  differential (0 mismatches), and valgrind memcheck with full leak
  checking (both decoder modes and `arixv_test`, which covers the
  byte-fallback model's arena: no errors, all heap blocks freed) pass.

### Reusable decoder removed (2026-11-17)

With the arena, stateless decode runs within ~2% of the reusable
`arixv::Decoder`, which existed only as a workaround for per-call allocation
cost. The library now has a single, stateless decode path:

- **Public API:** `arixv::Decoder` is gone from `arixv.h`.
  `src/arixv_decoder_compat.h` keeps a source-compatible, header-only
  `arixv::Decoder` (same constructor, move, and `Decode(data, size, out,
  max_output)` signatures) that forwards to stateless `Decode()`; it holds
  no state, so the old thread-safety and memory-retention caveats no longer
  apply. `arixv_test` checks the shim against `Decode()` over byte-fallback
  payloads, an error path, `max_output`, moves, and corpus modules.
- **Internals deleted:** `ModuleDecoder` / `RawResidualModuleDecoder`
  (the free `DecodeModule` / `DecodeModuleRawResidual` build a fresh heap
  `ModelState` per call), `ModelState::BeginDecode` / `BeginRawDecode` /
  `AdvanceGeneration` and the `generation` counter, the per-context stamp
  vectors and `Reset*Model` functions (`EnsureStamped` /
  `EnsureStampedTree` became create-if-null `EnsureContext`), the order-1
  opcode context stamp, `ByteModel`'s generation and stamps, and the
  arena's in-place `reset()`. Byte identity holds because the encoder has
  always run on a fresh `ModelState`, and every field `BeginDecode` reset
  already held that value in a fresh model.
- **Kept:** the per-instruction first-touch guards for the deliberately
  uninitialized `last_id` / `last_literal` / `last_word_count` arrays, now
  plain flags (`inst_group_touched`, `last_id_valid`, `last_literal_valid`)
  instead of generation stamps; the tracker ID map's generation stamp,
  which is its occupancy mechanism.
- **Tools:** `arixv_decode_bench --reuse-decoder` and the fuzz harness's
  `ARIXV_FUZZ_STATEFUL` mode (cross-call state bugs no longer exist) are
  removed; the overnight fuzz script's stateful AFL instance is now a plain
  ASan instance.
- **Result:** byte-identical M2 and raw-residual output; stateless decode
  instructions 307.7M → 302.7M per pass (−1.6%, fewer per-decode stamp
  vectors to allocate and zero); wall time +1.4% (8 of 9 interleaved rounds
  faster, but load rose mid-run, so treat it as "no regression"). `make
  test`, `make asan`, 3000-iteration fuzz, the 720-case mutated-blob
  differential against the previous CLI, and valgrind memcheck with full
  leak checking pass.

### Adaptation-rate bookkeeping (2026-11-18)

Byte-identical; developed with load-independent metrics (stateless decode
instructions per pass, as above), then timed. Profile share before: symbol-CDF
rate bookkeeping ~3.6%, binary `Prob` warm-up ~3%.

| change | stateless Ir per pass | Δ |
|---|---|---|
| baseline (0b0076c) | 302.7M | — |
| symbol CDFs: clamp-free rate, single count store (landed) | 298.3M | −1.5% |
| + binary `Prob`: branch-free warm-up (reverted) | 304.9M | +2.2% |

- **Symbol CDFs (landed).** The rate is `min(bitwidth(count + 1), cap)`
  with the count saturating at `2^cap − 1`; the clamp only ever fired at
  the saturated count. Saturating one step earlier, at `2^cap − 2`, gives
  the identical rate sequence (`bitwidth(2^cap − 1) = cap`) with no clamp,
  and the count is now written once, branch-free, as the lane-0 value
  `SymbolCdfAdapt` already restores (shadow training passes the count
  through unchanged).
- **Binary `Prob` (reverted).** With the default tuning the saturated count
  15 gives `bitwidth(16) = 5 = kProbShift`, so the shift could always be
  `bitwidth(count + 1)` with a branch-free count step. That cost
  instructions: most probabilities sit at the saturated count, where the
  old, well-predicted branch skips the `clz` entirely.
- Correctness: corpus and raw-residual output byte-identical; `make test`,
  `make asan`, 3000-iteration fuzz, the 720-case mutated-blob differential
  against the previous CLI (0 mismatches), and the scalar (`ARIXV_NO_SIMD`)
  build (identical blobs, cross-decodes with the SIMD build and the
  previous CLI) pass.
- Wall time (idle host, core 2, full corpus, 30 iterations, 9 interleaved
  rounds): stateless 83.5 → 83.8 MiB/s (+0.4% median; median per-round
  ratio ~1.009, with interference outliers in both builds) — a small
  non-regressing win, consistent with the instruction cut.

### Cross-tool benchmark refresh and heap-trim sensitivity (2026-11-18)

`scripts/bench/benchmark_spv_compressors.py --decode-iters=20`, pinned to
core 2 on an idle host, 1070-file intersection (same skips as July: smol-v
3, yari-v 10): ARIX-V mean/median/total 0.2203 / 0.2104 / 0.1681 (July
0.2240 / 0.2136 / 0.1715), encode 41.42 MiB/s, **decode 66.20 MiB/s (July
46.30, +43%)**; the other tools' rows match July within timing noise.

The script's 66.2 MiB/s sits well below `arixv_decode_bench`'s ~84 MiB/s,
a wider gap than in July. Its runner (`tools/bench/bench.cpp`) times each
`Decode` call the same way, but runs in short-lived 64-file batch processes
that encode each file just before decoding it. In one process over every
4th file, `build/arixv_bench` decodes at 74.6-74.9 MiB/s by default and
78.8-81.5 MiB/s with `GLIBC_TUNABLES=glibc.malloc.trim_threshold=268435456`:
the stateless decoder's ~0.5 MB of per-call model allocations are returned
to the kernel by heap trimming and faulted back in, and fresh batch
processes add cold-heap first-touch costs on top. The other tools allocate
little per call. *Follow-up candidate:* shrink or reuse the per-call
footprint (e.g. the four 56 KB per-instruction arrays and arena blocks) so
stateless decode is insensitive to the allocator's trim policy.

### Per-call memory footprint vs. allocator policy (2026-11-18)

Following up the cross-tool gap above. Same runner (`build/arixv_bench`),
all files, core 2, idle host:

| setting | decode |
|---|---|
| 64-file batch processes (what the script does) | 65.2 MiB/s |
| one long-lived process, default glibc | 75.0 MiB/s |
| one process, `glibc.malloc.trim_threshold` raised | 83.7 MiB/s |

Two effects, both about memory each `Decode` call touches: heap trimming
hands freed model memory back to the kernel and the next call faults it
back in; fresh processes start with no pre-faulted heap and glibc's
adaptive mmap/trim thresholds at their low defaults. Transparent huge pages
are `madvise`-only here and not involved (no THP faults).

Load-independent metrics: heap bytes per decode (malloc-level histogram
over the corpus), and minor page faults per decode in batched 64-file
processes ((faults at 21 iterations − at 1) / 20 per file).

| change | bytes/decode | batched faults/decode | stateless Ir |
|---|---|---|---|
| baseline (f484b18) | 590 KB | 9.88 | 298.2M |
| arena blocks start at 4 KB and double to 64 KB (**landed**) | ~500 KB | **7.10** | 301.0M (+0.9%) |
| ID map reserved for word_count / 4 instead of word_count (reverted) | — | 6.35 single-process (was 3.17) | — |
| per-instruction caches as lazy arena groups (reverted) | 360 KB | 7.64 | +0.6% |

- **Landed: geometric arena blocks.** Every arena (the model's, and each
  `ByteModel`'s, which used to take a 64 KB block for a few 544-byte trees)
  started with a full 64 KB block. Blocks now start at 4 KB and double;
  objects over 16 KB still get their own block. **Batched decode 65.1 →
  69.2 MiB/s (+6.3%, all 5 interleaved rounds)**; warm-process decode
  84.6 → 85.1 MiB/s (neutral), at +0.9% instructions for the extra small
  mallocs.
- **Reverted: smaller ID-map reservation.** It doubled faults and
  quadrupled `brk` calls: more mid-decode doubling rehashes, and without
  the oversized map's large frees glibc no longer raises its adaptive
  trim threshold, so the heap is trimmed more often.
- **Reverted: lazy per-instruction groups.** Bytes fell to 360 KB per decode
  but faults did not: those flat arrays were deliberately uninitialized, so
  their untouched pages never faulted. Cost +0.6% instructions.
- **What remains:** faults come from memory that is *touched*, mostly
  zero-initialized contexts; the largest is ~55 order-1 opcode trees ×
  2.2 KB per decode, fully zeroed though a walk touches 3 of each tree's 69
  tables. Lazily zeroing tree tables is the next lever, but it reaches into
  `BitTree`'s hot path.
- Correctness: corpus and raw-residual output byte-identical; `make test`,
  `make asan`, 3000-iteration fuzz, the 720-case mutated-blob differential,
  and valgrind memcheck with full leak checking pass.

Post-pass profile (reusable, instruction share): `CodeModule` body 46.7%,
`CodeUint` 15.2%, `RecencyList::Touch` 9.2%, cursor `Consume`+`Next` 8.0%,
`CodeByte` 4.1%, out-of-line `DecodeBit` 4.0% (plain flags). The largest
remaining decision bucket is the opcode index tree (E10 / E-A2).

## Suggested attack order

Tier A first — pure profiling debt, no ratio risk; estimated to take decode
from ~18 to ~50-70 MiB/s (strcmp + allocation + tracker alone are ~40% of
cycles, and cutting instruction count compounds). Then re-profile: if the
bit-decode chain is dominant, items 7+8 (encoder-computed static tables +
multi-symbol decode) are the highest-leverage format change short of the
Tier C redesign. *(2026-07-11: measured — items 7+8 as stated are dead
ends; the surviving form, corpus-trained static opcode tables at −5.0% size
and +13-16% decode, is parked on branch `static-opcode-tables` pending a
design decision.)* Prototype item 11 if "within ~2x of zstd's decode speed"
becomes the actual requirement — no amount of tuning gets a serial adaptive
binary coder there.
