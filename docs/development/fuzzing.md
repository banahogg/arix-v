# Fuzzing

The coverage-guided and long-running fuzzing workflow is development-only. It
is intentionally separate from `make test`, which runs a deterministic smoke
fuzzer without external tooling or a downloaded corpus.

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
the launched jobs.

Some sanitizer environments need custom process wrapping. For example,
`RUN_WRAP="setarch $(uname -m) -R" scripts/fuzz/overnight.sh` applies a wrapper
only to that campaign. Likewise, `make ASAN_RUN='…' asan` supplies an opt-in
wrapper for the sanitizer test target.
