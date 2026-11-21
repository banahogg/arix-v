# Fuzzing

The coverage-guided and long-running fuzzing workflow is development-only. It
is intentionally separate from `make test`, which runs a deterministic smoke
fuzzer without external tooling or a downloaded corpus.

For a quick coverage-guided check, run `make fuzz-smoke`. It needs
`clang++` and the corpus. It runs the libFuzzer round-trip and decode harnesses
(`tools/fuzz/*_cov.cpp`) under ASan/UBSan for `FUZZ_SMOKE_SECONDS` (default
120) each, on `FUZZ_SMOKE_JOBS` processes (default `nproc`). It is
recommended after major model or tracker changes and before pushing. Its
working corpora persist in `build/fuzz-smoke/` between runs. It seeds from the
whole corpus rather than the size-capped overnight seeds: with the <33 KiB cap
it missed a known encoder crash in 120 s, while with full seeds it found the
crash in 42–75 s on 32 cores. `FUZZ_SMOKE_SEED_KB` restores a cap.

To run the overnight campaign, first generate the optional corpus, then run:

```bash
python3 scripts/fetch_pinned_repos.py
python3 scripts/fetch_pinned_repos.py \
  --lock deps/grammar-sources.lock.json --dest grammar-deps
python3 scripts/build_spv_corpus.py
scripts/fuzz/overnight.sh
```

The campaign requires AFL++, Clang, Valgrind, multilib GCC/G++, and optionally
SPIRV-Tools for valid-module mutation. Results live in ignored `fuzz-out/`.
Use `scripts/fuzz/triage.sh` for a report and `scripts/fuzz/stop.sh` to stop
the launched jobs. For a time-boxed run, chain the stop and triage after a
sleep, e.g.
`scripts/fuzz/overnight.sh && nohup sh -c 'sleep 2h; scripts/fuzz/stop.sh; scripts/fuzz/triage.sh' >/dev/null 2>&1 &`.

### spirv-fuzz

The generator loop uses `spirv-fuzz` when it is on `PATH`, and otherwise only
`spirv-opt` pass pipelines. Distribution `spirv-tools` packages (Ubuntu
included) build without it, so build it from source. Upstream `spirv-fuzz`
validates every input as universal SPIR-V 1.3, which rejects the corpus's
SPIR-V 1.4–1.6 modules (about a third of it).
`scripts/fuzz/spirv-fuzz-target-env.patch` makes it use each input module's
own version instead:

```bash
git clone https://github.com/KhronosGroup/SPIRV-Tools.git ~/src/spirv-tools
cd ~/src/spirv-tools && python3 utils/git-sync-deps
git apply <arix-v checkout>/scripts/fuzz/spirv-fuzz-target-env.patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSPIRV_BUILD_FUZZER=ON -DSPIRV_SKIP_TESTS=ON
ninja -C build spirv-fuzz && cp build/tools/spirv-fuzz ~/.local/bin/
```

`spirv-fuzz` occasionally crashes on a corpus module (an upstream bug); the
loop then falls back to `spirv-opt` for that iteration.

Some sanitizer environments need custom process wrapping. For example,
`RUN_WRAP="setarch $(uname -m) -R" scripts/fuzz/overnight.sh` applies a wrapper
only to that campaign. Likewise, `make ASAN_RUN='…' asan` supplies an opt-in
wrapper for the sanitizer test target.
