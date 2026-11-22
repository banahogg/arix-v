# ARIX-V — SPIR-V Arithmetic Compressor

ARIX-V (*Arithmetic Reduction Intermediate Transform - V*) is a standalone,
purely adaptive SPIR-V compressor with a thin C99/C11 public API, zero runtime
dependencies, and a lean decoder-only target (~142–189 KB) designed for direct
engine and Vulkan runtime integration.

No corpus-trained probability tables are shipped: all shipped knowledge is
derived structurally from the pinned Khronos SPIR-V specification grammars plus
a small audited set of scalar constants (`src/tuning.h`).

## Benchmarks

**Results** (2026-11-21, `scripts/bench/benchmark_spv_compressors.py`,
1070-file decode-verified intersection, 20 decode iterations per file, single
core of an AMD Ryzen 9 5950X; `total = Σout / Σin`; decode is MiB/s of
uncompressed SPIR-V output):

| Tool | Mean | Median | Total | Single-Core Decode |
|---|---:|---:|---:|---:|
| smol-v | 0.3865 | 0.3831 | 0.3715 | 1249.77 MiB/s |
| yari-v | 0.3395 | 0.3373 | 0.3367 | 731.30 MiB/s |
| zstd -19 | 0.3438 | 0.3330 | 0.2756 | 619.94 MiB/s |
| smol-v + zstd -19 | 0.2647 | 0.2534 | 0.1950 | 591.29 MiB/s |
| **ARIX-V (`ARX2`)** | **0.1561** | **0.1539** | **0.1249** | **73.88 MiB/s** |

- **SIMD & multi-core scaling:** rANS symbol updates use x86_64 SSE2 and
  AArch64 NEON with a portable scalar fallback (`-DARIXV_NO_SIMD`). Because
  `arixv_decode` is completely stateless and thread-safe, decoding a shader
  pack across worker threads scales linearly with core count.
- **Full lossless coverage:** ARIX-V round-trips all 1,080 corpus shaders
  (including big-endian modules rejected by other tools), arbitrary non-SPIR-V
  byte streams (byte-coded or stored fallback), and SPIR-V modules using
  opcodes or enumerants from newer SPIR-V revisions.
- **Hardened decoder:** Validated container framing, bounded internal
  reservations against untrusted headers, and continuous libFuzzer + ASan/UBSan
  testing (`tools/fuzz/`).
- Development logs covering ratio and CPU optimization live in
  [`docs/development/performance.md`](docs/development/performance.md),
  [`docs/development/ratio.md`](docs/development/ratio.md), and
  [`docs/development/perf-post-ratio.md`](docs/development/perf-post-ratio.md).

## Integration (CMake & Source Copy)

The intended workflow is asymmetric: compress shaders offline at build time
with the `arixv` CLI (or `arixv::encoder`), and link only the lean
`arixv::decoder` target into your runtime application.

### CMake (`FetchContent` or `find_package`)

ARIX-V requires CMake ≥ 3.21 and exposes two namespaced targets:

- `arixv::decoder` — runtime decoder (`arixv_version`, `arixv_status_string`,
  `arixv_inspect`, `arixv_decode`). Built by default (`ARIXV_BUILD_ENCODER`
  defaults to `OFF` when consumed as a subproject).
- `arixv::encoder` — adds `arixv_encode_bound` and `arixv_encode` (enable with
  `-DARIXV_BUILD_ENCODER=ON`).

```cmake
include(FetchContent)
FetchContent_Declare(
  arixv
  GIT_REPOSITORY https://github.com/banahogg/arix-v.git
  GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(arixv)

target_link_libraries(my_engine PRIVATE arixv::decoder)
```

Or against an installed package (`cmake --install ...`):

```cmake
find_package(arixv REQUIRED)
target_link_libraries(my_engine PRIVATE arixv::decoder)
# Optional: target_link_libraries(my_tool PRIVATE arixv::encoder)
```

Pure-C CMake projects (`project(app LANGUAGES C)`) work out of the box for both
`find_package(arixv)` and `FetchContent` (static and `BUILD_SHARED_LIBS=ON`):
CMake automatically propagates the C++ runtime linkage required by ARIX-V's
internal C++17 implementation.

### Direct Source-Copy Integration (No Build System Required)

Copy `include/arixv/arixv.h` and `src/` into your project and compile the five
decoder translation units with C++17 (`-Iinclude -Isrc`):

- `src/arixv.cpp`
- `src/model.cpp`
- `src/tracker.cpp`
- `src/operand_cursor.cpp`
- `src/grammar_tables.cpp`

To include the encoder as well, also compile `src/arixv_encode.cpp` and define
`ARIXV_ENCODER=1` (in decoder-only source builds, define `ARIXV_ENCODER=0` if
you want encoder declarations hidden from `arixv/arixv.h`). Do **not** compile
`src/grammar_names.cpp` into runtime binaries — it contains diagnostic-only
name tables for tests and analysis tools.

When linking static ARIX-V object files into a pure-C executable outside CMake,
link the platform C++ runtime (`-lstdc++` on GCC/libstdc++, `-lc++` on
Clang/libc++ or macOS).

## Public C API (`include/arixv/arixv.h`)

The public header is pure C99/C11 and C++ compatible, and shared builds export
strictly the `arixv_*` C symbols.

### Vulkan Zero-Copy Decode (Two-Call Pattern)

`arixv_decode` decodes directly into caller-owned memory with no extra copy.
Pass `uint32_t`-aligned storage when forwarding the result to
`VkShaderModuleCreateInfo`:

```c
#include <arixv/arixv.h>
#include <stdlib.h>

/* 1. O(1) size query from the untrusted container header. */
size_t spirv_bytes = 0;
arixv_status st = arixv_decode(blob, blob_size, NULL, &spirv_bytes, NULL);
if (st != ARIXV_STATUS_OK || spirv_bytes == 0 ||
    spirv_bytes > MAX_SHADER_BYTES || (spirv_bytes % 4) != 0) {
  return handle_error(st);
}

/* 2. Allocate uint32_t-aligned output and decode directly into it. */
uint32_t* spirv = (uint32_t*)malloc(spirv_bytes);
if (!spirv) {
  return handle_oom();
}
size_t out_size = spirv_bytes;
st = arixv_decode(blob, blob_size, spirv, &out_size, NULL);
if (st != ARIXV_STATUS_OK) {
  free(spirv);
  return handle_error(st);
}

VkShaderModuleCreateInfo ci = {
  .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
  .codeSize = out_size,
  .pCode = spirv,
};
```

For hot shader-loading loops, you can also decode optimistically in a single
call into a reusable scratch buffer: if `arixv_decode` returns
`ARIXV_STATUS_BUFFER_TOO_SMALL`, `*out_size` is updated to the exact required
size without modifying the buffer, ready for a resize-and-retry.

### Encoding (`arixv::encoder`)

```c
#include <arixv/arixv.h>
#include <stdlib.h>

size_t bound = arixv_encode_bound(spirv_bytes, NULL);
if (bound == 0) {
  return handle_too_large();
}
void* out = malloc(bound);
size_t out_size = bound;
arixv_status st = arixv_encode(spirv, spirv_bytes, out, &out_size, NULL);
/* On ARIXV_STATUS_OK, `out_size` bytes were written to `out`. */
```

### O(1) Header Inspection

```c
arixv_header_info info = {0};
info.struct_size = sizeof(info);
if (arixv_inspect(blob, blob_size, &info) == ARIXV_STATUS_OK) {
  /* info.magic, info.mode (ARIXV_MODE_*), info.uncompressed_size */
}
```

## CLI Usage

Build the CLI with the `release` preset and use it as an offline build step:

```bash
cmake --preset release
cmake --build --preset release --target arixv_cli -j

# Single-file compress / decompress (use '-' for binary stdin/stdout)
build/release/arixv c -v shader.spv shader.arixv
build/release/arixv d shader.arixv shader.spv
cat shader.spv | build/release/arixv c - - | build/release/arixv d - shader.spv

# Batch compress a directory of shaders into out/ (<filename>.arixv)
build/release/arixv c --out-dir build/shaders shaders/*.spv

# O(1) header inspection (reads only the first 16 bytes of regular files)
build/release/arixv info build/shaders/*.arixv

# Round-trip verification and version info
build/release/arixv roundtrip shaders/*.spv
build/release/arixv --version
```

Exit codes: `0` success, `1` runtime/codec/I/O error, `2` CLI usage error.

## Decoder Binary Footprint

`arixv::decoder` carries zero unused grammar name strings (diagnostic names
live in the separate test-only `src/grammar_names.cpp`) and stores all SPIR-V
grammar tables as pure position-independent `.rodata` with zero `.data.rel.ro`
entries.

Measured on Linux x86_64 (GCC 15, `ARIXV_BUILD_ENCODER=OFF`):

| Build | `.text` | `.rodata` | `.data.rel.ro` | Total (`size`) | Stripped `.so` |
|---|---:|---:|---:|---:|---:|
| Shared `-O3` (`Release`) | 93.2 KB | 83.9 KB | 0 B | 188.9 KB | 194.9 KB |
| Shared `-O3` + LTO | — | — | 0 B | 182.6 KB | 186.7 KB |
| Shared `-Os` (`MinSizeRel`) | 44.4 KB | 84.1 KB | 0 B | 142.2 KB | 149.9 KB |
| Shared `-Os` + LTO | — | — | 0 B | 135.4 KB | 145.8 KB |

Static links with `-Wl,--gc-sections` against `arixv::decoder` achieve the
same footprint even when configured with `ARIXV_BUILD_ENCODER=ON` (~178 KB at
`-O3`, ~143 KB at `-Os`).

## Portability, Exceptions & Format Stability

- **Internal implementation:** C++17 internally behind a stable C99/C11 ABI
  (`include/arixv/arixv.h`), verified across Linux (GCC/Clang), macOS (AArch64
  NEON), and Windows (MSVC x64).
- **Exceptions & OOM contract:** Compiles cleanly with `-fno-exceptions
  -fno-rtti` (GCC/Clang) and `/EHs-c- /D_HAS_EXCEPTIONS=0 /GR-` (MSVC). When
  built with C++ exceptions enabled, internal allocation failures are caught at
  the C boundary and returned as `ARIXV_STATUS_OUT_OF_MEMORY`. When built with
  exceptions disabled, internal OOM aborts the process in v1.0.
- **32-bit & 64-bit support:** Tested on both 64-bit (`LP64`/`LLP64`, up to
  `UINT32_MAX` uncompressed bytes) and 32-bit (`ILP32`, up to
  `PTRDIFF_MAX - 16` bytes; valid larger blobs return `ARIXV_STATUS_TOO_LARGE`
  from `arixv_decode` while `arixv_inspect` still reports the full 64-bit size).
- **Format stability (`ARX2`):** The `ARX2` format is frozen for v1.0,
  including the pinned `SPIRV-Headers` revision (`02c0394e…`) and all wire
  constants, backed by an immutable cross-architecture golden fixture suite in
  `tests/golden/`. See [`docs/format-arx2.md`](docs/format-arx2.md) for the
  full specification.

## Repository Layout

- `include/arixv/arixv.h` — Public C99/C11 + C++ header.
- `src/` — ARIX-V library implementation and checked-in generated grammar tables.
- `cli/` — `arixv` command-line tool.
- `tests/` — Unit, golden-vector, CLI, and strict C11/C++17 consumer smoke tests.
- `tools/bench/` and `tools/fuzz/` — Benchmarks, diagnostics, and fuzz harnesses.
- `scripts/` — Grammar table generator, corpus builder, and benchmark runners.
- `docs/` — [`format-arx2.md`](docs/format-arx2.md), corpus docs, and development notes.
- `deps/` — Pinned upstream source lockfiles (fetched inputs are gitignored).

## Building and Testing

```bash
# Configure, build, and run CTest with CMake presets
cmake --preset release && cmake --build --preset release -j && ctest --preset release
cmake --preset decoder-only && cmake --build --preset decoder-only -j && ctest --preset decoder-only
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan -j && ctest --preset tsan
```

To verify that the checked-in grammar tables match the pinned Khronos JSON
grammars:

```bash
python3 scripts/fetch_pinned_repos.py \
  --lock deps/grammar-sources.lock.json --dest grammar-deps
python3 scripts/gen_grammar_tables.py \
  --grammar-dir grammar-deps/spirv-headers/include/spirv/unified1 --check
```

## Corpus and Benchmarking

The benchmark corpus is generated locally and is not distributed with ARIX-V;
see [`docs/benchmark-corpus.md`](docs/benchmark-corpus.md) before fetching
upstream sources:

```bash
python3 scripts/fetch_pinned_repos.py
python3 scripts/build_spv_corpus.py
python3 scripts/bench/benchmark_spv_compressors.py
```

To measure ARIX-V decode throughput standalone:

```bash
cmake --preset release && cmake --build --preset release --target arixv_decode_bench -j
./build/release/arixv_decode_bench --iters=20 corpus/spv/*.spv
```
