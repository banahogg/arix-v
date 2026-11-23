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

## Shader-pack corpus

Multi-module (ARXP pack) benchmarks need whole application shader sets, not
the deduplicated module pool above. Ten such sets, 3,823 modules in total,
are pinned in `deps/pack-corpus-sources.lock.json` and built locally into
`corpus/packs/`, which is ignored like the rest of the corpus:

```bash
python3 scripts/build_pack_corpus.py            # all sets
python3 scripts/build_pack_corpus.py --only xenia,angle --clean
```

The builder makes shallow sparse checkouts of the pinned revisions, and
downloads release artifacts with size and checksum checks, into
`benchmark-deps/packs/`. It then extracts one directory per source and writes:
- `corpus/packs/<set>.list`, the set's modules in pack order (sorted path
  order), relative to the repository root, usable as `@list` inputs to
  `arixv_pack_api_bench` and `arixv pack`;
- `corpus/packs/manifest.tsv`, with per-module sizes and SHA-256 hashes.

It needs `git`, `zstd` (Wicked) and `spirv-as` (Skia) on `PATH`. Rebuilt
output is byte-identical to the sets used for the measurements in
`docs/development/multi-module-*.md`.

| Set | Modules | Bytes | Source | How it is obtained |
|---|---:|---:|---|---|
| `wicked` | 398 | 21,655,344 | Wicked Engine v0.72.139 release (MIT) | zstd frames carved from the `Editor_Linux` binary in `Wicked.Engine.zip` (341 MB) |
| `0ad` | 339 | 1,720,380 | 0 A.D. 0.28.0 release (GPL-2.0+) | `shaders/spirv` from `mod.zip`, streamed out of the 1.4 GB data tarball; the XML permutation maps are kept beside the modules |
| `sascha_glsl` / `_hlsl` / `_slang` | 350 / 308 / 347 | 877,720 / 774,056 / 929,376 | SaschaWillems/Vulkan (MIT) | committed `.spv` from glslang, DXC and Slang |
| `xenia` | 92 | 1,168,876 | Xenia emulator (BSD-3) | C headers of uint32 words |
| `angle` | 149 | 449,396 | ANGLE Vulkan back end (BSD-3) | gzip-compressed C byte arrays |
| `quake3e` | 74 | 238,516 | Quake3e Vulkan renderer (GPL-2.0) | C byte arrays in `shader_data.c` |
| `lumos` | 82 | 1,227,204 | Lumos engine (MIT) | committed shaderc `.spv` |
| `skia` | 352 | 912,104 | Skia SkSL goldens (BSD-3) | `.asm.*` goldens assembled with `spirv-as --target-env spv1.3` (28 of 380 do not assemble) |

The sets come from a survey of public SPIR-V sources, recorded with ratings,
licenses, other candidates and dead ends in
[`development/pack-corpus-search.md`](development/pack-corpus-search.md).
Release artifacts can disappear upstream. If the Wicked or 0 A.D. download is
gone, keep an existing `corpus/packs/` copy: the manifest hashes identify it.
