# ARIX-V 1.0 Roadmap & Parallel Multi-Module Plan

Companion to [`performance.md`](performance.md), [`ratio.md`](ratio.md), and
[`perf-post-ratio.md`](perf-post-ratio.md).

## Goal & Release Scope

Take ARIX-V from a single-platform C++ research prototype to a shippable,
engine-ready **1.0** release while enabling **multi-module / shader-pack
compression research** to proceed in parallel without fighting merge conflicts.

- **1.0 format commitment:** 1.0 freezes the single-module `ARX2` bitstream and
  container format — including the pinned SPIR-V grammar revision
  (`02c0394e…`), generator-derived model tables/priors, and all wire-format
  constants — backed by an immutable cross-architecture golden fixture suite.
- **Multi-module scope:** Multi-module / shader-pack compression is developed on
  its own parallel track as an **additive** format (distinct 4-byte magic, e.g.
  `ARXB` / `ARX3`, for 1.x/2.0). It never invalidates `ARX2` single-module blobs
  and does not block 1.0.
- **Leave `src/model.cpp` as one file** for 1.0 so the productization track and
  the multi-module research track avoid churn conflicts.
- **Single build system (DRY):** Replace `Makefile` with `CMakeLists.txt` (+
  `CMakePresets.json`, requiring CMake >= 3.21) so Linux, macOS, Windows, and
  both parallel tracks share one source of truth. Source-copying directly into
  an application still requires no build system.
- **Decoder-only by default:** CMake exposes `arixv::decoder` (runtime decoder
  only) and `arixv::encoder` (adds the encoder). For source-copy builds,
  omitting `src/arixv_encode.cpp` builds only the decoder; define
  `ARIXV_ENCODER=1` and include `src/arixv_encode.cpp` to include the encoder.
- **Portability & exception contract:** Clean builds under `-fno-exceptions
  -fno-rtti` (GCC, Clang, and MSVC `/EHs-c- /D_HAS_EXCEPTIONS=0`), and full
  32-bit (`ILP32`) + 64-bit (`LP64`/`LLP64`) support. Exception-enabled builds
  catch allocation/container exceptions at the C boundary and return
  `ARIXV_STATUS_OUT_OF_MEMORY`; `-fno-exceptions` builds explicitly document
  abort-on-OOM for 1.0 (revisitable via `opts`/allocator callbacks later
  without breaking the 1.0 API).

---

## Execution Structure

```text
Phase 0: Shared Infrastructure (on main, before branching)
   ├─ [x] 0a. GitHub Actions CI (Linux GCC/Clang, ASan/UBSan, macOS AArch64 NEON, Windows MSVC)
   ├─ [x] 0b. Concrete rough edges (MSVC intrinsics, Encode thread-safety, heap ModelState, -fno-exceptions)
   └─ [x] 0c. model.cpp seams, hardening, golden vectors & CMakeLists.txt (drop Makefile; unify CI)
          │
          ├──────────────────────────────────────────┐
          ▼                                          ▼
Track A: 1.0 Productization                 Track B: Multi-Module Research
(include/arixv/, arixv*.cpp,                (separate branch; prototype & oracle
 grammar_tables, cli/, tests/, README)       for cross-shader compression)
   [x] 1. Thin C-only public API & zero-copy decode
   [x] 2. Decoder-only binary size trimming
   [x] 3. CLI usability
   [x] 4. Format spec, README refresh & v1.0 freeze
```

---

## Phase 0: Shared Infrastructure (Pre-Branch, on `main`)

Land these byte-identical infrastructure changes on `main` first so both
parallel tracks inherit CI coverage, cross-platform builds, one build system,
golden vectors, and clean seams.

### [x] 0a. GitHub Actions CI (`7bb1710`)
- `.github/workflows/ci.yml` covering Linux (`gcc` and `clang`: default SIMD,
  `-DARIXV_NO_SIMD`, ASan+UBSan, `gen_grammar_tables.py --check`), macOS
  AArch64 (NEON + scalar), and Windows MSVC x64.

### [x] 0b. Concrete Rough Edges (`67f0420` .. `ede7184`)
- Portable `Clz32` / `Ctz32` / `ARIXV_LIKELY` / `ARIXV_UNLIKELY` /
  `ARIXV_HAS_SSE2` (including MSVC x64 `_M_X64`) / `ARIXV_HAS_NEON` /
  `ARIXV_HAS_EXCEPTIONS` in `src/platform.h`, replacing all raw `__builtin_*`
  calls across `src/`.
- Gated `ARIXV_STATS` globals and per-bit instrumentation behind
  `ARIXV_ENABLE_STATS` so library `Encode()` is cleanly thread-safe (verified
  under ThreadSanitizer with `ConcurrentEncodeDecode()`).
- Heap-allocated `ModelState` in `EncodeModule()` and `EncodeModuleRawResidual()`.
- Guarded `Arena::Allocate` OOM `throw std::bad_alloc()` behind
  `ARIXV_HAS_EXCEPTIONS` and added `-fno-exceptions -fno-rtti` checks to CI.
- Fixed Apple Clang NEON macro expansion (`symbol_coder.h`), MSVC zero-length
  grammar array (`gen_grammar_tables.py` / `grammar_tables.cpp`), and MSVC
  `/W3` warnings (`range_coder.h`); all 4 CI jobs passing.

### [x] 0c. Minimal `src/model.cpp` Seams, Hardening, Golden Suite & CMake Migration
Keep `src/model.cpp` edits localized to avoid merge friction with Track B while
closing all build, portability, and determinism gaps before branching:

- **Raw-buffer `DecodeIO`:** Change `DecodeIO` and `DecodeModule()` to write
  directly into a caller-provided `(uint8_t* out, size_t out_size)` buffer
  rather than owning/resizing `std::vector<uint8_t>&`.
- **Clean decoder/encoder split:** Move encoder entry points into a dedicated
  TU (`src/arixv_encode.cpp`) and gate encoder-only internals behind
  `#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)` — gating whole function
  definitions only, never struct/class members shared across TUs (zero ODR
  risk).
- **Complete wire-format tuning lock:**
  - Move `ARIXV_KOPCODE_ORDER1_MIN_COUNT` (`src/model.cpp:34`) into
    `src/tuning.h` alongside all other wire constants.
  - Lock all `#ifndef ARIXV_K*` overrides in `src/tuning.h` behind an explicit
    opt-in `ARIXV_RESEARCH_TUNING` macro (disallowed / `#error` in standard
    library builds, or forced to a non-`ARX2` magic) and add a CI grep check
    ensuring no `#ifndef ARIXV_K` exists outside `src/tuning.h`.
- **Pre-branch portability, 32-bit & safety fixes:**
  - **32-bit (`ILP32`) alignment & offset safety:**
    - Use explicit 16-byte aligned allocation (`_aligned_malloc` /
      `_aligned_free` on Windows, `posix_memalign` / `free` on POSIX) in
      `src/arena.h` instead of relying on `std::malloc` (8-byte aligned on
      32-bit), and ensure `ModelState` allocation respects `alignof(ModelState)
      == 16` (`SymbolCdf` and `RecencyList` are `alignas(16)`).
    - Use checked unsigned `size_t` offsets (avoiding `end - ptr` pointer
      subtraction UB past `PTRDIFF_MAX`) in `DecodeIO` and `RansDecoder`, and
      widen `StringModel` history positions (`src/model.cpp:518`) from
      `uint32_t` to `size_t`.
  - **Hostile-input tracker hardening:** Size `Tracker::Reserve` from
    compressed payload length (growing as IDs arrive) rather than pre-allocating
    up to 1M slots (~36 MiB) from the untrusted header size, and switch
    `Tracker::IdMap` hashing from low bits (`(id * kIdHashMul) & mask_`) to
    high bits (`(id * kIdHashMul) >> shift`) to prevent mod-capacity linear
    probing collisions (preserves byte-identical encoding).
  - **Remove lazy function-local static:** Generate `GetValueEnumOrdinals()`
    (`src/model.cpp`) as a `constexpr` table in `scripts/gen_grammar_tables.py`
    so concurrent first-call decode is safe even under `-fno-threadsafe-statics`.
  - **Windows ARM64 NEON:** Include `defined(_M_ARM64)` in `ARIXV_HAS_NEON`
    (`src/platform.h`).
  - **Release-safe tests:** Replace `assert(...)` in `tests/` with an
    always-active `CHECK(...)` macro so CMake `Release` (`-DNDEBUG`) never
    strips test checks or decode calls.
- **Cross-architecture golden vector suite:**
  - Commit a compact set of real corpus + edge-case encoded `ARX2` fixtures
    (covering stored mode, byte-coded mode, structural SPIR-V, 16-symbol CDFs,
    byte-swapped input, ext-inst sets, long strings, and unknown/future opcodes
    & enumerants) with expected SHA-256 digests of both originals and encoded
    outputs.
  - Verify decoding and encoder byte-identity against these fixtures on every
    CI architecture and build configuration.
- **Replace `Makefile` with `CMakeLists.txt` + `CMakePresets.json`:**
  - `cmake_minimum_required(VERSION 3.21)` (for `PROJECT_IS_TOP_LEVEL`).
  - Public headers under `include/arixv/`; keep `src/` strictly `PRIVATE` so
    internal headers (`platform.h`, `model.h`, `tracker.h`, `arena.h`, `prob.h`,
    `tuning.h`) never collide with downstream engines.
  - **Library targets & shared/static topology:**
    - `ARIXV_BUILD_ENCODER` option (default `${PROJECT_IS_TOP_LEVEL}` — `OFF`
      by default for subproject/FetchContent consumers, `ON` for top-level
      builds).
    - **Static builds (`BUILD_SHARED_LIBS=OFF`, default):**
      - When `ARIXV_BUILD_ENCODER=OFF`: `arixv_decoder` (`ALIAS arixv::decoder`)
        compiles `src/model.cpp` with `ARIXV_ENCODER=0` (no encoder code
        compiled).
      - When `ARIXV_BUILD_ENCODER=ON`: `arixv_decoder` (`ALIAS arixv::decoder`)
        builds the core archive with `EncodeModule` hidden (stripped by
        `-ffunction-sections -Wl,--gc-sections` / `/OPT:REF` / LTO when linking
        only `arixv::decoder`, verified in A2), and `arixv_encoder` (`ALIAS
        arixv::encoder`) builds `src/arixv_encode.cpp`, links `arixv_decoder`,
        and propagates `ARIXV_ENCODER=1` `PUBLIC`. Diamond builds linking both
        `arixv::decoder` and `arixv::encoder` into one executable have zero
        duplicate symbols and zero ODR issues.
    - **Shared builds (`BUILD_SHARED_LIBS=ON`):**
      - Builds a **single** shared library (`arixv`) exporting only `ARIXV_API`
        public symbols (decoder-only when `ARIXV_BUILD_ENCODER=OFF`,
        decoder+encoder when `ON`). `arixv::decoder` (and `arixv::encoder` when
        enabled) resolve cleanly to it — avoiding two DLLs trying to share
        hidden C++ internals or both exporting `arixv_decode`.
    - **Private internal & isolated diagnostic targets:**
      - `arixv_internal` (always `STATIC` regardless of `BUILD_SHARED_LIBS`) so
        white-box tests (`model_test`, `grammar_tables_test`,
        `operand_cursor_test`), `cli`, and `fuzz_roundtrip` always link cleanly
        without exporting internal C++ symbols.
      - Isolated static targets (`arixv_internal_stats`,
        `arixv_internal_raw_residual`, etc.) for any build using diagnostic
        macros (`ARIXV_ENABLE_STATS`, `ARIXV_ENABLE_RAW_RESIDUAL`,
        `ARIXV_TRACE_DECISIONS`, `ARIXV_MEMSTATS`), preventing ODR mismatches
        with standard library targets.
  - **Symbol visibility & packaging:**
    - `ARIXV_API` defaults to **empty** (safe for static and source-copy
      builds); shared builds define `ARIXV_SHARED` (`PUBLIC`) and
      `ARIXV_BUILDING` (`PRIVATE`) for `dllimport` / `dllexport` /
      `visibility("default")`.
    - `CXX_VISIBILITY_PRESET hidden`, `VISIBILITY_INLINES_HIDDEN ON`, versioned
      internal inline namespace (`arixv::v1`), and `install(EXPORT)` +
      `find_package(arixv)` config.
  - Options defaulting to `${PROJECT_IS_TOP_LEVEL}`: `ARIXV_BUILD_CLI`,
    `ARIXV_BUILD_TESTS`, `ARIXV_BUILD_TOOLS` (`arixv_bench`,
    `arixv_decode_bench`, `fuzz_roundtrip`, plus opt-in diagnostic/libFuzzer
    targets).
  - `CMakePresets.json` (`default`, `release`, `asan`, `tsan`, `decoder-only`,
    `fuzz-smoke`).
  - Update `scripts/bench/benchmark_spv_compressors.py`, `scripts/fuzz/smoke.sh`,
    `scripts/gen_grammar_tables.py` (comment), `AGENTS.md`, and
    `.github/workflows/ci.yml`:
    - Linux GCC & Clang (SIMD, scalar, `-fno-exceptions -fno-rtti`, ASan+UBSan,
      TSan, 32-bit `-m32`, `check-generated`, tuning-override grep check).
    - macOS AArch64 (NEON + scalar).
    - Windows MSVC x64 (SSE2 + scalar running full test suite + `/EHs-c-
      /D_HAS_EXCEPTIONS=0`).
    - Consumer smoke tests via `FetchContent` and `install` + `find_package`
      against both static and `BUILD_SHARED_LIBS=ON` builds (linking both
      `arixv::decoder` and `arixv::encoder` into one executable).

---

## Track A: 1.0 Productization

### A1. Thin C-Only Public API & Zero-Copy Decode
Replace the C++ `arixv::` public API with a single thin, ABI-clean C API in
`include/arixv/arixv.h` (`#ifdef __cplusplus extern "C"`) and remove
`src/arixv_decoder_compat.h`.

Implementation remains C++17 internally (explicitly documented; CMake propagates
C++ runtime linkage automatically — document/handle `enable_language(CXX)` for
pure-C CMake parents, and document `-lstdc++` / `-lc++` for non-CMake
consumers).

**Public C API contract (`include/arixv/arixv.h`):**

```c
#define ARIXV_VERSION_MAJOR 1
#define ARIXV_VERSION_MINOR 0
#define ARIXV_VERSION_PATCH 0

#define ARIXV_MAGIC_0 'A'
#define ARIXV_MAGIC_1 'R'
#define ARIXV_MAGIC_2 'X'
#define ARIXV_MAGIC_3 '2'

/* ARIXV_API defaults to empty (static / source-copy builds).
 * Shared builds define ARIXV_SHARED (and ARIXV_BUILDING while compiling). */
#ifdef __cplusplus
#define ARIXV_NOEXCEPT noexcept
#else
#define ARIXV_NOEXCEPT
#endif

// Fixed-width 32-bit status code for stable C/C++ ABI across compilers.
// Callers should handle unknown future status values gracefully.
typedef int32_t arixv_status;
enum {
  ARIXV_STATUS_OK = 0,
  ARIXV_STATUS_BAD_CONTAINER = 1,       // malformed header, bad mode, or trailing bytes
  ARIXV_STATUS_UNSUPPORTED_FORMAT = 2,  // recognized ARX? magic for newer/unsupported version
  ARIXV_STATUS_TRUNCATED = 3,           // input ends before declared output is complete
  ARIXV_STATUS_BUFFER_TOO_SMALL = 4,    // out != NULL and *out_size < required (*out_size updated)
  ARIXV_STATUS_INVALID_ARGUMENT = 5,    // null required pointer, invalid opts, or size > limit
  ARIXV_STATUS_OUT_OF_MEMORY = 6,       // internal allocation failed (exceptions-enabled builds)
  ARIXV_STATUS_TOO_LARGE = 7,           // valid container exceeds this platform's addressable limit
};

// Packed library version: (major << 16) | (minor << 8) | patch.
ARIXV_API uint32_t arixv_version(void) ARIXV_NOEXCEPT;

ARIXV_API const char* arixv_status_string(arixv_status status) ARIXV_NOEXCEPT;

typedef struct arixv_header_info {
  uint32_t struct_size;        // caller must set to sizeof(arixv_header_info)
  uint32_t magic;              // little-endian 4CC (e.g. '2XRA' for "ARX2")
  uint32_t mode;               // container mode byte (stored / raw-byte / SPIR-V)
  uint32_t reserved;           // set to 0
  uint64_t uncompressed_size;  // declared output size in bytes (untrusted)
} arixv_header_info;

// O(1) header inspection without decompressing (stateless, thread-safe).
// Validates the container header and populates `*out_info` on ARIXV_STATUS_OK.
// Accepts any `out_info->struct_size` in [v1_size, sizeof(arixv_header_info)].
ARIXV_API arixv_status arixv_inspect(const void* in, size_t in_size,
                                     arixv_header_info* out_info) ARIXV_NOEXCEPT;

typedef struct arixv_decode_options {
  uint32_t struct_size;  // must be in [v1_size, sizeof(arixv_decode_options)]
  uint32_t flags;        // reserved; must be 0
} arixv_decode_options;

// Decompresses a blob produced by `arixv_encode` (stateless, thread-safe).
// `opts` may be NULL for defaults. `in` and `out` must not overlap.
//
// Follows the Vulkan two-call size-query pattern:
// - If `out == NULL`: inspects the container header without decompressing,
//   writes the declared uncompressed size (in bytes) to `*out_size`, and
//   returns ARIXV_STATUS_OK (or a container/format/truncation/too-large error).
//   Note: `*out_size` comes from the untrusted container header; callers should
//   validate it against their own size budget before allocating.
// - If `out != NULL`: `*out_size` on entry is the capacity of `out` in bytes.
//   If `*out_size` is smaller than the declared output size, leaves `out`
//   untouched, writes the required size to `*out_size`, and returns
//   ARIXV_STATUS_BUFFER_TOO_SMALL (allowing single-call optimistic decoding
//   into a reusable scratch buffer). Otherwise decodes directly into `out`,
//   writes the exact byte count to `*out_size`, and returns ARIXV_STATUS_OK.
//   On any error other than BUFFER_TOO_SMALL, `out` contents are unspecified
//   and `*out_size` is unchanged.
// - 0-byte outputs are unambiguous: `out == NULL` always queries size;
//   `out != NULL` with `*out_size == 0` decodes a 0-byte payload.
// - `out` need not be aligned for this byte API; Vulkan callers passing the
//   result to `VkShaderModuleCreateInfo::pCode` should allocate `uint32_t`-
//   aligned storage.
ARIXV_API arixv_status arixv_decode(const void* in, size_t in_size,
                                    void* out, size_t* out_size,
                                    const arixv_decode_options* opts) ARIXV_NOEXCEPT;

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
typedef struct arixv_encode_options {
  uint32_t struct_size;  // must be in [v1_size, sizeof(arixv_encode_options)]
  uint32_t flags;        // reserved; must be 0
} arixv_encode_options;

// Returns the worst-case encoded size (in bytes) for an input of `in_size`
// bytes with `opts` (NULL for defaults), or 0 if `in_size` exceeds the
// platform's maximum supported uncompressed size or `opts` is invalid.
//
// The buffer passed to `out` in `arixv_encode` must be at least as large as
// the return value of this function when called with matching `in_size` and
// `opts` to guarantee the call will not fail with
// ARIXV_STATUS_BUFFER_TOO_SMALL.
ARIXV_API size_t arixv_encode_bound(size_t in_size,
                                    const arixv_encode_options* opts) ARIXV_NOEXCEPT;

// Compresses `in_size` bytes at `in` into `out` (stateless, thread-safe;
// allocates internal scratch memory). `opts` may be NULL for defaults. `in`
// and `out` must not overlap. `in` may be NULL only when `in_size == 0`.
//
// `out` must be non-NULL; `*out_size` on entry is the capacity of `out` in
// bytes (size it with `arixv_encode_bound(in_size, opts)`). On ARIXV_STATUS_OK,
// `*out_size` is updated to the actual compressed byte count written to `out`.
// If `*out_size` is smaller than the compressed result (possible only if the
// caller provided less than `arixv_encode_bound(in_size, opts)`), leaves `out`
// untouched, writes the exact required size to `*out_size`, and returns
// ARIXV_STATUS_BUFFER_TOO_SMALL.
ARIXV_API arixv_status arixv_encode(const void* in, size_t in_size,
                                    void* out, size_t* out_size,
                                    const arixv_encode_options* opts) ARIXV_NOEXCEPT;
#endif
```

**Size-limit & return-value contract summary:**
- **Format vs platform limits** (guarantees `decode(encode(x))` never rejects
  its own output, with checked overflow-safe arithmetic on both 32-bit and
  64-bit):
  - `ARX2` format maximum uncompressed size: `UINT32_MAX` (4 GiB − 1).
  - Platform maximum addressable uncompressed size: `UINT32_MAX` on 64-bit;
    `PTRDIFF_MAX - 16` (~2 GiB) on 32-bit `ILP32` (so both input and
    stored/flush overhead stay safely within `PTRDIFF_MAX`).
  - `arixv_decode` / `arixv_inspect` accept any representable compressed span
    (`in_size <= PTRDIFF_MAX`), decoupled from `arixv_encode_bound(..., NULL)`
    so coded payloads near the limit are not spuriously rejected. If a valid
    `ARX2` header declares an uncompressed size exceeding the platform's
    addressable limit (e.g. a 3 GiB blob inspected/decoded on 32-bit),
    `arixv_decode` returns `ARIXV_STATUS_TOO_LARGE` (while `arixv_inspect`
    still reports the full `uint64_t uncompressed_size`).

| Function | `out` | Input `*out_size` | Return value | `out` bytes | Output `*out_size` |
|---|---|---|---|---|---|
| `arixv_decode` | `NULL` | ignored | `OK` / error | — | declared uncompressed bytes (on `OK`); unchanged on error |
| `arixv_decode` | non-`NULL` | `>= required` | `OK` (or error) | decoded bytes (unspecified on error) | exact bytes written (unchanged on error) |
| `arixv_decode` | non-`NULL` | `< required` | `BUFFER_TOO_SMALL` | untouched | exact required bytes (for retry) |
| `arixv_encode_bound` | — | — | worst-case encoded bytes (`>= 6`), or `0` if `in_size` out of range or `opts` invalid | — | — |
| `arixv_encode` | non-`NULL` | `>= encoded` | `OK` | compressed bytes | exact compressed bytes written |
| `arixv_encode` | non-`NULL` | `< encoded` | `BUFFER_TOO_SMALL` | untouched | exact encoded bytes needed |
| `arixv_encode` | `NULL` (or invalid `opts`) | — | `INVALID_ARGUMENT` | untouched | unchanged |

**C-boundary invariants, extensibility & testing:**
- **Exception & OOM contract:**
  - All `extern "C"` declarations and definitions use `ARIXV_NOEXCEPT`
    (`noexcept` in C++).
  - When `ARIXV_HAS_EXCEPTIONS` is enabled, wrap every entry point in
    `try { ... } catch (const std::bad_alloc&) { return ARIXV_STATUS_OUT_OF_MEMORY; } catch (...) { return ARIXV_STATUS_OUT_OF_MEMORY; }`
    so neither `bad_alloc` nor `length_error` can ever escape the C boundary.
  - When compiled with `-fno-exceptions` / `/EHs-c-`, document explicitly that
    internal OOM aborts the process in 1.0 (revisitable later via fallible
    internal containers or allocator callbacks on `opts` without changing the
    1.0 API).
- **Forward-compatible struct sizing:** `struct_size` validation accepts any
  size in `[v1_size, sizeof(T)]` and treats omitted tail fields from older
  callers as zero (rejecting sizes `< v1_size` or `> sizeof(T)`).
- Test/benchmark-only `force_coded` stays strictly internal (not exposed in
  `arixv.h`), preserving `arixv_encode_bound` as an unconditional guarantee for
  all valid public options.
- Forward-compatible with Track B (multi-module): `arixv_encode` / `arixv_decode`
  remain cleanly scoped to single-module `ARX2` blobs; `ARIXV_STATUS_UNSUPPORTED_FORMAT`
  distinguishes future `ARXB`/`ARX3` containers cleanly, `arixv_header_info` is
  versioned with `uint64_t uncompressed_size`, and future shader packs
  (`arixv_pack_*`) or optional dictionary/allocator options slot in additively
  without breaking the 1.0 API or ABI.
- Cover hostile inputs, truncated headers, `ARX?` unsupported-format detection,
  `TOO_LARGE` / boundary-size arithmetic checks (without multi-GiB allocations),
  `out == NULL` queries, `BUFFER_TOO_SMALL` retry paths, 0-byte payloads,
  invalid `opts` / prefix `struct_size` validation, `arixv_inspect`, and
  `uint32_t`-aligned Vulkan output buffers.
- Update `cli/`, `tests/`, `tools/bench/`, and `tools/fuzz/` to use the C API
  (internal test/CLI `--force-coded` paths use an internal helper).
- Add strict **C11** and **C++17** consumer smoke tests in CI against both
  static and `BUILD_SHARED_LIBS=ON` builds (linking `arixv::decoder`,
  `arixv::encoder`, and both together in one executable via `FetchContent` and
  `find_package`).

### A2. Decoder-Only Binary Size & Runtime Footprint
Target a lean, focused runtime decoder library for engine integration:
- Measure baseline vs `arixv::decoder` object/binary sizes (`.text`, `.rodata`,
  `.data.rel.ro`, `.data`) under `-O3` and `-Os` (both with
  `ARIXV_BUILD_ENCODER=OFF` and with `ARIXV_BUILD_ENCODER=ON` + linker GC / LTO).
- **Trim unused grammar string tables without ODR risk
  (`scripts/gen_grammar_tables.py`, `src/grammar_tables.*`):**
  - `InstructionDesc::name` (~876 core + hundreds of ext-inst names) and
    `OperandKindDesc::name` (~91 names) are never read anywhere in `src/` (only
    `ExtInstSetDesc::name` is used by `FindExtInstSet`).
  - Remove `name` from `InstructionDesc` and `OperandKindDesc` (keeping struct
    layouts invariant across all builds!) and emit diagnostic names into
    separate parallel lookup tables compiled only for diagnostic/test targets.
- Gate A2 on byte-identical golden vectors.
- Audit any remaining heavy `.rodata` / `.text` contributors in
  `arixv::decoder` and document the final footprint in `README.md`.

### A3. CLI Usability (`cli/main.cpp`)
Keep the CLI focused on offline build-step compression and inspection (and keep
subcommand space clean for future Track B `pack` / `unpack`):
- Commands:
  - `arixv c [--force-coded] [-q|-v] <in> <out>` (plus `-` for stdin/stdout;
    optional ratio summary with `-v`; reject `--force-coded` output if it
    exceeds decoder limits before writing)
  - `arixv c [--force-coded] --out-dir <dir> <files...>` — batch compression
    for build scripts
  - `arixv d <in> <out>` (plus `-` for stdin/stdout)
  - `arixv info <files...>` — O(1) header inspection via `arixv_inspect`
    (magic, mode, compressed size, uncompressed size, ratio)
  - `arixv roundtrip [--force-coded] <files...>` (existing)
  - `arixv --version` / `-h` / `--help` — prints `arixv_version()` (`1.0.0`)
    and container magic (`ARX2`)
- Clear, consistent exit codes and error messages on stderr.

### A4. Format Spec, Documentation Refresh & v1.0 Freeze
- Publish a concise `ARX2` v1.0 format specification (`docs/format-arx2.md`):
  - Container header layout, mode bytes, compact size encoding, payload/model
    rules, frozen SPIRV-Headers revision (`02c0394e…`) + generator tables,
    format vs 32-bit platform limits, and explicit separation between
    **decoder compatibility** (guaranteed forever for `ARX2`) and **encoder
    byte determinism** (verified by golden suite).
- Refresh [`README.md`](../../README.md):
  - Updated benchmark table (post-Part-A ~81–84 MiB/s decode + note on AArch64
    NEON & multi-core scaling).
  - Updated usage examples for the C API (Vulkan two-call decode +
    `VkShaderModuleCreateInfo`, `arixv_encode_bound` + `arixv_encode`,
    `CMakeLists.txt` / `FetchContent` with `arixv::decoder` & `arixv::encoder`,
    pure-C CMake `enable_language(CXX)` / non-CMake `-lstdc++`/`-lc++` note,
    `arixv::decoder` binary size).
  - Explicit statement of C++17 internal implementation, `-fno-exceptions`
    compatibility & OOM contract, 32-bit/64-bit support, and `ARX2` v1.0
    stability.
- Verify full release checklist before tagging `v1.0.0`:
  - All CI jobs green (Linux GCC/Clang SIMD+scalar+`-fno-exceptions`+ASan/UBSan+TSan+32-bit,
    Windows MSVC x64 SIMD+scalar+no-exceptions, macOS AArch64 NEON+scalar,
    strict C11 & C++17 `FetchContent` + `find_package` static &
    `BUILD_SHARED_LIBS=ON` consumer checks, `check-generated`, tuning-override
    grep check).
  - `fuzz-smoke` clean (with memory/RSS limits) on the C API / zero-copy decode
    path + hostile-input tests.
  - Golden `ARX2` test vectors frozen and verified byte-identical across
    x86_64 SSE2, AArch64 NEON, 32-bit, and scalar builds.

---

## Track B: Multi-Module / Shader-Pack Compression (Parallel Research)

Runs on a separate branch (e.g. `research/multi-module`) after Phase 0 lands,
tracked in its own development doc (e.g. `docs/development/multi-module.md`).

### Motivation
Real Vulkan applications ship shader packs / pipeline caches containing dozens
to thousands of SPIR-V modules (permutations, uber-shader variants, shared UBO
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
   - Warm-starting adaptive models (`ModelState` / `StringModel` / opcode &
     enum CDFs) across modules in a group vs resetting per module.
   - Cross-module string dictionary / history sharing.
   - Similarity sorting + structural or byte-level delta / shared-block
     deduplication (e.g., identical type/constant/decoration preambles).
3. **Container & API design:**
   - Distinct container magic (e.g. `ARXB` / `ARX3`), completely additive to
     frozen single-module `ARX2` (1.0 decoders return
     `ARIXV_STATUS_UNSUPPORTED_FORMAT` on newer `ARX?` magics).
   - Design for **chunked / grouped blocks** (e.g. 8–32 related shaders per
     independent block) so engines still get fast random access to individual
     shaders *and* multi-core parallel decode across blocks.
   - Maintain ARIX-V's core principle: purely adaptive / grammar-derived, no
     corpus-trained shipped tables, hardened against hostile inputs.
