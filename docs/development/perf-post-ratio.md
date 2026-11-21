# Post-ratio CPU performance plan

Started 2026-11-21 on branch `perf/post-ratio-speedup` from `main` (`59f55f3`).
Companion to [`performance.md`](performance.md) and [`ratio.md`](ratio.md).

## Goal

Recover (and ideally beat) the pre-ratio CPU throughput (**87.1 MiB/s** decode,
**50.0 MiB/s** encode on `049e8ad`) while keeping the `59f55f3` compression
ratio (**0.1252** full 1080-file corpus, **0.1255** 1070-file cross-tool
intersection) intact via byte-identical optimizations first, with optional
format / ratio-tradeoff levers tracked separately below.

## Baseline profile (`main` @ `59f55f3`)

### Macro delta (`049e8ad` → `59f55f3`)

- Adaptive decisions (`ARIXV_STATS=1`): **−13.7%** (`7.13M → 6.15M`), yet
  stateless decode instructions rose **+14.3%** (`290.0M → 331.4M` Ir/pass,
  callgrind over every 4th corpus file, `(iters=3 − iters=1) / 2`).
- Structural ceiling (`arixv_decode_bench --raw-residual`): **143.7 → 111.0
  MiB/s** — structural/model bookkeeping is now **~59%** of decode time (up
  from ~44%).
- Throughput (`taskset -c 4`, idle Ryzen 9 5950X):
  - `arixv_decode_bench --iters=5` (1080 files): **63.26 MiB/s** (encoded
    `759,260 B`, checksum `8949134200`).
  - `arixv_bench` (1080 files, single pass): **38.43 MiB/s** encode, **71.80
    MiB/s** decode (checksum `35796698800`).
  - Cross-tool 1070-file batch benchmark (`README.md`): **~67.5 MiB/s** decode,
    **~39.2 MiB/s** encode (was 87.1 / 50.0 MiB/s).
- Per-decode memory (`build/memory_breakdown`): `178,566 B` heap/decode,
  `88,018 B` arena blocks/decode, `39,088 B` model fixed touched + `58,048 B`
  arena objects touched (~97 KB total touched vs ~91 KB pre-ratio).

### Subsystem breakdown (callgrind, every 4th file = 270 files, `331.4M` Ir/pass)

| Subsystem | Ir / pass | Share | Notes |
|---|---:|---:|---|
| Entropy coding (range + rANS + `SymbolCdf`) | `118.7M` | 35.8% | `OpcodeTree`, `BitTree<4>`, `CodeUint`, `DecodeBit`/`DecodeBitLocal` |
| `model.cpp` field coders + `CodeModule` glue | `86.3M` | 26.0% | ~2× pre-ratio (`CodeIdRef`, `CodeDeferredResultType`, `CodeModule` loop, `CodeString`) |
| `Tracker` (`IdMap`, `OnIdUse`, `OnInstructionEnd`, type-algebra) | `37.7M` | 11.4% | >2× pre-ratio (~16M) |
| `RecencyList` (`Touch` / `Find` / `ShiftIn` / `Replace`) | `34.7M` | 10.5% | up from ~21M |
| `OperandCursor` | `18.6M` | 5.6% | constructed even on `fixed_layout` instructions |
| Alloc / zeroing / arena / `ModelState` setup | `14.5M` | 4.4% | `ModelState::ModelState` is 8.7% of all L1d misses |

### Branch & cache hotspots (callgrind `--cache-sim=yes --branch-sim=yes`, every 8th file = 135 files)

- **Branch mispredicts (`Bcm`):** **6.41%** of conditional branches (`1.49M`
  misses): `CodeModule` (35.0%), `CodeUint` (18.1%), `RangeDecoder::DecodeBit`
  (7.9%), `RecencyList::Touch` (5.9%).
- **L1i misses (code bloat):** `CodeModule<BitDecoder>` **16.0 KB**,
  `CodeString<BitDecoder>` **8.9 KB**, `BuildDeferredResultTypeCandidates`
  **5.0 KB** (cloned twice by LTO → **8.5%** of all I1 misses),
  `Tracker::OnInstructionEnd` **3.9 KB**.
- **L1d misses:** `CodeModule` (29.5%), `Tracker::Ensure` (18.4%),
  `ModelState::ModelState` (8.7%), `memset` (5.9%).

---

## Measurement & verification rules

Every byte-identical optimization is evaluated before keeping:

1. **Exact byte-identity:**
   - Full 1080-file corpus encoded size = `759,260 B`, `arixv_bench` checksum =
     `35796698800`, `arixv_decode_bench` checksum = `8949134200`.
   - `--raw-residual` checksum unchanged on the 1078 valid SPIR-V files.
   - Mutated-blob differential against `59f55f3` CLI matches on error codes and
     outputs.
   - Scalar (`-DARIXV_NO_SIMD`) build produces bit-identical output to SSE2.
2. **Load-independent metrics:**
   - Stateless callgrind `Ir` per pass over every 4th file (`(iters=3 - iters=1) / 2`).
   - Callgrind `Bcm` / `D1mr` / `I1mr` over every 8th file when changing branches
     or code layout.
   - `build/memory_breakdown` and 64-file batched minor page faults when touching
     allocations / `ModelState`.
3. **Wall-clock A/B:**
   - Interleaved paired rounds on `taskset -c 4` (`arixv_decode_bench --iters=10`
     and `arixv_bench` encode/decode) on an idle host.
4. **Full validation before merging:** `make all && make test && make asan`,
   3000-iter fuzz, valgrind memcheck, scalar/SIMD cross-check.

---

## Part A: Byte-identical optimizations (zero format / zero ratio change)

### Group 1 — Branchless range coder & `kProbShift == 3` warm-up (`src/range_coder.h`)
- [x] **1a. Branch-free `kProbShift == 3` warm-up in `Prob::UpdateFromRaw`** (`40c8f06`)
  - Profile: `L55` is **3.20% of all `Bcm`** + `bsr` instruction.
  - Because `ARIXV_KPROB_SHIFT == 3` sets `kPackedSaturation = 3` (`count ∈ {0,1,2,3}`),
    `shift = 1 + ((count + 1) >> 1)` and `next_count = count + (count < 3)` are
    exact with zero branch and zero `bsr`. (Unlike the earlier `kProbShift == 5`
    attempt in `performance.md`, no `bsr` is needed.)
- [x] **1b. Branchless bit decision & `v` update in `DecodeBitLocal`** (`40c8f06`)
  - Profile: `DecodeBitLocal` (`L297`) is **7.77% of all `Bcm`** (42.5% miss
    rate on uint mantissas); `DecodeBit` (`L222`) is **4.98% of all `Bcm`**
    (16.9% miss rate).
  - Empirical finding: making `DecodeBit` itself branchless (83% predicted)
    caused GCC register spills and doubled its instruction count (+24.5M Ir),
    whereas keeping `DecodeBit` branching and making `DecodeBitLocal` branchless
    via mask + parallel `v0`/`v1` (`Prob::UpdateFromRawBranchless`) gave a clean
    +1.33% wall-clock decode win (12/12 faster).

### Group 2 — `SymbolCdf` / rANS hot path (`src/symbol_coder.h`, `src/prob.h`, `src/model.cpp`)
- [x] **2a. `if constexpr (kN <= 8)` fast path in `SymbolCdfLookup` & `SymbolCdfAdapt`** (`9da3f71`)
  - Profile: `OpcodeTree::root`, `UintModel::length` root, and
    `result_type_choice` (>1.1M calls) have `kN <= 8` where `d[8..15]` is
    mathematically `0` forever, yet we previously loaded/compared/shifted/stored
    all 16 lanes. Halves SIMD work for `kN <= 8`.
- [x] **2b. Merge `t.d[0] = count` into lane 0 before the single 16B SSE2 store** (`9da3f71`)
  - Profile: `SymbolCdfAdapt` wrote `movaps %xmm0, (%rdx)` then immediately
    `mov %ax, (%rdx)`, causing a 16B→2B store-to-load forwarding stall whenever
    a CDF was reused back-to-back. Merged via `_mm_insert_epi16`.
- [x] **2c. Fuse `SymbolCdfLookup` + `SymbolCdfAdapt`, eliminate `uint16_t c[17]` stack array, & `CodeUint` fast paths** (`9da3f71`)
  - Profile: `uint16_t c[17]` on stack forced `-fstack-protector` `%fs:0x28`
    canary setup/teardown on every tree function + 16B→2B stall on `c[s]`, and
    `SymbolCdfAdapt` reloaded `t.d` and recomputed `d + U` already in XMM regs.
  - Bundled with `__builtin_clz` in `BitWidth` and `if (nbits <= 1) return nbits;`
    short-circuit in `CodeUint` (skips `HighRow` & zero-length bit sequence for `v == 1`).
- [x] **2d. Keep `RansDecoder::x_` in a local across multi-chunk walks & 16-bit LE `ReadWord`** (`26a8861`)
  - Implemented `RansDecoder::DecodeFirst`/`DecodeNext`/`Commit` behind a zero-overhead
    `SymbolSession` (`BeginSymbols()`) used by `OpcodeTree::CodeValue`, `BitTree::CodeValue`
    (`CodeUint` length tree, unknown-opcode tree), and `LazyLeafTree8::CodeValue`, plus
    `std::memcpy` (`movzwl`) for little-endian `ReadWord()` and `p_ = begin_` pinning on
    `InitState()` failure (removing dead `x_`/`last_f_` stores and redundant `x == 0`
    checks on chunk 2+).
  - Result: shrank `OpcodeTree::CodeValue` (`0x6f9 → 0x5da`), `LazyLeafTree8::CodeValue`
    (`0x4ca → 0x422`), and `CodeUint` (`0x9df → 0x96c`), cut **−1.43M Ir/pass**
    (`319.71M → 318.28M`), and gained **+1.05%** wall decode vs `5c` (12/12 faster).

### Group 3 — Eliminate redundant `Tracker` hash probes & bookkeeping (`src/tracker.h`, `src/tracker.cpp`, `src/model.cpp`)
- [x] **3a. `CodeResultId` fast-check `candidate > tracker.max_seen_id()`** (`baa1fe1`)
  - Profile: `tracker.Find(prev_result_id + 1)` did 239K hash probes; probe loop
    was **1.85% of all `Bcm`** + D1 misses. Any `candidate > max_seen_id()`
    cannot exist in `ids_`; skips ~85–90% of lookups.
- [x] **3b. Eliminate `PrevResListSlow` hash lookup** (`baa1fe1`)
  - Profile: **0.93M Ir** + D1 misses. `Tracker::OnInstructionEnd` already
    computed `info.type_list` for the previous instruction's result ID; returning
    and storing it directly on `ModelState` removes `tracker.Find(cur_prev_res_id)`.
- [x] **3c. Cache `{last_rtype_id, last_rtype_values_list}` from `OnResultTypeUse` for `ValuesListFor`** (`baa1fe1`)
  - Profile: `OnInstructionEnd` immediately re-hashed `info.result_type` right
    after `OnResultTypeUse(type)` already looked up that exact ID.
- [x] **3d. Fast-path `Tracker::OnInstructionEnd`** (`baa1fe1`)
  - Profile: **3.68% Ir**, 3.9 KB code. Skip `forward_refs_.Remove` when
    `forward_refs_.size() == 0`; load `desc->flags` once and short-circuit all
    11 `HasInstructionFlag` checks (**0.52% Ir** alone) via `kResultKindFlags`
    and `kBlockStateFlags`.
- [x] **3e. Remove `Tracker::IdMap::slot_gen_` using `keys_[i] == 0` empty sentinel** (`40e6c5b`)
  - Discovery: `IdMap` still allocated and probed a separate `slot_gen_` array
    left over from the removed reusable decoder. Because `id == 0` already uses
    `zero_info_`, zero-initialized `keys_` (`keys_[i] == 0` ⇒ empty) eliminates
    `slot_gen_` completely — saving 1 heap allocation per decode, ~2.5 KB/decode
    heap (`178,352 → 176,039 B`), 1 cache line touch per `Find`/`Ensure`, and
    **−2.19M Ir/pass** (+1.04% wall decode).

### Group 4 — `RecencyList` branchless `Touch` & `Replace` fast path (`src/tracker.cpp`)
- [x] **4a. `RecencyList::Replace` head fast path (`items_[0] == old_id`)** (`baa1fe1`)
  - Profile: sequential result IDs in `result_runs_` almost always match slot 0;
    `if (size_ > 0 && items_[0] == old_id) { items_[0] = id; return; }` skips
    the full SSE2 `MatchMask` scan on ~80%+ of result IDs.
- [x] **4b. `RecencyList::Touch` slot-0 head fast path** (`e88186b`)
  - Profile: `RecencyList` is **10.5% Ir** and **5.9% `Bcm`** (`if (last == 0)`
    mispredicts 36.4% = **3.24% of all `Bcm`**; `if (mask != 0)` mispredicts
    9.8% = **1.69% of all `Bcm`**).
  - Empirical finding: GCC already fuses `MatchMask` + `ShiftIn` automatically.
    Removing the slot-0 early exit (−2.11%) or unconditionally storing `size_`
    (−2.85%) regressed wall-clock. Moving the slot-0 check ahead of `MatchMask`
    (`if (size_ > 0 && items_[0] == id) return;`) and dropping the redundant
    post-`ctz` `if (last == 0) return;` cut **−1.42M Ir/pass** (`322.98M → 321.56M`)
    and gained **+0.46%** wall decode (10/12 faster).

### Group 5 — `CodeModule`, `CodeIdRef`, `CodeDeferredResultType`, `DecodeIO`, `WordBuffer` (`src/model.cpp`)
- [x] **5a. `ARIXV_NOINLINE` on `BuildDeferredResultTypeCandidates` + inline `choice == 1` fast path** (`3ace310`)
  - Profile: `BuildDeferredResultTypeCandidates` was 5.0 KB and cloned twice by
    LTO (**8.5% of all L1i misses**); candidate 1 (`ElementTypeOf(operand_types[0])`,
    e.g., `OpLoad`) hits more often than candidate 0. Inline fast path for
    `choice == 1` when `num_ids > 0 && num_literal_ints == 0 && operand_types[0] != 0`
    and `total_ids == 1 || t0->type_class != kPointer` avoids calling into it on
    `OpLoad` and unary extract ops.
- [x] **5b. Tighten `DecodeIO::Append` (1.67% Ir) & `WordBuffer::push_back` (1.39% Ir)** (`3ace310`)
  - Cache `uint8_t* ptr_` / `end_` in `DecodeIO` (single compare instead of
    reloading `out_.size()` twice); track `inline_cap_` in `WordBuffer` +
    `ARIXV_NOINLINE PushBackSlow` so `push_back` avoids inspecting `spill_` on
    the inline path.
- [x] **5c. Hoist `OperandCursor`, skip per-operand `coded_words.clear()`, & drop unused `cursor_field` copy on `fixed_layout`** (`98d3979`)
  - Empirical finding: outlining a second variable-layout operand switch
    (`ARIXV_NOINLINE CodeVariableOperands`) created a second call site to
    `CodeIdRef`/`CodeResultId`/`CodeValueEnum`/`CodeLiteralInteger`, causing
    GCC to un-inline all four (+8.59M Ir, −1.62% wall). Keeping the single
    operand switch while hoisting `OperandCursor cursor;` outside the
    instruction loop, clearing `coded_words` only on wide multiword operands,
    and skipping `cursor_field = field` on `fixed_layout` cut **−1.85M Ir/pass**
    (`321.56M → 319.71M`) and gained **+2.43%** wall decode (12/12 faster).
- [x] **5d. Evaluate branchless `CodeIdRef` mode select & `id_idx == 0` unification** *(tested & skipped)*
  - Empirical finding (7 variants tested): unifying `id_idx == 0` / `id_idx > 0`
    with pure arithmetic masks + `cur_list_by_mode[6]` + bitwise `need_update`
    cut callgrind's simulated `Bcm` by −174K (−2.6%) and `Bc` by −1.62M, yet
    added +5.82M Ir and regressed wall decode by −1.34% (0/12). Even replacing
    only the 5-case `switch` on `id_idx > 0` with `cur_list_by_mode[5]` +
    removing `kUnresolvedList`/`PrevResListSlow` added +0.92M Ir and lost
    −0.28% (3/12) because `mode == 0` lost its zero-load fast path and Zen 3's
    TAGE predictor predicts `id_idx == 0` far better than callgrind's branch
    simulator. Reverted cleanly.

### Group 6 — `CodeString` / `TouchString` & `ModelState` setup (`src/model.cpp`)
- [x] **6a. `ARIXV_NOINLINE` on `CodeGenericOperandWord`** (`3ace310`)
  - Profile: only hit in 4/1080 files, yet inlining bloated `CodeString<BitDecoder>`
    to 8.9 KB (**7.6% of all L1i misses**).
- [x] **6b. Static grammar `value_enum_ordinal` & flat `type_in_list`/`type_rank` in `ModelState`** (`3ace310`)
  - Profile: `ModelState::ModelState` was **8.7% of all L1d misses**.
    `GetValueEnumOrdinals()` is now a static table, and `type_in_list` /
    `type_rank` are direct `std::vector`s (removing 4 heap allocations).
- [x] **6c. Handle-based `TouchString` & avoid decode-hit re-search** (`ba12c71`)
  - Profile: **0.97% Ir**, **2.53% `Bcm`**. Replaced `vector<string>` rotation
    with `recent_string_pool` + 64-byte `recent_string_order` handle array and
    fast-pathed encode/decode hits directly via `PromoteRecentString` (**−3.00M
    Ir/pass**, +0.84% wall decode, 12/12 faster).

### Group 7 — Encoder hot path (`src/prob.h`, `src/model.cpp`)
- [x] **7a. Scalar lookup in `BitEncoder::CodeSymbol`** (`3ace310`)
  - Profile: **11.2% of encode Ir!** Replaced full SSE2 `SymbolCdfLookup` +
    stack `c[17]` copy with direct scalar loads (`t.d[s] + K::kUniform[s]`).
- [x] **7b. Bypass `coded_words` in `SingleWordStager` on encode too** (`3ace310`)
  - Profile: `WordBuffer<8>::push_back` was **1.2% of encode Ir** because
    `SingleWordStager` previously only fast-pathed decode.

---

## Part B: Optional format / ratio-tradeoff levers

Tracked here for evaluation **after** byte-identical wins are exhausted, or if
we want to push decode speed further at a small ratio cost. Each changes the
wire format or ratio and should be tested on its own branch/experiment.

- [ ] **T1. Skip `recent_ids_.Touch(result_id)` when `typed_lists_` already tracks it**
  - Rationale: saves one full `RecencyList::Touch` per value-producing
    instruction; typed-list hits already dominate ID references, so the ratio
    penalty should be small.
- [ ] **T2. Higher order-1 opcode promotion threshold (`kOpcodeOrder1MinCount = 8` vs `4`)**
  - Rationale: fewer lazily-allocated order-1 opcode contexts → smaller arena
    footprint, fewer D1 misses and page faults, tiny ratio cost.
- [ ] **T3. Compact / flat `IdMap` packing or smaller per-ID metadata**
  - Rationale: `Tracker::Ensure` is **18.4% of all L1d misses**; shrinking
    `IdInfo` or separating cold type-algebra fields from hot lookup fields
    (`result_type`, `type_list`, `values_list`) could cut D1 misses (potentially
    byte-identical if purely structural layout!).
- [ ] **T4. Existing tradeoff branches (from prior work, re-evaluate on `59f55f3` if needed):**
  - `size/tradeoff-no-order1-opcode` (was corpus +1.07%, Ir −4.0%, ~18 KB less touched)
  - `size/tradeoff-eight-operand-slots` (was corpus +8 B, ~3 KB less touched)
  - `ratio/tradeoff-id-list-31` (existing branch; check speed/ratio tradeoff)

---

## Progress & measurement log

Baseline (`59f55f3`):
- Encoded bytes: `759,260 B` (ratio `0.125223`), `arixv_bench` checksum `35796698800`, `arixv_decode_bench` checksum `8949134200`.
- Callgrind stateless Ir/pass (every 4th file, n=270): `331.4M`.
- Callgrind Bcm (every 8th file, n=135, 1→3 diff/2): `1.49M` (`6.41%`).
- `arixv_decode_bench --iters=10` (`taskset -c 4`): paired A/B against saved `59f55f3` binary at each step.
- `arixv_bench` paired A/B (`taskset -c 4`, 6 rounds): `40.82 MiB/s` encode, `75.60 MiB/s` decode (`59f55f3` baseline).
- `--raw-residual` ceiling (1078 valid SPIR-V files, `--iters=5`, 6 paired rounds): `114.56 → 133.99 MiB/s` (**+13.91%**, 6/6 faster, checksum `8949125080`).

| Step | Commit | Items | Stateless Ir/pass (Δ vs base) | Wall decode A/B vs base (`--iters=10`, 12 rounds) | Wall encode / decode (`arixv_bench`) | Status |
|---|---|---|---:|---:|---:|---|
| Base | `59f55f3` | — | `331.40M` (`0.00%`) | `0.00%` (`~72.3 MiB/s`) | `41.15 / 76.40 MiB/s` (6-rd med) | baseline |
| Batch 1a | `baa1fe1` | `3a`, `3b`, `3c`, `3d`, `4a` | `328.95M` (`−0.74%`) | `+2.09%` (`73.69 MiB/s`, 12/12) | — | kept |
| Batch 1b | `3ace310` | `5a`, `5b`, `6a`, `6b`, `7a`, `7b` | `322.38M` (`−2.72%`) | `+4.22%` (`75.37 MiB/s`, 12/12) | `42.82 / 79.15 MiB/s` | kept |
| Batch 2 | `40c8f06` | `1a`, `1b` (`DecodeBitLocal` branchless) | `330.02M` (`−0.42%`) | `+5.6%` (`+1.33%` vs 1b, 12/12) | `43.79 / 78.74 MiB/s` | kept |
| Batch 3 | `9da3f71` | `2a`, `2b`, `2c` + `CodeUint` fast paths | `328.17M` (`−0.98%`) | `+6.97%` (`77.40 MiB/s`, 12/12) | — | kept |
| Batch 4 | `40e6c5b` | `3e` (remove `IdMap::slot_gen_`) | `325.98M` (`−1.64%`) | `+7.73%` (`78.02 MiB/s`, 12/12) | — | kept |
| Batch 5 | `ba12c71` | `6c` (handle-based `TouchString` & hit fast path) | `322.98M` (`−2.54%`) | `+8.64%` (`78.60 MiB/s`, 12/12) | `45.16 / 81.67 MiB/s` (`+10.58% / +7.87%`, 6/6) | kept |
| Batch 6 | `e88186b` | `4b` (`RecencyList::Touch` slot-0 head fast path) | `321.56M` (`−2.97%`) | `+9.13%` (`79.02 MiB/s`, 12/12; `+0.46%` vs `6c`) | — | kept |
| Batch 7 | `98d3979` | `5c` (hoist `OperandCursor`, skip `coded_words.clear()`) | `319.71M` (`−3.53%`) | `+11.41%` (`80.69 MiB/s`, 12/12; `+2.43%` vs `4b`) | — | kept |
| — | *(reverted)* | `5d` (7 branchless/table `CodeIdRef` variants) | `320.63M..325.53M` | `−0.28%..−2.13%` vs `5c` | — | skipped |
| Batch 8 | `26a8861` | `2d` (local `RansDecoder` state + 16-bit LE `ReadWord`) | **`318.28M` (`−3.96%`)** | **`+12.04%` (`80.89 MiB/s`, 12/12; `+1.05%` vs `5c`)** | **`45.80 / 84.19 MiB/s` (`+11.58% / +10.00%`, 6/6)** | kept |

### Part A completion summary (`26a8861`)
- **Byte-identity:** `759,260 B` (`0.125223` ratio on all 1080 corpus files), `arixv_bench` checksum `35796698800`, `arixv_decode_bench` checksum `8949134200`, `--raw-residual` checksum `8949125080`.
- **Verification:** `make all`, `make test`, `make asan`, `-DARIXV_NO_SIMD` scalar parity (`35796698800`), 300/300 mutated-blob differential matches against `59f55f3`, 3000-iter `fuzz_roundtrip`, and `valgrind --leak-check=full` all passed cleanly.
- **Branch & memory deltas (`59f55f3` → `26a8861`):**
  - Callgrind `--branch-sim=yes` (every 8th file, n=135, 1→3 diff/2): `Ir 200.56M → 192.68M` (−3.93%), `Bc 23.30M → 21.71M` (−6.82%), `Bi 328.3K → 305.9K` (−6.82%).
  - `build/memory_breakdown`: `178,566 → 175,745 B` heap/decode (−2,821 B/decode).
