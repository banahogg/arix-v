# Multi-module gate 3: validated structural prototype (AXST)

Research only (Track B, gate 3 of the [multi-module plan](multi-module.md)).
`AXST` is a research framing, not a format proposal. Single-module `ARX2`,
the golden vectors, the public API, and the standard translation units are
unchanged. Everything here builds only with `ARIXV_BUILD_RESEARCH_PACK=ON`.

> **Retired tooling (library 1.1).** The block codec described here now ships
> as the ARXP pack API (`src/pack_block.inc`, `src/arixv_pack.cpp`). The AXST
> framing, the ARXP research mirror, `arixv_struct_probe` and
> `arixv_struct_parallel` were removed then; check out commit `99df5ed` to
> rerun the commands below. `arixv_pack_api_bench` remains, on the public API.

Status, 2026-11-21:

- All 470 frozen packs × 4 configurations encode and decode byte-exactly,
  and the 1.0 decoder rejects every stream.
- The quick-reject gates agreed after gate 2 all pass.
- Single-worker decode time and the ratio cost of independent blocks are
  measured.
- Multi-worker scaling is not measured yet.

## Decision context (agreed 2026-11-21)

The gate-2 thresholds are **quick-reject gates only**:

- at least −12% versus ARX2 on duplicate-free representative collections;
- smaller than the best generic method on Dota 2 (#unique);
- no representative pack more than +0.5% versus framed ARX2;
- full-set decode at most 2× independent ARX2.

Passing them does not mean acceptance. Size and decode speed are judged
together. For example, −12% at 2× slower is probably a no, but either figure
could be acceptable if the other were much better.

## What was built

| File | Role |
|---|---|
| `src/research_struct.inc` | The AXST codec. Included by `model.cpp` inside its `ARIXV_RESEARCH_PACK` block |
| `src/research_struct_step.inc` | **Generated** from `CodeModule` by `scripts/bench/gen_research_step.py`. Header, per-instruction step, and tail as separate templates, plus ID-role recording. Research CTest `research_struct_step_current` fails if it is stale | *(Removed at gate 4: the codec now uses the production hooked `CodeModule`; see the gate-4 report.)*
| `src/research_pack.h` | `EncodeStructPack`, `DecodeStructPack`, `ModelIdRoles`, plan and stats types |
| `tools/bench/struct_probe.cpp` | `arixv_struct_probe`: `pack` (encode, decode, verify, optional timing and `--out`), `control` (identity checks), `decode` / `decode-arx2` (RSS) |
| `tools/bench/structure_dump.cpp` | Now takes frames and roles from `ModelIdRoles`, the codec's own masks |
| `scripts/bench/multi_module_structure.py` | New `gate3`, `analyze3` and `blocks` subcommands. Encoder plans come from the gate-2 parser (ID-abs mode, depth 8) |
| `scripts/bench/multi_module_probe.py` | The research fuzz smoke also seeds AXST streams, including small frozen packs with copies (`--axst-plans`) |
| `tests/research_pack_test.cpp` | AXST tests. Both libFuzzer harnesses now also cover AXST |

### Stream

```text
"AXST" | version=1 | flags | compact count | compact module bytes[k] | one continuous coder payload
flags: bit0 signal (dup/structural/copy signaling), bit1 warm (gate-1b opcode/count/enum carry), others 0
```

Per member, in the one range/rANS stream:

1. **Duplicate flag.** If set, the distance to an identical earlier member
   (adaptive Elias-γ).
2. Otherwise, a **structural flag.** Members that are not model-eligible are
   coded with the byte model, like ARX2 mode 1.
3. Otherwise the module is coded by the **unchanged model step**. Before each
   instruction, once a candidate base exists and the target is not
   byte-swapped, an adaptive flag says whether a copy starts there. A copy
   codes:
   - "rep" (continue the previous copy's base run), or the base member
     distance plus a direct-coded instruction position;
   - an adaptive Elias-γ length;
   - for every ID word of the copied run, one choice: correspondence-map hit,
     base + running delta, or an explicit zigzag delta (adaptive Elias-γ, per
     role).

### Train-through

Copied instructions run through the **same** model step as literal ones, on
the encoder path with a probability-update-only coder (`TrainBits`). The
input is the reconstructed words (`TrainIO`). The model, tracker and
string state after a copy are therefore exactly what coding the instruction
would have produced, so later literals keep their full context. Encoder and
decoder train identically.

### Design choices that keep production code untouched

- **The step is a generated copy of `CodeModule`.** Copy signaling must sit
  between instructions, and single instructions must be trainable.
  Refactoring the production loop would change standard translation units,
  so the step is generated mechanically. A test keeps it in sync, and two
  identity controls prove it is bit-exact.
- **Copy-base frames and ID roles come from the model itself.** They are
  recorded from the model's operand categories while each module is coded,
  so building a base needs no separate grammar walk. They match gate 2's
  grammar walk on 352,216 of 352,222 instructions. The plan dump uses the
  same roles.
- **The encoder follows an external plan** from the Python gate-2 parser,
  which is an encoder-side choice. The decoder needs no plan and validates
  every reference:
  - distance, base candidacy, position, and length;
  - that each copied instruction fits the module;
  - that the model frames it identically.

  The encoder rejects any invalid plan copy with an error. It never drops a
  copy silently.

## Validation

| Check | Result |
|---|---|
| Identity controls (`control`; also in CTest over fixture prefixes): AXST without signaling vs gate 1b's continuous payload | Bit-identical for `cont:none` and `cont:oce:carry` |
| Corpus: 470 packs × {dedup, dedup+warm, copy, copy+warm} | 1,880/1,880 decode byte-exact; 1.0 decoder returns `kBadContainer` for all |
| Release (-O3) and RelWithDebInfo builds | Identical stream sizes for every pack × config |
| `research_pack_test` (CTest, 300 iterations) | Pass. Includes: identity; round trips with copies, duplicates, raw/empty/header-only/swapped members, warm on and off; copy cheaper than recoding a renumbered module; plan rejection (mismatch, future base, overlap, unreached, out-of-range, swapped target, copies or raw members without signaling); every truncated prefix rejected; 3,000 byte-flip hostile decodes; mutation fuzz with auto-planned copies |
| `research_pack_test --fuzz-iters 2000` under ASan/UBSan (`build/research-asan`) and an ASan probe of the Dota 2 pack | Pass |
| `research_struct_step_current` | Pass |
| Research fuzz smoke (libFuzzer rt/dec under ASan/UBSan, 300 s × 8 jobs, AXRP + AXST seeds) | Clean: rt 68,891 execs, cov 4,787; dec 587,910 execs, cov 4,923; 0 crash/timeout/OOM |
| `ctest --preset default` / `ctest --test-dir build/research` | 7/7 and 9/9 (incl. `research_struct_step_current`) |
| Standard `model.cpp` preprocessed vs HEAD (encoder, decoder-only, stats, raw residual) | Identical |

## Size results (validated bytes)

These are byte-weighted ratios of serialized bytes to original bytes.

- "warm (1b)" is gate 1b's `cont:oce:carry`.
- "est*" is the gate-2 estimate.
- "Best generic" is the per-pack minimum of original and smol-v with solid
  zstd-19, zstd-22 w27, and xz -9e.
- The full tables are in `build/gate3/results/analysis.md`.

### By source

| Group | Packs | ARX2 | ARX2+dedup | Warm (1b) | AXST dedup+warm | AXST copy | **AXST copy+warm** | est* | Best generic | vs ARX2 | Per-pack median (IQR) | vs best generic | < generic |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Dota 2 | 1 | 0.1490 | 0.1296 | 0.1386 | 0.1205 | 0.0410 | **0.0402** | 0.0453 | 0.0500 | −73.0% | — | −19.5% | 1/1 |
| Dota 2 (#unique) | 1 | 0.1490 | 0.1493 | 0.1387 | 0.1388 | 0.0472 | **0.0462** | 0.0521 | 0.0571 | −69.0% | — | −19.0% | 1/1 |
| Blender | 1 | 0.1331 | 0.1332 | 0.1307 | 0.1308 | 0.0970 | **0.0966** | 0.1080 | 0.1433 | −27.5% | — | −32.6% | 1/1 |
| Shadertoy | 1 | 0.0914 | 0.0915 | 0.0890 | 0.0890 | 0.0872 | **0.0860** | 0.0873 | 0.1128 | −6.0% | — | −23.7% | 1/1 |
| Naga remaps (#unique) | 1 | 0.1383 | 0.1395 | 0.1080 | 0.1080 | 0.0704 | **0.0681** | 0.0799 | 0.0942 | −50.8% | — | −27.7% | 1/1 |
| GL CTS (zink/angle) | 20 | 0.1226 | 0.1236 | 0.1026 | 0.1027 | 0.0639 | **0.0621** | 0.0679 | 0.0938 | −49.4% | −16.5% (−45.4…−7.7) | −33.8% | 20/20 |
| Vulkan-Samples collections (rep) | 151 | 0.1375 | 0.1369 | 0.1276 | 0.1265 | 0.1059 | **0.1032** | 0.1090 | 0.1515 | −25.0% | −13.0% (−20.4…−8.5) | −31.9% | 151/151 |
| Stage-pair packs | 201 | 0.1494 | 0.1507 | 0.1432 | 0.1435 | 0.1389 | **0.1363** | 0.1406 | 0.2060 | −8.8% | −7.3% (−11.7…−4.5) | −33.8% | 201/201 |
| Variant-family packs | 14 | 0.0830 | 0.0833 | 0.0793 | 0.0794 | 0.0558 | **0.0554** | 0.0610 | 0.0696 | −33.3% | −27.3% (−43.1…−19.2) | −20.4% | 14/14 |
| Control: unrelated mixes | 4 | 0.1439 | 0.1448 | 0.1323 | 0.1324 | 0.1299 | **0.1255** | 0.1308 | 0.1714 | −12.8% | −10.2% | −26.8% | 4/4 |
| Control: cross-frontend | 45 | 0.1540 | 0.1522 | 0.1377 | 0.1354 | 0.1221 | **0.1168** | 0.1246 | 0.1633 | −24.1% | −19.8% (−26.1…−16.3) | −28.5% | 45/45 |
| Control: singletons | 2 | 0.0753 | 0.0754 | 0.0754 | 0.0754 | 0.0754 | 0.0754 | 0.0754 | 0.0811 | +0.0% | — | −7.1% | 2/2 |

### By manifest tier

| Tier | Packs | ARX2 | Warm (1b) | AXST copy+warm | vs ARX2 | Per-pack median (IQR) | vs best generic | < generic | < warm (1b) |
|---|---|---|---|---|---|---|---|---|---|
| rep same_collection | 153 | 0.1401 | 0.1317 | **0.0823** | −41.2% | −13.0% (−20.9…−8.8) | −30.5% | 153/153 | 132/153 |
| ↳ no exact duplicates | 144 | 0.1343 | 0.1277 | **0.1020** | −24.1% | −12.7% (−19.2…−8.5) | −32.2% | 144/144 | 123/144 |
| ↳ with exact duplicates | 9 | 0.1503 | 0.1388 | **0.0473** | −68.6% | −22.7% (−58.4…−20.4) | −23.1% | 9/9 | 9/9 |
| rep stage_pair | 201 | 0.1494 | 0.1432 | **0.1363** | −8.8% | −7.3% (−11.7…−4.5) | −33.8% | 201/201 | 145/201 |
| rep variant_family | 14 | 0.0830 | 0.0793 | **0.0554** | −33.3% | −27.3% (−43.1…−19.2) | −20.4% | 14/14 | 14/14 |

Across all 470 packs, including those gate 1b skipped, the ratio is 0.1320
for ARX2 and 0.0860 for AXST copy+warm.

Where AXST loses to the gate-1b warm start (77 small representative packs),
the loss is at most 8 B (2.5% of a 300-byte pack). AXST frames each member
with its byte size where gate 1b used body words, and it adds per-member
flags.

### Quick-reject gates

| Gate | Threshold | Result | Pass |
|---|---|---|---|
| Duplicate-free representative collections vs ARX2 | ≤ −12% | −24.1% | yes |
| Dota 2 (#unique) vs best generic | < 0 | −19.0% (44,129 vs 54,448 B, original + xz -9e) | yes |
| Worst representative pack vs framed ARX2 | ≤ +0.5% | −1.24% | yes |
| Full-set decode vs independent ARX2 (single worker) | ≤ 2× | 1.20× on representative collections | yes |

### Validated against the gate-2 estimate

The real bytes beat the estimate in every group, for example −24.1% against
−18.3% on duplicate-free collections. Three reasons:

- Adaptive explicit-ID coding is far cheaper than the estimate's uniform
  `log2(bound)`.
- The stream is continuous with one flush.
- Warm statistics combine with copies; that adds about 1–4%.

The central gate-2 assumption, that copied instructions can train the model
without being coded, is now implemented and validated.

## Decode speed and memory

The measurement setup:

- Release (-O3, no LTO) research build, one worker pinned to one core of an
  idle Ryzen 9 5950X.
- Best of 7, resident input, outputs kept.
- Independent ARX2 means decoding the standalone blobs of the same members
  one after another through `arixv::Decode`, in the same binary.

This is a first measurement, not yet the full §2 performance contract.

| Group | Original MiB | ARX2 ms | ARX2 MiB/s | AXST dedup+warm ms | AXST copy ms | AXST copy+warm ms | copy+warm MiB/s | copy+warm / ARX2 |
|---|---|---|---|---|---|---|---|---|
| Dota 2 | 1.05 | 17.3 | 61 | 16.0 | 20.5 | 20.7 | 51 | 1.20× |
| Dota 2 (#unique) | 0.91 | 15.0 | 61 | 16.5 | 20.5 | 20.8 | 44 | 1.38× |
| Blender | 0.74 | 11.7 | 63 | 12.9 | 14.6 | 14.5 | 51 | 1.24× |
| GL CTS | 0.68 | 10.8 | 63 | 11.8 | 13.6 | 14.0 | 49 | 1.29× |
| Vulkan-Samples collections (rep) | 1.62 | 26.7 | 61 | 28.5 | 31.0 | 31.4 | 51 | 1.18× |
| Stage-pair packs | 0.97 | 15.6 | 62 | 16.8 | 17.1 | 17.6 | 55 | 1.13× |
| Variant-family packs | 0.38 | 5.0 | 77 | 5.2 | 5.8 | 5.8 | 66 | 1.16× |
| Control: singletons | 0.24 | 2.9 | 85 | 3.1 | 3.1 | 3.1 | 80 | 1.07× |
| rep same_collection | 3.40 | 55.6 | 61 | 57.4 | 66.1 | 66.7 | 51 | 1.20× |
| ↳ no exact duplicates | 2.18 | 35.1 | 62 | 38.1 | 41.9 | 42.2 | 52 | 1.20× |

Where the time goes (Dota 2, callgrind, before the last two optimizations):

- **Train-through: about half of decode instructions.** A copied instruction
  costs about the same as decoding it. This is the design's inherent cost:
  copying saves bytes, not model work.
- **Literal decode:** about 11%.
- **Avoidable overhead, now removed:**
  - a separate grammar walk to build base ID roles, replaced by roles
    recorded from the model;
  - hash-map ID restoration, replaced by a dense per-base table reused
    across targets with stamps;
  - eager base construction.

  Together these took decode from 1.45× to 1.20× of ARX2.
- **Residual step overhead:** 1.07× on singletons. This is the generated
  step function, role recording, and the output copy.

Decode is still slower than independent ARX2 for three reasons:

- copied instructions cost as much model work as decoding;
- the prototype keeps every base's words, frames and roles;
- the step adds a little overhead.

Exact duplicates are faster than ARX2, because they are memcpy.

**Memory:** decoding Dota 2 (1.05 MiB output, all outputs kept) peaks at
8.8 MB RSS, against 5.9 MB for independent ARX2. The difference is the
retained base words, roles, frames and per-base ID tables, about 2.5–3× the
output size.

**Encode:** given a plan, the C++ encoder takes 23 ms for Dota 2 and 322 ms
for all 470 packs. Producing the plan takes the Python parser about 1.3 s
for Dota 2. A production encoder would need a native parser; this is not
optimized.

## Independent blocks

Each block is a complete AXST stream with copy+warm. It has its own header,
duplicate detection, and plan, and its bases come from inside the block
only. Blocks are consecutive members in manifest order. These are
decode-verified bytes; full tables are in `build/gate3/blocks/blocks.md`.
Only multi-member packs are included.

| Group | ARX2 | 1 mod | 2 | 4 | 8 | 16 | 32 | 64 | Whole | ≤64 KiB | ≤256 KiB | ≤1 MiB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Dota 2 | 0.1490 | 0.1493 | 0.1265 | 0.1107 | 0.0946 | 0.0773 | 0.0686 | 0.0498 | **0.0402** | 0.1003 | 0.0692 | 0.0443 |
| Dota 2 (#unique) | 0.1490 | 0.1492 | 0.1283 | 0.1101 | 0.0931 | 0.0803 | 0.0688 | 0.0561 | **0.0462** | 0.1038 | 0.0732 | 0.0462 |
| Blender | 0.1331 | 0.1332 | 0.1162 | 0.1123 | 0.0967 | 0.0966 | 0.0966 | 0.0966 | 0.0966 | 0.1332 | 0.1298 | 0.0966 |
| Naga remaps (#unique) | 0.1383 | 0.1392 | 0.1140 | 0.0982 | 0.0834 | 0.0776 | 0.0734 | 0.0711 | 0.0681 | 0.0745 | 0.0681 | 0.0681 |
| GL CTS (15 multi-member) | 0.1233 | 0.1240 | 0.1020 | 0.0856 | 0.0742 | 0.0679 | 0.0633 | 0.0610 | 0.0610 | 0.0646 | 0.0610 | 0.0610 |
| Vulkan-Samples collections | 0.1380 | 0.1387 | 0.1238 | 0.1113 | 0.1049 | 0.1040 | 0.1037 | 0.1037 | 0.1037 | 0.1093 | 0.1037 | 0.1037 |
| Variant families | 0.0830 | 0.0833 | 0.0662 | 0.0554 | 0.0554 | 0.0554 | 0.0554 | 0.0554 | 0.0554 | 0.0716 | 0.0554 | 0.0554 |
| Stage pairs | 0.1499 | 0.1509 | 0.1383 | 0.1368 | … | … | … | … | 0.1368 | 0.1368 | 0.1368 | 0.1368 |

Gains saturate at about 8–16 members for Vulkan-Samples collections, 4 for
variant families, and 32–64 for GL CTS. Dota 2 keeps gaining up to the whole
set. 64-member blocks lose 24% of its whole-set size (0.0498 against
0.0402), and 1 MiB blocks lose about 10% (0.0443). Byte caps of 64–256 KiB
cost much more than member caps on large application packs. Parallel decode
of independent blocks is straightforward, but its speed-up is not measured
here.

## Findings

1. **The structural opportunity holds with validated bytes, and it is
   larger than estimated.**
   - −24.1% versus ARX2 on duplicate-free representative collections.
   - Dota 2 is 19% smaller than xz -9e, Blender −27.5%, and variant
     families −33%.
   - AXST beats the best generic method on every representative pack.
2. **Copying is the mechanism; warm statistics are a small additive gain.**
   Compare AXST dedup+warm with copy+warm, and copy with copy+warm.
3. **Decode costs about 1.2× independent ARX2, single-threaded.** Train-through
   makes copied instructions cost about as much as decoding. Duplicate-heavy
   sets offset this, because exact duplicates are nearly free.
4. **Full-set solid streams matter for large application sets.** Independent
   blocks are nearly free for small collections, but cost Dota 2 24% at 64
   members.
5. **Small, diverse sets gain modestly** (stage pairs −8.8%). AXST is
   slightly worse than the gate-1b warm start on tiny packs (≤ 8 B).

## Limitations

- **Plan-driven encoder.** A production encoder needs a native parser. The
  parse quality (greedy, depth 8) is untuned; gate 2 found search depth
  barely matters.
- **The step is a generated copy of `CodeModule`.** It is kept current by a
  check, but any production design would refactor the model loop instead.
- **Memory is not bounded.** Base retention grows with the set. Blocks or a
  base budget would bound it, at the measured block cost. No streaming
  decode: the rANS tail is needed first, as in gate 1b.
- **Timing scope.** Single worker on one host. No multi-worker scaling, cold
  versus warm process, or allocator study yet (§2 contract).
- **Corpus limits.** Two application-derived origins, each a single pack.
  Representative packs are mostly Vulkan-Samples directories. Folds were not
  used for any choice. The application conclusions remain provisional.
- **Hostile-input coverage** is unit tests, mutation fuzz, and a 300-second
  coverage-guided smoke. There is no long campaign yet.

## Hardening status

- **Research fuzz smoke** (`multi_module_probe.py fuzz-smoke --seconds 300
  --jobs 8 --axst-plans build/gate3/results/packs`) is clean.
  - Seeds: 199 AXRP streams, 100 AXST pairs, and 100 small frozen packs with
    copies.
  - rt: 68,891 execs, coverage 4,787 (gate 1b: 3,086).
  - dec: 587,910 execs, coverage 4,923 (gate 1b: 3,103).
  - No crashes, timeouts or OOMs.
- **ASan/UBSan:** `research_pack_test --fuzz-iters 2000` and the Dota 2
  probe pass.
- **Standard `fuzz-smoke`** was not rerun. Standard builds compile a
  preprocessed `model.cpp` identical to HEAD.
- **Before any integration:** long campaigns (AFL++/cmplog,
  `scripts/fuzz/overnight.sh`-style) on the AXST decoder.

## Rerun

```bash
cmake -S . -B build/research -DCMAKE_BUILD_TYPE=RelWithDebInfo -DARIXV_BUILD_RESEARCH_PACK=ON
cmake --build build/research -j
cmake -S . -B build/research-release -DCMAKE_BUILD_TYPE=Release -DARIXV_BUILD_RESEARCH_PACK=ON
cmake --build build/research-release -j --target arixv_struct_probe
D=build/gate1-coordination
COMMON="--manifest $D/manifest.json --corpus-dir /path/to/corpus --source-lock /path/to/lock.json"
# Gate-2 dump + parse (dump roles = AXST model roles)
python3 scripts/bench/multi_module_structure.py run $COMMON \
  --structure-dump build/research/arixv_structure_dump \
  --gate1a-baselines $D/gate1a-results/pack_baselines.csv \
  --gate1a-per-module $D/gate1a-results/per_module.csv \
  --gate1b-packs build/research/gate1b-corpus/packs.csv --out build/gate2/results
G3="$COMMON --dump build/gate2/results/structure_dump.bin --gate2-packs build/gate2/results/packs.csv"
python3 scripts/bench/multi_module_structure.py gate3 $G3 --probe build/research/arixv_struct_probe --out build/gate3/results
taskset -c 4 python3 scripts/bench/multi_module_structure.py gate3 $G3 \
  --probe build/research-release/arixv_struct_probe --out build/gate3/timing --time 7 --jobs 1
python3 scripts/bench/multi_module_structure.py analyze3 --results build/gate3/results --timing build/gate3/timing
python3 scripts/bench/multi_module_structure.py blocks $COMMON --dump build/gate2/results/structure_dump.bin \
  --probe build/research/arixv_struct_probe --gate3-packs build/gate3/results/packs.csv --out build/gate3/blocks
```

The size run takes about 4 s, timing about 22 s, and blocks about 20 s on
this host.
