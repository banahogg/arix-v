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
2. **Cheaper reusable model reset.** `__memset` is 1.85% of reusable samples
   under `Decoder::Decode`. First split it between `IdMap::Clear` and the
   model-array resets, then measure a touched-slot clear list or generation
   stamps against their lookup and memory costs; do not assume either wins
   without a corpus benchmark.
3. **Entropy format, not more micro-tuning.** The coding path is about two
   thirds of reusable cycles and remains serial because every adaptive bit
   updates its next probability. A fast optional mode with encoder-supplied
   static probabilities, or a narrow fixed/global multi-symbol table for
   only opcode/uint-length fields, is the remaining path to a large gain.
   Per-file broad rANS tables and four interleaved range streams were already
   measured and reverted (items 8-9).

## Tier B — small format/ratio tradeoffs (path to 100+ MiB/s)

7. **Semi-adaptive probabilities: let the encoder do the adapting.** The
   design is already asymmetric (offline CLI encode, runtime decode). Push
   further: the encoder runs the adaptive model as today but *snapshots*
   converged probabilities for hot contexts into the container (quantized,
   only contexts actually used). The decoder loads static probabilities and
   skips `Update()` for those contexts — the update is roughly half the
   per-bit dependency chain — and static probs enable item 8. Ratio cost:
   table bytes, mostly amortized on medium/large files; could be optional by
   file size.

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
    Decode = zstd (≈700 MiB/s) + a cheap reconstruction pass with no per-bit
    modeling — essentially "smol-v with a much smarter transform." Ratio
    lands between current ARIX-V and smol-v+zstd: trade maybe 10-20% ratio
    for ~10-30x decode speed. If the 100x gap is the headline problem, this
    is the architecture that closes it.

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

## Suggested attack order

Tier A first — pure profiling debt, no ratio risk; estimated to take decode
from ~18 to ~50-70 MiB/s (strcmp + allocation + tracker alone are ~40% of
cycles, and cutting instruction count compounds). Then re-profile: if the
bit-decode chain is dominant, items 7+8 (encoder-computed static tables +
multi-symbol decode) are the highest-leverage format change short of the
Tier C redesign. Prototype item 11 if "within ~2x of zstd's decode speed"
becomes the actual requirement — no amount of tuning gets a serial adaptive
binary coder there.
