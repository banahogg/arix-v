# ARIX-V 1.0 Roadmap & Parallel Multi-Module Plan

Companion to [`performance.md`](performance.md), [`ratio.md`](ratio.md), and
[`perf-post-ratio.md`](perf-post-ratio.md).

## Goal & Release Scope

Take ARIX-V from a single-platform C++ research prototype to a shippable,
engine-ready **1.0** release while enabling **multi-module / shader-pack
compression research** to proceed in parallel without fighting merge conflicts.

- **1.0 format commitment:** 1.0 freezes the single-module `ARX2` bitstream and
  container format, backed by golden test vectors and cross-platform CI.
- **Multi-module scope:** Multi-module / shader-pack compression is developed on
  its own parallel track as an **additive** format (distinct 4-byte magic, e.g.
  `ARXB` / `ARX3`, for 1.x/2.0). It never invalidates `ARX2` single-module blobs
  and does not block 1.0.
- **Leave `src/model.cpp` as one file** for 1.0 so the productization track and
  the multi-module research track avoid churn conflicts.

---

## Execution Structure

```text
Phase 0: Shared Infrastructure (on main, before branching)
   ├─ GitHub Actions CI (Linux GCC/Clang, ASan/UBSan, AArch64 NEON, Windows MSVC)
   ├─ Concrete rough edges (MSVC intrinsics, Encode thread-safety, stack ModelState)
   └─ Small model.cpp seams (raw-buffer DecodeIO, opt-in ARIXV_STATS & RawResidual)
          │
          ├──────────────────────────────────────────┐
          ▼                                          ▼
Track A: 1.0 Productization                 Track B: Multi-Module Research
(mostly arixv.h/cpp, grammar_tables,        (separate branch; prototype & oracle
 CMakeLists.txt, cli/, tests/, README)       for cross-shader compression)
   1. Thin C-only public API & zero-copy
   2. Decoder-only build & binary size
   3. CMakeLists.txt + CI coverage
   4. CLI usability
   5. README refresh & v1.0 freeze
```

---

## Phase 0: Shared Infrastructure (Pre-Branch, on `main`)

Land these small, byte-identical changes on `main` first so both parallel tracks
inherit CI coverage, cross-platform builds, and clean seams.

### 0a. GitHub Actions CI (first, before fixing MSVC)
Add `.github/workflows/ci.yml` running on pushes and PRs:
- **Linux x86_64 (`ubuntu-latest`):**
  - GCC & Clang builds via `Makefile` (`make all && make test`, including `check-generated` after fetching pinned grammar sources).
  - Scalar/SIMD parity check (`-DARIXV_NO_SIMD` matches SSE2 on corpus/golden tests).
  - ASan + UBSan (`make asan`).
  - `-fno-exceptions -fno-rtti` compile + test check (once Phase 0b lands).
- **Linux or macOS AArch64 (`ubuntu-24.04-arm` / `macos-latest`):**
  - Verifies the AArch64 NEON SIMD paths (`ARIXV_SYMBOL_NEON`, `ARIXV_RECENCY_NEON`) and scalar parity.
- **Windows MSVC (`windows-latest`):**
  - Added immediately (initially expected to fail on `__builtin_*`), then goes green in 0b.

### 0b. Fix Concrete Rough Edges
- **MSVC / Windows portability (`src/platform.h`, `src/range_coder.h`, `src/symbol_coder.h`, `src/tracker.cpp`, `src/prob.h`):**
  - Replace all 12 raw `__builtin_clz` / `__builtin_ctz` / `__builtin_expect` calls with portable helpers in `src/platform.h` (`Clz32`, `Ctz32`, `ARIXV_UNLIKELY`), using `_BitScanReverse` / `_BitScanForward` on `_MSC_VER`.
  - Ensure `src/` builds warning-clean on MSVC (`/W4` or clean `/W3`).
- **`Encode()` thread-safety (`src/prob.h`, `src/model.cpp`):**
  - Currently `EncodeModule()`, `CodeModule()`, `StatSiteScope`, and `BitEncoder::CodeBit()` read/write non-atomic globals (`g_stat_cat`, `g_stat_site`) even when `ARIXV_STATS` is unset, causing data races under concurrent `Encode()` calls.
  - Gate all `ARIXV_STATS` global state and per-bit checks behind `#ifdef ARIXV_STATS` (or keep it off-by-default at compile time, enabled in `arixv_bench` / diagnostic targets), making `Encode()` cleanly stateless and thread-safe.
- **Heap-allocate `ModelState` in `EncodeModule()` (`src/model.cpp`):**
  - Mirror `DecodeModule()`'s `std::make_unique<ModelState>()` instead of placing the ~39 KB `ModelState` on the stack (safer for small worker-thread stacks on Windows/macOS).
- **`-fno-exceptions` / `-fno-rtti` cleanliness (`src/arena.h`, `src/arixv.cpp`):**
  - `src/arena.h` has the only `throw` in `src/` (`throw std::bad_alloc()`). Guard it with `#if defined(__cpp_exceptions) || defined(_CPPUNWIND)` (falling back to `std::abort()` when compiled with `-fno-exceptions` / `/EHs-c-`, matching standard library containers under `-fno-exceptions`).
  - No RTTI is used anywhere in `src/`; verify `-fno-exceptions -fno-rtti` in CI.

### 0c. Minimal `src/model.cpp` Seams for Track A
Keep changes localized to avoid merge friction with Track B:
- **Raw-buffer `DecodeIO`:** Change `DecodeIO` and `DecodeModule()` to write into a caller-provided `(uint8_t* out, size_t out_size)` span rather than owning/resizing `std::vector<uint8_t>&`.
- **Opt-in benchmark-only residual coder:** Gate `RawResidualEncoder`, `RawResidualDecoder`, `EncodeModuleRawResidual`, and `DecodeModuleRawResidual` behind `#ifdef ARIXV_ENABLE_RAW_RESIDUAL` (enabled by `decode_bench` and `model_test` in the Makefile, excluded from library builds by default).
- **Decoder-only compile switch (`ARIXV_DECODER_ONLY`):** Gate `BitEncoder`, `RansEncoder`, `RangeEncoder`, `EncodeIO`, `EncodeModule`, and `arixv_encode` behind `#ifndef ARIXV_DECODER_ONLY`.

---

## Track A: 1.0 Productization

### A1. Thin C-Only Public API & Zero-Copy Decode (`src/arixv.h`, `src/arixv.cpp`)
Because ARIX-V has not yet shipped 1.0, replace the C++ `arixv::` public API
with a single thin, ABI-clean C API in `src/arixv.h` (`#ifdef __cplusplus extern "C"`)
and remove `src/arixv_decoder_compat.h`.

Implementation remains C++17 internally (explicitly documented; CMake propagates
C++ runtime linkage automatically).

**Proposed C API surface:**

```c
#define ARIXV_MAGIC_0 'A'
#define ARIXV_MAGIC_1 'R'
#define ARIXV_MAGIC_2 'X'
#define ARIXV_MAGIC_3 '2'

typedef enum arixv_status {
  ARIXV_STATUS_OK = 0,
  ARIXV_STATUS_BAD_CONTAINER,    // wrong magic, bad mode/header, or trailing bytes
  ARIXV_STATUS_TRUNCATED,        // input ends before declared output is complete
  ARIXV_STATUS_BUFFER_TOO_SMALL, // out != NULL and *out_size < required (*out_size updated)
  ARIXV_STATUS_INVALID_ARGUMENT, // null required pointer or size > UINT32_MAX
  ARIXV_STATUS_OUT_OF_MEMORY,    // internal allocation failed (when exceptions enabled)
} arixv_status;

typedef struct arixv_encode_options {
  // Always emit the coded payload even when stored mode would be smaller.
  int force_coded;
} arixv_encode_options;

const char* arixv_status_string(arixv_status status);

// Vulkan two-call decode pattern (stateless, thread-safe):
// - If `out == NULL`: parses the container header in O(1), writes the declared
//   uncompressed byte size to `*out_size`, and returns ARIXV_STATUS_OK (or a
//   container/truncation error). Note: `*out_size` comes from the untrusted
//   header; callers should validate it against their own budget before allocating.
// - If `out != NULL`: checks `*out_size >= required`; if too small, writes
//   `required` to `*out_size` and returns ARIXV_STATUS_BUFFER_TOO_SMALL
//   (allowing single-call optimistic decoding into a reusable scratch buffer).
//   Otherwise decodes directly into `out`, writes actual bytes to `*out_size`,
//   and returns ARIXV_STATUS_OK.
// - 0-byte outputs are unambiguous: `out == NULL` always queries size;
//   `out != NULL` with `*out_size == 0` decodes a 0-byte payload.
arixv_status arixv_decode(const void* in, size_t in_size,
                          void* out, size_t* out_size);

#ifndef ARIXV_DECODER_ONLY
// O(1) upper bound on encoded size for `in_size` bytes (in_size + 10; 0 if
// `in_size > UINT32_MAX`). Avoids two-pass encoding.
size_t arixv_encode_bound(size_t in_size);

// Single-call encode into caller-provided `out` (stateless, thread-safe).
// `opts` may be NULL for defaults. On ARIXV_STATUS_OK, `*out_size` receives
// the exact encoded byte count.
arixv_status arixv_encode(const void* in, size_t in_size,
                          void* out, size_t* out_size,
                          const arixv_encode_options* opts);
#endif
```

**C-boundary invariants:**
- No C++ exceptions may ever escape `extern "C"` entry points: wrap in
  `try { ... } catch (const std::bad_alloc&) { return ARIXV_STATUS_OUT_OF_MEMORY; }`
  when exceptions are enabled (`#if defined(__cpp_exceptions) || defined(_CPPUNWIND)`).
- Keep internal fuzzing/testing coverage for hostile inputs, truncated headers,
  `out == NULL` queries, `BUFFER_TOO_SMALL` retry paths, 0-byte payloads, and
  alignment-safe writes into `uint32_t`-aligned buffers (`VkShaderModuleCreateInfo::pCode`).
- Update `cli/`, `tests/`, `tools/bench/`, and `tools/fuzz/` to use the C API
  (a tiny internal C++ test/tool helper can wrap `std::vector<uint8_t>` if convenient).
- Add a pure-C (`.c`, compiled with `cc -std=c11`) smoke test in `tests/` + CI
  verifying `arixv.h` compiles cleanly as strict C.

### A2. Decoder-Only Binary Size & Runtime Footprint
Target a lean, focused runtime decoder library for engine integration:
- Measure baseline vs decoder-only object/binary sizes (`.text`, `.rodata`, `.data.rel.ro`, `.data`) under `-O3` and `-Os`.
- Already gated in Phase 0c:
  - `ARIXV_DECODER_ONLY` drops encoder coder/model instantiations.
  - `ARIXV_ENABLE_RAW_RESIDUAL` (off by default) drops benchmark-only raw-residual instantiations.
- **Trim unused grammar string tables (`scripts/gen_grammar_tables.py`, `src/grammar_tables.*`):**
  - `InstructionDesc::name` (~876 core + hundreds of ext-inst names) and
    `OperandKindDesc::name` (~91 names) are never read anywhere in `src/` (only
    `ExtInstSetDesc::name` is used by `FindExtInstSet`).
  - Gate `InstructionDesc::name` and `OperandKindDesc::name` behind a diagnostic
    macro (e.g. `ARIXV_GRAMMAR_NAMES`, enabled for tests/coverage tools and off
    by default) so the linker/compiler doesn't ship tens of KB of dead `.rodata`
    and PIC relocations into runtime engine binaries.
- Audit any remaining heavy `.rodata` / `.text` contributors in the decoder-only
  build (e.g., `nm --size-sort` / `bloaty` or `size`) and document the final
  footprint in `README.md`.

### A3. `CMakeLists.txt` (FetchContent-Friendly)
Add a clean, minimal `CMakeLists.txt` at the repo root alongside the existing
`Makefile`:
- Library target `arixv` (static/normal library) + alias `arixv::arixv` for
  `FetchContent` / `add_subdirectory` consumers.
- Public include dir `src/`, C++17 requirement on implementation only (C consumers
  include `arixv.h` directly).
- Options (defaulting sensibly when included as a subproject via `PROJECT_IS_TOP_LEVEL`):
  - `ARIXV_DECODER_ONLY` (`OFF` by default; defines `ARIXV_DECODER_ONLY` publicly)
  - `ARIXV_BUILD_CLI` (`ON` when top-level, `OFF` when subproject)
  - `ARIXV_BUILD_TESTS` (`ON` when top-level, `OFF` when subproject)
- Keep `Makefile` working for fast local dev/fuzz/bench workflows; CI tests both
  `Makefile` and `CMake` (including `ARIXV_DECODER_ONLY=ON` and pure-C consumer).

### A4. CLI Usability (`cli/main.cpp`)
Keep the CLI focused on offline build-step compression and inspection without
bloating it:
- Commands:
  - `arixv c [--force-coded] [-q|-v] <in> <out>` (plus `-` for stdin/stdout; optional summary line with `-v`)
  - `arixv d <in> <out>` (plus `-` for stdin/stdout)
  - `arixv info <files...>` — O(1) header inspection (magic, mode, compressed size, uncompressed size, ratio) without full decode
  - `arixv roundtrip [--force-coded] <files...>` (existing)
  - `arixv --version` / `-h` / `--help` — prints version (`1.0.0`) and container magic (`ARX2`)
- Batch compression convenience (e.g. `arixv c --out-dir <dir> <files...>` or similar) so build scripts can compress a directory of shaders in one invocation.
- Clear, consistent exit codes and error messages on stderr.

### A5. Documentation Refresh & v1.0 Freeze
- Refresh [`README.md`](../../README.md):
  - Updated benchmark table (post-Part-A ~81–84 MiB/s decode + note on AArch64 NEON & multi-core scaling).
  - Updated usage examples for the C API (Vulkan two-call decode + `VkShaderModuleCreateInfo` example, `arixv_encode_bound` + `arixv_encode`, `CMakeLists.txt` / `FetchContent`, `ARIXV_DECODER_ONLY` binary size).
  - Explicit statement of C++17 internal implementation / `-fno-exceptions` compatibility and `ARX2` v1.0 format stability promise.
- Verify full release checklist before tagging `v1.0.0`:
  - All CI jobs green (Linux GCC/Clang, Windows MSVC, AArch64 NEON, ASan/UBSan, `-fno-exceptions -fno-rtti`, `ARIXV_DECODER_ONLY=ON`, pure-C consumer, `check-generated`).
  - `make fuzz-smoke` clean on the new C API / zero-copy decode path + hostile-input tests.
  - Golden `ARX2` test vectors frozen and verified byte-identical across x86_64 SSE2, AArch64 NEON, and scalar builds.

---

## Track B: Multi-Module / Shader-Pack Compression (Parallel Research)

Runs on a separate branch (e.g. `research/multi-module`) after Phase 0 lands,
tracked in its own development doc (e.g. `docs/development/multi-module.md`).

### Motivation
Real Vulkan applications ship shader packs / pipeline caches containing dozens to
thousands of SPIR-V modules (permutations, uber-shader variants, shared UBO
layouts, shared debug/type/helper declarations). Today, ARIX-V resets every
adaptive model between files (median shader ~2.4 KB), paying cold-start every
time and missing all cross-shader redundancy.

### Research Questions & Milestones
1. **Corpus & benchmark setup:** Assemble a realistic multi-shader evaluation
   benchmark (both grouped subsets of the existing corpus by source/project and
   real shader-permutation sets where available), measuring both **total pack
   ratio** and **random-access / full-pack parallel decode throughput** vs
   `tar + zstd -19`, `smol-v + tar + zstd -19`, and per-file `ARIX-V`.
2. **Oracle analysis (before committing to a design):** Quantify where
   cross-module savings actually come from:
   - Warm-starting adaptive models (`ModelState` / `StringModel` / opcode & enum
     CDFs) across modules in a group vs resetting per module.
   - Cross-module string dictionary / history sharing.
   - Similarity sorting + structural or byte-level delta / shared-block
     deduplication (e.g., identical type/constant/decoration preambles).
3. **Container & API design:**
   - Distinct container magic (e.g. `ARXB` / `ARX3`), completely additive to
     frozen single-module `ARX2`.
   - Design for **chunked / grouped blocks** (e.g. 8–32 related shaders per
     independent block) so engines still get fast random access to individual
     shaders *and* multi-core parallel decode across blocks.
   - Maintain ARIX-V's core principle: purely adaptive / grammar-derived, no
     corpus-trained shipped tables, hardened against hostile inputs.
