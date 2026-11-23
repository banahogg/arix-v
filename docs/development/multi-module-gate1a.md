# Multi-module gate 1a: frozen manifest and generic baselines

Gate 1a of the multi-module research plan (Track B). It runs with no codec
changes. It freezes an exhaustively classified corpus manifest, measures
full-set size baselines with exact reconstruction checks, and runs generic
(zstd/xz) conditional-cost probes. Everything here is size-only. There is no
timing, and nothing here is a container or API proposal.

Snapshot measured: base commit `f0f2af8`, manifest SHA-256
`0b9e61e58bd8e32fc68dd7d9cd398f8a1b650e6d9240f8cfab5cb33177c786a7`,
`baseline_identity_verified: true`.

## Tools

| File | Role |
|---|---|
| `scripts/bench/multi_module_manifest.py` | `build`: verify inputs, classify, write manifest + extras + counts. `validate`: schema/invariant check. `publish`: atomic manifest copy, then ready marker last |
| `scripts/bench/multi_module_baselines.py` | Per-module ARX2/smol-v encodes, pack baselines, prefix marginal curves, conditional costs, verification report, `report.md` |
| `tests/multi_module_baselines_test.py` | Framing/reconstruction, provenance verification, rule table, prefix selection, relationship evidence, publish immutability |

All input locations are command-line arguments. No host path is hard-coded.
Generated data goes under `build/gate1a/` (ignored).

## Inputs and verification

`build` checks the lockfile against `sources.tsv` (revision and URL). It
checks that each dependency checkout HEAD equals the locked revision and that
its tracked files are clean. It checks every canonical corpus file (name, size,
SHA-256) and the `sources.tsv` counts. It checks every duplicate alias against
its canonical file. Finally it checks every logical entry (1340) byte-for-byte
against the pinned upstream file. The manifest's `snapshot` records the
SHA-256 of the lockfile, `manifest.tsv`, `duplicates.tsv` and `sources.tsv`.

## Classification and folds

The rule table is `RULES` in the manifest script. The first match wins. An
unmatched path becomes `unclassified` and stays out of representative packs.
The rule table follows the research plan. Counts per rule are published in
`group_counts.{json,csv}` and in report §1, including rules with no members.

- **Module ids** are `<source>:<input_path>`, one per logical entry. Duplicate
  aliases are separate modules that point at the same canonical file.
- **Collections** are sample plus language for Vulkan-Samples
  (`vks/<sample>/<glsl|hlsl|slang|spirv>`), taken from the directory or the
  sibling source extension. Root-level shaders form `vks/_root` (control).
  GL CTS collections use the documented zink/angle directories. clspv OpenCL
  kernels are separate. Naga remaps are `transform_derived`.
- **Origins** are the documented producer. All Vulkan-Samples share one
  origin. Dota 2 and Blender are separate origins. glslang tests from smol-v
  and SPIRV-Database share `glslang-tests`. Shadertoy, DXC, Naga and the
  fuzzers have origin `null` and use collection holdouts.
- **Folds** start from origin (or from collection when the origin is unknown).
  Folds that share any content are merged by union-find; the validator rejects
  content that crosses folds. Vulkan-Samples and SaschaWillems are also merged,
  because many Khronos samples were ported from that repository.
  `fold_merges` in the extras records the evidence.
- **yari-v** entries (128) are mirrors of the smol-v dumps, with the same names
  and hashes. They join the Dota 2 / Shadertoy folds but no pack.
- **`representative`** is true only for multi-member logical collection packs
  of the `application_derived` / `sample_collection` categories, and for
  stage-pair and variant packs. Test, CTS, example, transform and fuzz
  collections are classified but reported in their own rows.
- **Packs** are logical collection packs in lexicographic logical-key order
  (not a load order), plus a `#unique` companion where duplicates exist.
  - `stage_pair` (202): shader files of different stages referenced within 8
    lines of each other in the sample's C++ source.
  - `variant_family` (14): a root stem plus `root_*` stems of the same stage,
    all referenced within 40 lines in one sample source. Examples are
    `compute_buffer{,_fp16,_fp16_fallback}` and `mlp{,_combo,_morpheus,...}`.
    Every pack's `evidence` field carries the file and line numbers. Rejected
    stem candidates are listed in the extras.
  - Controls: cross-frontend unions, seeded unrelated mixes (n = 8 and 32), and
    singletons.
- **Unavailable tiers:** there is no stage-pair or variant evidence outside
  Vulkan-Samples, and no true family metadata anywhere. Dota 2's numeric names
  are not used to infer families.

The extras file also records, per module: language, debug class (detected
from the actual instructions), entry-point stages, header words, parse errors
and upstream path/revision.

### Prefix cases

Each unique-content target in a classified non-robustness collection gets:

- `none`.
- Each stage-pair or variant prefix (partners only).
- For each strong prefix: three seeded same-collection prefixes matched to its
  bytes (within −20%/+25%, excluding partners), three unrelated prefixes (other
  fold, other known origin), and three prefixes that are both unrelated and
  matched on generator word and SPIR-V version.
- Three seeded same-collection prefixes of about 32 KiB, each with the same
  controls matched to its bytes.

Target content is never in its own prefix. A missing candidate gives an
`unavailable` case with a reason; no substitute is drawn. Seeds come from
`sha256("arixv-gate1a-v1|<case id>")`. The case-id grammar is documented in
the extras `parameters`.

## Baseline methods

Common framing (MMF) is `compact(count) ‖ compact(len_i)… ‖ payloads`, using
ARX2's compact u32 code. Identity is positional in manifest order, so no key
or order-map bytes are charged to any method.

| Method | Definition |
|---|---|
| `arx2_bare_sum` | Sum of standalone ARX2 blobs from the unchanged CLI (default mode selection) |
| `arx2_framed` | MMF(ARX2 blobs) |
| `arx2_shared_header` | `AXS0` (research magic) ‖ count ‖ per member mode, size field, payload length ‖ unchanged payloads. Rebuilds the exact blobs, and every rebuilt blob is decoded by the CLI |
| `arx2_dedup` | Exact-blob dedup: per member a tag/reference, new blobs stored once |
| `arx2_solid_zstd19` | Sanity check: zstd-19 over MMF(ARX2) |
| `orig_perfile_zstd19` | MMF(per-file zstd-19 frames) |
| `orig_solid_zstd19`, `smolv_solid_zstd19` | zstd-19 over MMF(original) or MMF(smol-v) |
| `*_tar_zstd19` (secondary) | Canonical ustar (`%06d` names, mtime/uid/gid 0, mode 0644, 10240 B records) + zstd-19 |
| `*_solid_zstd22w27`, `*_solid_xz9e` (strong references) | `--ultra -22` with window log 27; liblzma preset 9e, CRC64 |

zstd is libzstd 1.5.7 through Python 3.14 `compression.zstd`. Settings:
level 19 with window log 23, or level 22 with window log 27; checksum off,
content size on, dict id off. The effective window is parsed from each frame
header (`zstd_window`). Every pack is under 8 MiB, so the whole set is always
inside the window. xz is liblzma 5.8.3.

A one-module cross-check reproduces the CLI sizes: `xz -9e -T1` gives 1152 B
and `zstd -19 --no-check` gives 1249 B. smol-v is built from the pinned
checkout (verified revision, `kEncodeFlagNone`, no stripping). Each smol-v
blob is decode-verified twice.

Every pack and method row is decompressed or parsed back and compared
byte-for-byte: 6568 verified rows, 0 failures. Twelve rows are unavailable:
the four smol-v methods on each of three packs. Those packs contain three
`fragment_shader_barycentric/hlsl` files that are malformed upstream (1042 and
778 bytes are not word-sized; one has a bad word count). They are kept as
logical entries. ARX2 stores them through its byte fallback. The report's
tables use the common subset of packs.

## Initial results

These are byte-weighted ratios (serialized / original, lower is better). Full
tables with medians are in `report.md`.

| Group | Packs | ARX2 bare | Wrapper | ARX2 dedup | Solid zstd-19 | smol-v + zstd-19 | Solid xz -9e | smol-v + xz -9e |
|---|---|---|---|---|---|---|---|---|
| Dota 2 (110 logical entries) | 1 | 0.149 | 0.149 | 0.130 | 0.064 | 0.062 | **0.050** | 0.057 |
| Blender | 1 | **0.133** | 0.133 | 0.133 | 0.256 | 0.159 | 0.194 | 0.143 |
| Vulkan-Samples collections | 155 | **0.138** | 0.138 | 0.137 | 0.223 | 0.156 | 0.193 | 0.153 |
| GL CTS (zink/angle) | 20 | 0.123 | 0.122 | 0.124 | 0.156 | 0.101 | 0.127 | **0.094** |
| Naga remaps | 1 | 0.139 | 0.138 | 0.112 | 0.129 | 0.086 | 0.105 | **0.076** |
| Shadertoy | 1 | **0.091** | 0.091 | 0.091 | 0.217 | 0.124 | 0.169 | 0.113 |
| Stage-pair packs | 201 | **0.149** | 0.149 | 0.151 | 0.285 | 0.208 | 0.255 | 0.210 |
| Variant-family packs | 14 | 0.083 | 0.083 | 0.083 | 0.166 | **0.075** | 0.135 | **0.070** |

Findings:

1. **Framing and headers are negligible.** The zero-modeling wrapper saves
   about 4 B per member beyond the first, under 0.5% of ARX2 bytes. Minimal
   framing costs about 1–2 B per member. Neither can justify a pack format.
2. **Duplicates account for part of the Dota 2 and Naga gap.** Exact dedup
   brings Dota 2 from 0.149 to 0.130 and Naga from 0.139 to 0.112. The rest of
   the gap is near-duplicate content that solid generic compression captures
   and independent ARX2 does not. On Dota 2, solid xz is 3× smaller than bare
   ARX2: 54.9 KB against 163.8 KB, with every member's original bytes intact.
   This is the clearest cross-module opportunity in the corpus.
3. **Small, diverse packs favor independent ARX2.** Vulkan-Samples collections
   average about 11 KB, stage pairs are typically 2 members, and Shadertoy and
   Blender are large but heterogeneous. For all of these, bare ARX2 beats every
   generic solid method.
4. **GL CTS, Naga and variant families favor smol-v + solid generic
   compression.** It beats bare ARX2 by about 15–45% there. These collections
   contain many near-identical modules.
5. **Conditional costs** are diagnostics with the prefix not charged. Read
   them against the unrelated controls as well as against no prefix.
   - An unrelated prefix already helps (generic SPIR-V boilerplate).
     Same-collection prefixes help more: median xz cost relative to an
     unrelated prefix is 0.68, and relative to no prefix is 0.49.
   - Variant-family prefixes help most: 0.60 relative to unrelated, and 0.86
     relative to a byte-matched same-collection prefix (n = 10).
   - Stage-pair prefixes are no better than byte-matched same-collection
     prefixes (1.02, n = 142). For generic compressors, a pipeline partner
     offers no advantage over any sibling shader.
   - The median spread across seeded same-collection samples is 16 B.
6. **Incremental curves.** The median ratio of solid to per-file zstd falls
   from 0.88 at k = 2 to about 0.42 at k = 32 (xz: 0.79 to 0.34). Few packs are
   large: only 8 at k ≥ 32 and 3 at k ≥ 64, so the large-k points are not
   general.

## Limitations

- Only two application-derived origins are present, and Blender has 9
  modules. One collection cannot establish general suitability. Dota 2's
  results come from one application's dump, not a confirmed shipping set.
- The pinned smol-v checkout contains Talos (100) and Unity (124) SPIR-V
  dumps. They are not named `*.spv`, so `build_spv_corpus.py` skips them.
  Adding them would provide two more application origins; this needs a
  separately versioned corpus and manifest.
- Corpus SPIRV-Database selection is a hash-capped sample (400 of the
  available files; none from bigwheels or nzsl).
- The toolchain-matched control is mostly unavailable (234 of 9144 cases).
  Generator words include the tool version, and Vulkan-Samples' shaderc and
  Slang words appear in no other known origin. The cases are left unavailable
  rather than loosened.
- Stage-pair and variant evidence is source-adjacent references, not parsed
  pipeline objects. Every claim records its file and line.
- Naga remaps re-emit unidentified SPIRV-Database inputs, so leakage between
  folds cannot be excluded for them.
- Conditional costs are generic diagnostics, not bounds on a specialized
  model. Deployable totals must charge the prefix once.
- No figures are generated. The CSVs are ready to plot.

## Rerun

```bash
CORPUS=/path/to/corpus          # manifest.tsv, duplicates.tsv, sources.tsv, spv/
DEPS=/path/to/benchmark-deps    # pinned checkouts per deps/benchmark-sources.lock.json
cmake --preset default && cmake --build --preset default -j4
python3 scripts/bench/multi_module_manifest.py build --corpus "$CORPUS" --deps "$DEPS" \
    --out build/gate1a/manifest
python3 scripts/bench/multi_module_baselines.py --manifest build/gate1a/manifest/manifest.json \
    --arixv build/default/arixv --smolv-dir "$DEPS/smol-v" --out build/gate1a/results --jobs 4
python3 -m unittest tests/multi_module_baselines_test.py
# Publishing (owner only; refuses to overwrite a published snapshot):
python3 scripts/bench/multi_module_manifest.py publish \
    --manifest build/gate1a/manifest/manifest.json \
    --verification build/gate1a/results/baseline_verification.json \
    --manifest-dest "$COORD/manifest.json" --ready-dest "$COORD/manifest.ready.json" \
    --extra build/gate1a/manifest/manifest_extras.json \
    --extra build/gate1a/manifest/group_counts.json --extra build/gate1a/manifest/group_counts.csv
```

`build` is deterministic: a rebuild reproduces the manifest SHA-256 above.
The full baseline run takes about 5 minutes with 4 workers. Corrections go
into a versioned replacement, for example `manifest.v2.json` with
`manifest.v2.ready.json`. A published ready snapshot is never edited.
