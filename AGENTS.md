# Agent notes

## Build and test

- `cmake --preset default && cmake --build --preset default -j` builds the
  libraries (`arixv::decoder`, `arixv::encoder`), CLI, benchmarks, unit tests,
  and deterministic fuzzer.
- `ctest --preset default` runs the unit tests, golden vector checks,
  `check_generated` (when `grammar-deps/` is present), and a 500-iteration smoke
  fuzz. Run it after every change.
- `cmake --preset release && cmake --build --preset release -j && ctest --preset release`
  builds and tests optimized Release + LTO binaries.
- `cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan`
  runs the test suite under ASan/UBSan.
- `cmake --preset tsan && cmake --build --preset tsan -j && ctest --preset tsan`
  runs the test suite under ThreadSanitizer.
- `cmake --preset decoder-only && cmake --build --preset decoder-only -j && ctest --preset decoder-only`
  verifies the decoder-only build (`ARIXV_BUILD_ENCODER=OFF`).
- `-DARIXV_BUILD_PACK=OFF` builds without the shader-pack API (a 1.0-sized
  decoder). CI checks that its shared library exports only the 1.0 symbols;
  check it locally after changing pack gating.

## Coverage-guided fuzz smoke

Run coverage-guided fuzz smoke after major changes to the model
(`src/model.cpp`), the tracker (`src/tracker.*`) or the pack code
(`src/pack_block.inc`, `src/arixv_pack*.cpp`), and before pushing:

```bash
cmake --preset fuzz-smoke
cmake --build --preset fuzz-smoke --target fuzz-smoke -j
```

It runs the libFuzzer round-trip and hostile-decode harnesses, for single
modules (`rt`, `dec`) and for shader packs (`pack-rt`, `pack-dec`), under
ASan/UBSan for about nine minutes in total, and exits non-zero on any crash
or sanitizer report.

CTest's 500-iteration fuzz is too short to catch encoder bugs that need a
specific malformed module. For example, an `OpExtInst` whose set operand named
a struct type crashed the encoder. The plain fuzzer needed 1.9k–37k iterations
to hit it, while `fuzz-smoke` found it in 42–75 s on 32 cores.

- Needs `clang++` and the SPIR-V corpus in `corpus/spv` (see
  `docs/development/fuzzing.md`).
- `FUZZ_SMOKE_SECONDS` sets the time per harness (default 120).
  `FUZZ_SMOKE_JOBS` sets the number of processes (default `nproc`). On
  machines with few cores, raise the time.
- Crash inputs land in
  `build/fuzz-smoke/fuzz-smoke/<rt|dec|pack-rt|pack-dec>/artifacts/`.
  Reproduce one with `build/fuzz-smoke/fuzz_roundtrip_lf <file>`,
  `fuzz_decode_lf`, `fuzz_pack_rt_lf` or `fuzz_pack_dec_lf`. Add a minimal
  version of the input to `tests/model_test.cpp` (or `tests/pack_test.cpp`
  for packs) as a regression test when fixing it.
- For long campaigns (AFL++, cmplog, -m32, valgrind), use
  `scripts/fuzz/overnight.sh`.
