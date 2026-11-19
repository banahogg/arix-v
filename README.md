# ARIX-V — SPIR-V arithmetic compressor

ARIX-V (Arithmetic Reduction Intermediate Transform - V) is a standalone,
purely adaptive SPIR-V compressor: a grammar-driven structural model over
adaptive entropy coders (a binary range coder for flags and rANS for
multi-bit symbols). No corpus-trained tables are shipped; all shipped
knowledge is derived from the pinned SPIR-V grammars plus
hand-set scalar constants (`src/tuning.h`).

**Results** (2026-11-18, `scripts/bench/benchmark_spv_compressors.py`,
1070-file decode-verified intersection, 20 decode iterations per file, one
core of a Ryzen 9 5950X; total = Σout/Σin; decode is MiB/s of original SPIR-V
output):

| tool | mean | median | total | decode |
|---|---|---|---|---|
| smol-v | 0.3865 | 0.3831 | 0.3715 | 1236.70 MiB/s |
| yari-v | 0.3395 | 0.3373 | 0.3367 | 730.57 MiB/s |
| zstd -19 | 0.3438 | 0.3330 | 0.2756 | 652.23 MiB/s |
| smol-v + zstd -19 | 0.2647 | 0.2534 | 0.1950 | 622.78 MiB/s |
| **ARIX-V** | **0.2169** | **0.2073** | **0.1667** | **87.12 MiB/s** |

ARIX-V decode was 46 MiB/s on this table in July; the decoder and model-size
work behind the change (rANS symbol coding, cheaper structural paths, and a
~66% smaller per-call model footprint, so stateless `Decode` costs almost no
page faults even in short-lived processes) is recorded in
`docs/development/performance.md`.

ARIX-V round-trips all 1080 corpus files (including the three big-endian
modules the other tools reject) and arbitrary non-SPIR-V bytes (byte-fallback
model), and the decoder is hardened against hostile input (validated
container, bounded allocation, fuzzed; see `tools/fuzz/`).

## Usage

The intended setup is asymmetric: compress shaders offline with the CLI as a
build step, then decompress at runtime by linking the library into your
application.

### Compressing (build time, CLI)

```bash
make build/arixv
build/arixv c shader.spv shader.arixv      # compress
build/arixv d shader.arixv shader.spv      # decompress
build/arixv roundtrip <files...>           # verify round-trip, print sizes
```

### Decompressing at runtime

Use the executable built by the Makefile to compress shaders offline. For
runtime decompression, the supported distribution model is source integration:
copy `src/` into your project and compile its five `.cpp` files with C++17 and
`-Isrc`. There are no runtime dependencies and no package-install or
ABI-stability promise. The checked-in `grammar_tables.cpp` means consumers do
not need the optional grammar-source checkout.

Include `arixv.h` from your application:

```cpp
#include "arixv.h"

std::vector<uint8_t> spirv;
arixv::Status status = arixv::Decode(blob.data(), blob.size(), spirv);
if (status != arixv::Status::kOk) {
  // Corrupt or truncated blob; `spirv` is left empty.
}
```

`Decode` is stateless and thread-safe: each call builds and frees its own
model, so there is nothing to retain between calls or share between threads.

Earlier releases also offered a reusable `arixv::Decoder` that kept model
memory between calls. Stateless `Decode` now runs within ~2% of it, so it was
removed; `arixv_decoder_compat.h` keeps a source-compatible `arixv::Decoder`
that simply forwards to `Decode`, for existing callers. New code should call
`Decode` directly.

`Decode` treats its input as untrusted: the container is validated, output
allocation is bounded by the `max_output` argument (default 1 GiB) rather
than by the untrusted header, and the decoder is fuzzed
(`tools/fuzz/fuzz_roundtrip.cpp`). `arixv::Encode` is also available in the
library for in-process compression.

### Container compatibility

Every ARIX-V blob begins with the four ASCII bytes `ARX2`, followed by its mode
and payload. These four bytes are the entire format-version mechanism: a
decoder accepts only an exact magic match. Any change that can alter encoded
bytes must choose a new four-byte magic; ARIX-V makes no compatibility promise
between different magic values.

The CLI and Makefile are intended for offline compression and development.
Benchmarks, fuzzers, and scripts are development tools; none are required by
an integrating application.

## Layout

- `src/` — the ARIX-V library; the only directory a target application needs.
- `cli/` — the `arixv` command-line tool.
- `tools/bench/` — local benchmark and coverage-report tools.
- `tools/fuzz/` — fuzz harnesses.
- `tests/` — unit tests.
- `scripts/bench/` and `scripts/fuzz/` — development-only automation.
- `docs/` — benchmark-corpus, performance, fuzzing, and tuning notes.
- `deps/` — pinned upstream source lockfiles; fetched inputs are ignored.

## Building and testing

```bash
make all    # build/arixv, build/arixv_bench, build/arixv_decode_bench, build/fuzz_roundtrip
make test   # unit + fuzz smoke; also exercises a corpus if one is present
make asan   # sanitizer build of the test suite
```

`arixv_bench` prints the TSV benchmark protocol and decode-verifies every
file; `scripts/bench/benchmark_spv_compressors.py` builds and includes it
automatically. `ARIXV_STATS=1` dumps per-field-category entropy at exit.
`scripts/gen_grammar_tables.py` regenerates `src/grammar_tables.cpp` from the
pinned SPIRV-Headers JSON grammars; the file is checked in and must only be
changed through the generator. Fetch grammar inputs before running the check:

```bash
python3 scripts/fetch_pinned_repos.py \
  --lock deps/grammar-sources.lock.json --dest grammar-deps
make check-generated
```

## Design notes

ARIX-V is *purely adaptive*: no probability tables trained offline on the
corpus are shipped. All shipped knowledge is structural — derived from the
SPIR-V specification/grammar — plus a small number of hand-set scalar
constants (adaptation shifts, initial probabilities, rank-list lengths, cap
sizes), enumerated in `src/tuning.h` so the tuned surface stays auditable.

The tuning rule: scalar defaults come from spec/implementation reasoning
first; a designated *train split* of the corpus may then be used to choose
among candidate scalars (`scripts/bench/tune_scalars.py`), while a held-out split
stays blind for validation. The corpus is a test set, not a design target —
nothing table-shaped may be derived from it.

## Corpus and benchmarking

The benchmark corpus is generated locally and is not distributed with ARIX-V.
Its pinned upstream sources and their declared license locations are recorded
in `deps/benchmark-sources.lock.json`; see
[`docs/benchmark-corpus.md`](docs/benchmark-corpus.md) before fetching them.
Create the corpus with:

```bash
python3 scripts/fetch_pinned_repos.py
python3 scripts/build_spv_corpus.py
```

Run the reference benchmark with:

```bash
python3 scripts/bench/benchmark_spv_compressors.py
```

It reports mean, median, and total compression ratio plus encode/decode
throughput for ARIX-V and the fetched baseline tools. Decode throughput is
averaged over 20 decode iterations per file by default. To add another
compressor, expose a runner that accepts SPIR-V file paths on the command line
and prints one TSV line per input in this format:

```text
<path>\t<input_bytes>\t<output_bytes>\t<encode_ns>\t<decode_ns>
```

Then add that runner to the `runners` list in
`scripts/bench/benchmark_spv_compressors.py`.

To measure ARIX-V decode throughput on its own, build the decode benchmark:

```bash
make build/arixv_decode_bench
build/arixv_decode_bench --iters=20 corpus/spv/*.spv
```
