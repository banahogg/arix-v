# Shader-pack corpus search (2026-11-22)

> Provenance record of the survey behind the shader-pack corpus. The sets
> rated highest and used for the gate-4 measurements are pinned in
> `deps/pack-corpus-sources.lock.json` and rebuilt by
> `scripts/build_pack_corpus.py` into `corpus/packs/`
> ([benchmark corpus](../benchmark-corpus.md)). Paths below under
> `build/corpus-search/raw/` are where the search agent staged them; that
> staging area no longer exists. The analysis scripts mentioned were
> one-off and were not kept; the build script reimplements the extraction.


Date of search: 2026-11-22. Nothing in the arix-v repository was modified (no source, docs or git state).
All downloads are under `build/corpus-search/raw/` (about 285 MB in total; the 0 A.D. release stream was 1.4 GB transferred but only `mod.zip` was kept).
Analysis scripts live outside that directory (session scratchpad) and were run with `python3 -I`.

## How facts were verified

* **"Committed .spv?"** was checked with blob-less shallow clones (`git clone --depth 1 --filter=blob:none --no-checkout`) followed by `git ls-tree -r HEAD`. This is a list of tracked paths at one revision, not a content scan.
* **Staged sets** were parsed word by word. I checked the magic number, the generator ID in word 2, and the instruction stream; the stage comes from `OpEntryPoint`; debug info means `OpName`/`OpLine` is present.
* **"xz concat/each"** is `xz -9e(concatenated modules in path order) / sum(xz -9 of each module)`. It is a quick proxy for cross-module redundancy (lower means more pack-level gain). Values are only comparable within this report.
* **"nn-Jaccard≥0.9"** is the number of modules whose `OpName`-string set has a sibling in the same set with Jaccard similarity ≥ 0.9. It is a cheap indicator of a permutation family. It is not available for stripped sets.
* Generator IDs seen: 0x7 = shaderc/glslang-via-shaderc (as spirv-as stamps too), 0x8 = glslang, 0xd = shaderc (glslc), 0xe = DXC, 0x28 = Slang.
* Everything marked **(unverified)** was not confirmed by me.
* Several web-search summaries were thin or wrong about repository contents. Every repository claim in this report comes from a tree listing or a download, not from search snippets.

## Ranked summary

| # | Candidate | What it is | Modules / bytes | Producer | Debug / stripped | Families | Staged | License (binaries) | Rating |
|---|---|---|---|---|---|---|---|---|---|
| 1 | **Wicked Engine** release binary (`Editor_Linux`) | Complete shipped shader set of an open-source engine, extracted from its release binary | 398 (390 unique) / 21.66 MB | DXC | debug names, no OpLine | 205/398 have a ≥0.9 sibling; xz concat/each 0.64 | Yes | MIT | **5** |
| 2 | **0 A.D. 0.28.0** `mod.zip` `shaders/spirv` | Real game, shipped Vulkan SPIR-V set (339 unique shaders, 489 program XMLs mapping permutations to hashes) | 339 / 1.72 MB | shaderc glslc | stripped | shaders are content-hash named, so already deduplicated; xz 0.42 | Yes | GPL-2+ (code); shaders are code | **4** |
| 3 | **SaschaWillems/Vulkan** (committed .spv) | Same example set compiled by glslang, DXC and Slang | 1005 (872 unique) / 2.58 MB | glslang 350, DXC 308, Slang 347 | debug names (OpName) | 500/1005 have a ≥0.9 sibling; xz 0.38; stages include RT, mesh/task, tess, geom | Yes | MIT | **4** |
| 4 | **Xenia** `vulkan_spirv` | Emulator GPU shaders (texture loads, resolves, gamma, etc.) | 92 / 1.17 MB | glslang | debug names | 90/92 have a ≥0.9 sibling; xz 0.18 (best family structure of all) | Yes | BSD-3 | **3.5** |
| 5 | **ANGLE** Vulkan internal shaders (`shaders/gen/*.inc`) | 149 pre-generated permutations of ~15 shaders (ImageClear ×48, ImageCopy ×36, BlitResolve ×24, GenerateMipmap ×10...) | 149 / 449 KB | glslang | stripped | xz 0.34; names stripped, so no Jaccard | Yes (converted) | BSD-3 | **3.5** |
| 6 | **Quake3e** `renderervk/shaders/spirv/shader_data.c` | Q3 engine permutations: tx0/tx1/tx2 × fog/env/fixed/ident1/cl | 74 (66 unique) / 239 KB | glslang | debug names | 36/74 ≥0.9; xz 0.30 | Yes (converted) | GPL-2 (unverified, from id's Q3 code base) | 3 |
| 7 | **Lumos** engine `Assets/Shaders/CompiledSPV` | Engine's committed shaderc output | 82 / 1.23 MB | shaderc | debug names | 18/82 ≥0.9; xz 0.61 | Yes | MIT | 3 |
| 8 | **Skia** SkSL golden `.asm.*` (assembled with spirv-as) | SkSL-compiler output, text goldens | 352 of 380 assemble (342 unique) / 0.91 MB | SkSL (re-stamped by spirv-as) | debug names | 166/352 ≥0.9; xz 0.39 | Yes (assembled) | BSD-3 | 2.5 (test suite) |
| 9 | Small app sets: **RetroArch** menu shaders, **Ryujinx** (Ryubing), **Granite** CACAO, **vkQuake2**, **Vita3K** builtins, **DXVK/vkd3d-proton** internal shaders | See details | 14–44 each | various | mixed | mostly weak | Yes | GPL-3 / MIT / MIT / GPL-2 / GPL-3 / zlib, LGPL-2.1 | 2 |
| 10 | **llama.cpp ggml-vulkan** (recipe only) | Compute permutation factory: at least 242 `string_to_spv` call sites, many in loops over quant types (est. >1000 modules, unverified) | n/a | glslc | n/a | Very strong by construction | No | MIT (source) | 4 if a build is acceptable |
| 11 | Larger controls (control set candidates) | Tint test corpus (13,131 `.spvasm`), SPIRV-Reflect tests (128 modules, six producers), SPIRV-LLVM-Translator (105 OpenCL .spv) | see details | mixed | mixed | n/a | SPIRV-Reflect only | Apache-2.0 | 1–2 |

Answer to the headline question: **no repository I could find commits a large, complete, application-derived .spv set.** The best real-application sets come from (a) shipped release artifacts (Wicked, 0 A.D.) and (b) a handful of engines that commit generated shaders (Xenia, ANGLE, Quake3e, Lumos). Most engines build SPIR-V at build time or at runtime and commit nothing.

## Staged data (all under `build/corpus-search/raw/`)

| Directory | Contents | Modules | Bytes | Source revision |
|---|---|---|---|---|
| `wicked-linux-editor-carved/` | zstd frames decoded from `Editor_Linux` in release v0.72.139 | 398 | 21,655,344 | release asset `Wicked.Engine.zip` (341 MB, published 2026-11-03) |
| `sascha/shaders/{glsl,hlsl,slang}` | committed `.spv` (sparse clone, shaders only) | 1005 | 2,581,152 | `d2d49bf7f2d8469ad9fee47b2a43b430d7e10e1b` (2026-11-09) |
| `0ad-shaders-spirv/` | `shaders/spirv/*.spv` (+489 `.xml`/`.xmb` mapping files) from `mod.zip`; the 93 MB `mod.zip` is under `0ad-0.28.0/` | 339 | 1,720,380 | release `0ad-0.28.0-unix-data.tar.xz` (1,415,012,652 bytes) |
| `xenia/` and `xenia-converted-spv/` | C headers (`#if 0` disassembly + uint32 arrays) and converted binaries | 92 | 1,168,876 | `95a5c3ee250f80c3b9d139658649d9ffb6db3eec` |
| `angle/` and `angle-converted-spv/` | gzip-in-C-array `.inc` files and the decompressed binaries | 149 | 449,396 | `b7a90a6cc08e52857a6addc2bc2e569b7b01e9df` (2026-11-20) |
| `quake3e-src/` and `quake3e-converted-spv/` | one C file with 74 byte arrays and converted binaries | 74 | 238,516 | `dec793de9677bc9c16c7027e8905c2d4f4643a19` |
| `lumos/Lumos/Assets/Shaders/CompiledSPV` | committed `.spv` | 82 | 1,227,204 | `50ac714464ee0576a6b741c31ac607231edc5a2c` |
| `skia/` and `skia-assembled-spv/` | `tests/sksl` golden `.asm.*` and spirv-as output | 352 | 912,104 | `4802f18c7cd248700dd6f813d5299e9b8759f1e9` |
| `retroarch/`, `retroarch-converted-spv/` | `gfx/drivers/vulkan_shaders/*.inc` | 36 | 197,888 | `3e771e7d074f9692464063fd91a9da3f5eef5c42` |
| `ryujinx/` | `src/Ryujinx.Graphics.Vulkan/{Effects/Shaders,Shaders/SpirvBinaries}` | 33 | 193,920 | Ryubing `6af327bdd50366163d80bc692f6bf47ec1c1993e` |
| `granite/` | `assets/shaders/post/ffx-cacao` | 33 (all compute) | 446,412 | `69f450c62dc3780680b936539a931b5d03e436fb` |
| `vkquake2/`, `vkquake2-converted-spv/` | `ref_vk/spirv/*.c` | 23 | 32,924 | `6763f207229f97cffabb6fc2da72017a794b139b` |
| `vita3k/` | `vita3k/shaders-builtin` | 14 | 122,396 | `64e655dd797153d0c5a5cbf734634be31288168d` |
| `spirv-reflect/` | `tests/` | 128 (124 unique) | 344,344 | `6002a58dfc83cad60f206c5fc99b5a39d1623dcb` |
| `dxvk-3.1.1-release/`, `dxvk-carved/` | magic-scan of x64 DLLs | 30 | 365,992 | release v3.1.1 |
| `vkd3d-proton-3.1-release/`, `vkd3d-proton-carved/` | magic-scan of x64 DLLs | 44 | 227,000 | release v3.1 |
| `wgpu-naga/` | `naga/tests/{in,out}/spv` (text `.spvasm` and a few binaries; not analysed further) | n/a | n/a | wgpu `38bb981557886b40fdd338b7f9f052032e417cb6` |

Validation status: converted/carved modules were parsed structurally (valid instruction streams), not fully validated. I ran `spirv-val` only on the carved DXVK and vkd3d-proton modules: 0/30 DXVK passed (I used `--target-env spv1.5` but the modules are SPIR-V 1.6, so this is probably my flag, not bad data), and 40/44 vkd3d-proton passed. The Wicked frames were accepted because they decode to a stream starting with the SPIR-V magic number; they were not run through spirv-val.

---

## Per-candidate details

### 1. Wicked Engine release binary (rating 5)

* URL: https://github.com/turanszkij/WickedEngine, release v0.72.139 (asset `Wicked.Engine.zip`, 341,439,123 bytes). Source tree looked at: `4396192b2717f68971e090deaef0ad2498d69d61` (2026-11-22), which has 315 `.hlsl` + 51 `.h` + 40 `.hlsli` under `WickedEngine/shaders/` and **no** committed `.spv`/`.cso`.
* License: `LICENSE.txt` is MIT (Copyright 2026 Turánszki János). Extracting and redistributing the embedded SPIR-V should be fine under MIT; third-party headers in the tree carry their own licenses (unverified for the shaders' includes).
* Where the SPIR-V is: not as files. The release's `Editor_Linux` (29,591,728 bytes) embeds the shader dump as zstd frames. The magic `28 b5 2f fd` occurs 437 times; 398 frames decode to a SPIR-V module, 39 decode to something else. The Windows `.exe` has 802 zstd magic hits, but I did not process it (its set also includes DX12 bytecode, which is irrelevant here).
* Recipe (verified): download the zip, extract `Editor_Linux`, split at each zstd magic, `zstd -dc` each slice, keep outputs starting with `03 02 23 07`. About 20 lines of Python and `zstd`. Effort: minutes. No toolchain needed.
* Facts: 398 modules (390 distinct), 21,655,344 bytes in total (min 252, median 27,808, max 283,268), all produced by DXC (generator 0xe), SPIR-V 1.6, `-fspv-target-env=vulkan1.3` per `wiShaderCompiler.cpp`, `OpName` present for all, no `OpLine`. Stages: 252 compute, 85 fragment, 44 vertex, 8 mesh, 1 task, 3+3 tess, 2 geometry. xz concat/each = 0.639. OpName-set nearest-neighbor ≥0.9 for 205 modules and ≥0.7 for 290.
* Permutation evidence: strong. This is the engine's permutation machinery (object shaders with many define combinations) and the 205 near-twin modules are consistent with it. The source of the permutations was not traced to specific defines.
* Why it ranks first: it is the only staged set that is both large (20+ MB) and a complete real-engine shader set with compute/graphics/mesh stages. Modules are big (a better stress test for cross-module matching than the small sets).
* Caveats: only the debug-name-bearing form is available. A stripped variant can be produced with `spirv-opt --strip-debug` (not run). Older releases (v0.72.113 and v0.72.120 exist) would give a cross-version family; I did not fetch them. It is unknown whether the dump is complete for the build or only the shaders compiled for Linux (the Windows binary is bigger).

### 2. 0 A.D. 0.28.0 (rating 4)

* URL: https://releases.wildfiregames.com/0ad-0.28.0-unix-data.tar.xz (1,415,012,652 bytes). Verified by streaming `curl | tar -xJ` of only `*/binaries/data/mods/mod/mod.zip` (93,421,350 bytes). Version 0.27.1 also exists (1,367,955,136 bytes) and would add a cross-version family (not fetched).
* The git mirror https://github.com/0ad/0ad at `61a3b9507d974084e6badb88a0826bd89a6d5b8b` is stale (dated 2024-08-17) and commits no `.spv`. The shaders there are 41 GLSL files plus XML program definitions (`source/tools/spirv/compile.py` generates the `.spv` at release time; it needs python3 + `yaml`, `glslc` and spirv-tools 2023+).
* `mod.zip` contains `shaders/spirv/` with 339 `.spv`, 489 `.xml` and 489 `.xmb`. `.spv` names are content hashes, so the 339 are distinct by construction. The 489 XML files describe program/permutation → hash. That gives you the permutation structure for free.
* Facts: 339 modules, 1,720,380 bytes, all generator 0xd (shaderc glslc), SPIR-V 1.3, fully stripped (no OpName/OpLine, since `compile.py` strips debug after reflection). Stages: 183 fragment, 146 vertex, 10 compute. xz concat/each 0.416.
* License: 0 A.D. code is GPL-2+ (art is CC-BY-SA). The shaders are code, so redistribution of the SPIR-V is under GPL-2+ terms (not independently verified in the tarball's license files).
* Rating rationale: a genuine shipped game with real stripped output and documented permutation structure. Smaller than Wicked, but the stripped form is the shipping form. Effort: one 1.4 GB stream.

### 3. SaschaWillems/Vulkan (rating 4)

* URL: https://github.com/SaschaWillems/Vulkan at `d2d49bf7f2d8469ad9fee47b2a43b430d7e10e1b`. The tree lists 1005 `.spv` under `shaders/glsl`, `shaders/hlsl`, `shaders/slang`. Real binaries, not LFS pointers (all parse as SPIR-V).
* The task says "SaschaWillems via Vulkan-Samples ports" is already in use. This is the original repository and it differs: the same ~330 shaders exist in three language variants (glslang 350, DXC 308, Slang 347 modules), which gives a **cross-producer** family set (same source intent, different compilers) that the Vulkan-Samples ports do not.
* Facts: 1005 modules (872 unique by sha1), 2,581,152 bytes; versions 1.0 (597), 1.4 (386), 1.5 (22); stages vert 411, frag 426, comp 30, plus ray tracing (rgen 24, rchit 24, rmiss 29, rahit 4, rcall 9), tess 15+15, geom 10, task 3. All 1005 have OpName (no OpLine). xz concat/each 0.377. OpName nearest neighbor ≥0.9: 500.
* License: MIT.
* Staged: sparse clone of `shaders/` only (12 MB).
* Caveats: modules are small samples, not an application; no stripped variants (could be derived).

### 4. Xenia (rating 3.5)

* URL: https://github.com/xenia-project/xenia at `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`, path `src/xenia/gpu/shaders/bytecode/vulkan_spirv/` (92 headers, committed, "Generated with `xb buildshaders`"). Each header holds a commented disassembly in `#if 0` plus a `uint32_t` array.
* Facts: 92 modules, 1,168,876 bytes; glslang (0x8); SPIR-V 1.0; stages 86 compute, 4 fragment, 2 vertex; debug names present. xz concat/each 0.181; 90/92 have a ≥0.9 OpName sibling. This is the strongest pack structure I measured, though the set is a single emulator's resolve/texture-load family.
* License: BSD-3-clause ("Copyright (c) 2015, Ben Vanik").
* Recipe: parse the uint32 array from each header (script included in my tools, not in repo). Effort: trivial.

### 5. ANGLE internal shaders (rating 3.5)

* URL: https://chromium.googlesource.com/angle/angle at `b7a90a6cc08e52857a6addc2bc2e569b7b01e9df`, path `src/libANGLE/renderer/vulkan/shaders/gen/` (149 `.inc` files). Each file is a `constexpr uint8_t[]` holding a **gzip** stream of the SPIR-V, with the GLSL source in a trailing comment. I decompressed all 149.
* Variants per shader (from file names): ImageClear.frag ×48, ImageCopy.frag ×36, BlitResolve.frag ×24, GenerateMipmap.comp ×10, ConvertVertex.comp ×8, ConvertIndex.comp ×4, plus a handful of ×1–3. This is genuine permutation output (flag bitmasks as file name suffixes).
* Facts: 149 modules, all distinct, 449,396 bytes; glslang; SPIR-V 1.0 (137) and 1.3 (12); stripped (no OpName at all); stages 115 frag, 33 comp, 1 vert. xz concat/each 0.336.
* License: BSD-3 (TransGaming/Google/3Dlabs). The repository LICENSE file text was read.
* Note: "GL CTS zink/angle dirs" in your list are different (test shaders); these are the product's own generated tables.
* Rating limited by the small total size and tiny shaders.

### 6. Quake3e (rating 3)

* URL: https://github.com/ec-/Quake3e at `dec793de9677bc9c16c7027e8905c2d4f4643a19`, `code/renderervk/shaders/spirv/shader_data.c` (1,449,539 bytes, 74 byte arrays).
* Facts: 74 modules (66 unique), 238,516 bytes; glslang; SPIR-V 1.0; 42 vert, 32 frag; debug names; xz 0.301. Names show the permutation axes: `vert_tx{0,1,2}` × `{,_cl,_fixed,_ident1}` × `{,_env}` × `{,_fog}`, `frag_light*`, etc.
* License: Quake 3 GPL-2 (unverified in this repository; the README text I read did not state it explicitly).
* Source templates (`gen_vert.tmpl`, `gen_frag.tmpl`, `light_*.tmpl`) are committed, so the permutation generator is visible.
* Rating: tiny but a clean example of template-driven permutations.

### 7. Lumos (rating 3)

* URL: https://github.com/jmorton06/Lumos at `50ac714464ee0576a6b741c31ac607231edc5a2c`; `Lumos/Assets/Shaders/CompiledSPV/*.spv` (82 files). MIT.
* 82 modules, 1,227,204 bytes; shaderc (0xd), SPIR-V 1.0; 52 frag, 22 vert, 8 comp; debug names; xz 0.611, 18/82 near-twins.
* Of limited use for permutation research but is a plain real-engine set.

### 8. Skia SkSL goldens (rating 2.5)

* URL: https://skia.googlesource.com/skia at `4802f18c7cd248700dd6f813d5299e9b8759f1e9`; `tests/sksl/**/golden/*.asm.{frag,comp,vert}` (380 files: shared 190, intrinsics 102, blend 29, compute 27, spirv 22, errors 6, workarounds 3, realistic 1).
* These are **text disassembly goldens**. 352 assemble with `spirv-as --target-env spv1.3`; 28 fail (some goldens record compile errors). The assembled binaries carry spirv-as's generator ID (0x7), so they are not byte-identical to what SkSL emits.
* 352 modules (342 unique), 912,104 bytes; 319 frag, 23 comp, 10 vert; debug names; xz 0.395.
* License: BSD-3. Test suite, not an application. Included only as a different-producer control.

### 9. Small app-derived sets (rating 2)

* **RetroArch** `gfx/drivers/vulkan_shaders/*.inc` (https://github.com/libretro/RetroArch, `3e771e7d074f9692464063fd91a9da3f5eef5c42`): 36 modules, 197,888 bytes; 29 shaderc (0xd) and 7 glslang; debug names; xz 0.49. GPL-3.
* **Ryujinx / Ryubing fork** (https://git.ryujinx.app/ryubing/ryujinx at `6af327bdd50366163d80bc692f6bf47ec1c1993e`; the original GitHub repository was not reachable in my probe, so use this fork, which has the same files): 33 modules, 193,920 bytes (blit/clear/convert shaders plus FSR/SMAA/FXAA effects); 30 glslang, 3 gen-0xd; xz 0.72 (little redundancy). MIT.
* **Granite** `assets/shaders/post/ffx-cacao` (https://github.com/Themaister/Granite, `69f450c62dc3780680b936539a931b5d03e436fb`): 33 compute modules, 446,412 bytes, generator 0x7, SPIR-V 1.3; xz 0.363 (FidelityFX CACAO variants, a real permutation family). MIT.
* **vkQuake2** (https://github.com/kondrak/vkQuake2, `6763f207229f97cffabb6fc2da72017a794b139b`) `ref_vk/spirv/*.c`: 23 modules, 32,924 bytes. GPL-2.
* **Vita3K** builtin shaders (https://github.com/Vita3K/Vita3K, `64e655dd797153d0c5a5cbf734634be31288168d`): 14 modules, 122,396 bytes. GPL-3 (unverified).
* **DXVK 3.1.1** and **vkd3d-proton 3.1** release DLLs, carved by magic-number scan: 30 and 44 internal-shader modules (365,992 and 227,000 bytes); both glslang, SPIR-V 1.6, debug names. DXVK is zlib-licensed and vkd3d-proton LGPL-2.1 (both from my memory of the projects, unverified here). The carve is heuristic: it trims each module after its last `OpFunctionEnd`.

### 10. llama.cpp ggml-vulkan (recipe only; rating 4 if building is acceptable)

* URL: https://github.com/ggml-org/llama.cpp at `8d60873dfb8619f2b5ed31adfbab07cccb40db5c` (2026-11-22). No `.spv` committed. `ggml/src/ggml-vulkan/vulkan-shaders/` has 202 files (mostly `.comp`) and `vulkan-shaders-gen.cpp` (1,427 lines, 242 `string_to_spv(` call sites, many inside loops over quantization types and options).
* Recipe: install `glslc` (Vulkan SDK or `shaderc`), then build the `vulkan-shaders-gen` target of `cmake -DGGML_VULKAN=ON`, which runs glslc per variant with `-D` defines (`--target-env=vulkan1.2` or `1.3` depending on feature checks). The generator accepts `--glslc` and `--output-dir`; per-variant `.spv` files are written there before being embedded (verified in source at line 441; I did not verify whether they survive after the embed step, so a `--no-clean`-style option may be needed).
* Count is an estimate only (>1000 plausible). Requires running a project build, so it was not done.
* License: MIT for the source; the generated SPIR-V is derivative output of MIT code.
* Value: compute-only, ML kernels; very high variant counts. A different distribution from graphics, but ideal for stress-testing family detection.

### 11. Controls

* **SPIRV-Reflect `tests/`** (https://github.com/KhronosGroup/SPIRV-Reflect, `6002a58dfc83cad60f206c5fc99b5a39d1623dcb`; not in your used list): 128 modules (124 unique), 344,344 bytes; producers DXC 46, glslang 43, gen-0x7 16, Slang 12, gen-0xd 8, unknown 3; includes ray-tracing, mesh-shader and SPIR-V 1.5/1.6 samples. Apache-2.0. Cheap multi-producer control.
* **Dawn/Tint test corpus** (https://dawn.googlesource.com/dawn at `90dcd33551d8b1a646861a5cdef7887434b63867`): 13,131 `.spvasm` (8,740 under `test/tint/builtins/gen`, 1,079 under `test/tint/unittest/reader`, 428 under `test/tint/vk-gl-cts/graphicsfuzz`) plus 4 real `.spv`, three of which are Unity "Boat Attack" WebGPU shaders in `third_party/benchmark_shaders/unity_boat_attack/` (vertex, fragment, compute; real game output but only three modules). Mostly Tint-generated or unit-test fragments; low value.
* **SPIRV-LLVM-Translator** (https://github.com/KhronosGroup/SPIRV-LLVM-Translator, `708f93118262f4df5ccc8ef2a840380b3459bcf3`): 105 `.spv`, OpenCL kernels, a different domain from Vulkan shaders; I did not stage them.

---

## Dead ends (checked, not useful)

All of these were checked by tree listing at the revision shown by `git ls-remote`/clone on 2026-11-22; "none committed" means no `*.spv`, `*.spvasm` or SPIR-V arrays at that revision.

| Candidate | Finding |
|---|---|
| Godot (`67421f3...`) | No SPIR-V committed (155 `.glsl` sources); shader cache is generated at runtime in the user directory. |
| bgfx (`06f558e...`) | No compiled example shaders in the tree (the `examples/runtime/shaders` output is not committed); only SPIR-V headers and tooling. |
| Filament (`995bbd1...`) | No committed `.filamat`; 116 `.mat` sources need `matc`. Its vendored `third_party/dawn` has only the three Boat Attack `.spv`; its SPIRV-Tools fuzz corpora duplicate SPIRV-Tools data you already use. |
| The Forge (`cd50468...`) | 318 `.fsl` sources, built per platform; nothing committed. |
| RBDOOM-3-BFG, vkDOOM3, vkQuake, Quake2RTX | GLSL/HLSL sources only (Q2RTX: roughly 27 compute + 5 rahit + 4 rgen + RT sources; needs `glslangValidator` at build time; GPL-2). The `.inc` files in vkQuake and vkDOOM3 are GLSL includes. Recipe-only for Q2RTX, rated 3 as an RT-stage source. |
| DXVK, vkd3d-proton source repositories | Only shader sources (39 and 73 files); the release-DLL carve above is the only binary route, and gives 30 and 44 modules. |
| NVIDIA NRD, FidelityFX FSR2/SDK, FidelityFX Cauldron | No SPIR-V and no permutation outputs in the tree (FSR2 has only a `ffx_fsr2_shaders_vk.cpp/.h` stub; the SDK has 110 `.hlsl`). Permutations are produced by their build scripts. |
| O3DE | 514 `.azsl` sources; the pipeline needs the AZSL compiler and a full build. |
| Diligent, Kohi, Piccolo, Spartan, axmol, cocos-engine, NazaraEngine, RTXPT, Stride, wmoge, Auto-Vk-Toolkit, Acid, magnum, ImGui, RenderDoc, Bevy | No application SPIR-V committed (wmoge/magnum/RenderDoc hits were glslang/tooling test files). AnKi's `.spv` hits were SPIRV-Tools paths; its only real `.spv` files are two Compressonator binaries. |
| ncnn (`eb8a50d...`) | 331 `.comp` sources, but the library compiles them to SPIR-V at runtime (embedded glslang), so no build-time output exists. A runtime capture would need a Vulkan driver (lavapipe). Not attempted. |
| MNN, ExecuTorch, PyTorch Vulkan | MNN has 2 `.spv`; ExecuTorch has 168 `.glsl` + 168 `.yaml` variant templates and PyTorch has 120 GLSL sources, all generated at build. Recipe-level only. |
| Veloren, Warzone 2100, 0 A.D. GitHub mirror, Cemu, RPCS3, shadPS4, Dolphin, Azahar, PPSSPP | GLSL/HLSL sources or runtime compile; nothing committed. The 0 A.D. mirror is stale (2024-08-17) and its `.spv` are release artifacts (see #2). |
| Mesa fossil-db (`gitlab.freedesktop.org/hakzsam/fossil-db` and `/mesa/fossil-db`) | `git clone` asked for credentials, so I could not access it from here. This is the one source that would be a real-game pipeline-capture collection (Fossilize archives with SPIR-V from many shipping games); it is **not verified** that these URLs are right, and licensing for the game shaders is unclear anyway. Worth asking the user whether they have access. |
| ValveSoftware/Fossilize | 134 files, no captures committed (tooling and test JSON only). |
| gfx-rs/wgpu (Naga snapshot tests) | 142 `.spvasm` outputs + 38 inputs in `naga/tests`; already covered by your "Naga outputs"; test snapshots, not applications. |
| KhronosGroup/SPIRV-Cross | 8 `.spv` in `tests-other`; its shader tests are sources/`.asm` text. |
| dneto0/spirv-samples | 1,144 `.spvasm` that are Tint SPIR-V-reader unit-test fragments (tiny, synthetic). |
| VK-GL-CTS | 73 files (37 `.spvasm`, 36 `.spv`); mostly test fragments, tiny. |
| Vulkan-ValidationLayers, shaderc, DXC, Vulkan-Tools, clspv, MoltenVK, OpenJK, vulkan-guide, niagara, SDL, Vulkan-Hpp, google/amber, google/gapid, graphicsfuzz | 0–4 committed SPIR-V files each (tooling/test inputs). |
| libretro/slang-shaders (`02bf134...`) | **Source only** (1,656 `.slang` files, 2,526 `.slangp` presets; many near-duplicate copies across `crt`, `edge-smoothing`, `bezel` folders, so it is a promising family source). I could not compile it: no `glslangValidator`/`glslc` is installed here and I did not install anything. Recipe: split each `.slang` on `#pragma stage vertex`/`fragment`, preprocess with `-DVERTEX`/`-DFRAGMENT`, compile with `glslangValidator -V -S <stage>` (or glslc) with `-I` pointing at each shader's directory. Estimated 3,000+ modules (unverified). Licenses are per-shader (GPL, MIT, public domain mixes), so likely a build-recipe-only candidate. Rating 3 if the compile pass works. |
| Wicked Engine source, Blender | Wicked is covered above. Blender's Vulkan backend compiles shaders at runtime via shaderc; the blender issue "Precompile SpirV shaders #120554" proposes precompiling, but I could not confirm whether it shipped, so a Blender runtime dump (lavapipe) is a recipe-only idea, unverified. |
| Hugging Face / papers | No SPIR-V binary datasets or shader-cache compression corpora found. The only published comparable corpora I found are the ones you already use (SMOL-V's Unity/Talos/DOTA2 data via smol-v and yari-v). The Shadertoys dataset (GLSL source, Vipitis) is source only. |
| Repositories that advertise game "recompilations" (e.g. Leonida, SpaceAnimalsRecomp) | Not used: they distribute material derived from commercial games and the licensing is unclear. |

## Other unverified leads (not followed)

* 0 A.D. 0.27.0/0.27.1 releases as a cross-version family (same mechanism as 0.28.0).
* Older Wicked releases (v0.72.113, v0.72.120) for version-to-version families.
* Fuchsia's Spinel/HotSort per-vendor precompiled `.spv` (previously in Skia's `src/compute`): not in Skia's current tree (0 hits); Fuchsia's own tree was not probed.
* Qt `.qsb` files (SPIR-V inside QSB containers): not probed.
* Godot "shader baker" export output and Blender runtime SPIR-V dumps: would need a running Vulkan stack (lavapipe) and were not attempted.
* ANGLE's translator output at runtime (not the pre-generated tables) would give many more variants but needs a harness.

## Reproduction notes

* Tree probes: `git clone -q --depth 1 --filter=blob:none --no-checkout <url> d && git -C d ls-tree -r HEAD`. (`ls-tree -l` triggers a lazy fetch of every blob for its size column; avoid it on large repositories.)
* Sparse stages: `git clone --depth 1 --filter=blob:none --sparse <url> d && git -C d sparse-checkout set <dir>`.
* No hooks, scripts or build tools from the downloaded content were run. Format converters (C array, ANGLE gzip, zstd-frame carver, magic-scan carver) were one-off standalone Python scripts that were not kept; `scripts/build_pack_corpus.py` reimplements the ones the pinned sets need.
* Total network use: about 2 GB, dominated by the 1.4 GB 0 A.D. stream (not all kept) and the 341 MB Wicked zip (kept only in the scratchpad, not under `raw/`).
