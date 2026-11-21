# Agent notes

## Build and test

- `make all` builds the CLI, benchmarks, and the deterministic fuzzer.
- `make test` runs the unit tests and a 500-iteration smoke fuzz. Run it
  after every change.
- `make asan` runs the unit tests under ASan/UBSan.

## Coverage-guided fuzz smoke (`make fuzz-smoke`)

Run `make fuzz-smoke` after major changes to the model (`src/model.cpp`) or
the tracker (`src/tracker.*`), and before pushing. It runs the libFuzzer
round-trip and hostile-decode harnesses under ASan/UBSan for about five
minutes in total, and exits non-zero on any crash or sanitizer report.

`make test`'s 500-iteration fuzz is too short to catch encoder bugs that need
a specific malformed module. For example, an `OpExtInst` whose set operand
named a struct type crashed the encoder. The plain fuzzer needed 1.9k–37k
iterations to hit it, while `fuzz-smoke` found it in 42–75 s on 32 cores.

- Needs `clang++` and the SPIR-V corpus in `corpus/spv` (see
  `docs/development/fuzzing.md`).
- `FUZZ_SMOKE_SECONDS` sets the time per harness (default 120).
  `FUZZ_SMOKE_JOBS` sets the number of processes (default `nproc`). On
  machines with few cores, raise the time.
- Crash inputs land in `build/fuzz-smoke/<rt|dec>/artifacts/`. Reproduce one
  with `build/fuzz_roundtrip_lf <file>` or `build/fuzz_decode_lf <file>`.
  Add a minimal version of the input to `tests/model_test.cpp` as a
  regression test when fixing it.
- For long campaigns (AFL++, cmplog, -m32, valgrind), use
  `scripts/fuzz/overnight.sh`.
