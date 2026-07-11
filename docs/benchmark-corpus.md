# SPIR-V reference corpus

This document describes the benchmark corpus for compressor speed and ratio
tests. The binary corpus itself is generated locally in `corpus/spv/` and is
intentionally not distributed with ARIX-V.

The exact upstream URLs and revisions are pinned in
`deps/benchmark-sources.lock.json`. The lockfile also records the license file each
upstream project declares where known. Fetching a source does not replace a
review of its upstream terms; in particular, SPIRV-Database has no license
file recorded in the lockfile and is fetched only on demand.

Fetch the inputs directly from their upstream repositories, at the pinned
revisions:

```bash
python3 scripts/fetch_pinned_repos.py
```

The builder deduplicates by SHA-256 and stores canonical files in
`corpus/spv/`. It writes `manifest.tsv`, `duplicates.tsv`, and `sources.tsv`
to `corpus/`; these generated provenance files are ignored along with the
corpus itself. Existing output requires `--clean`, preventing an accidental
mixture of corpus snapshots.

Build the corpus with:

```bash
python3 scripts/build_spv_corpus.py
```
