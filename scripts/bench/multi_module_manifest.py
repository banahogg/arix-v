#!/usr/bin/env python3
"""Gate 1a: frozen multi-module corpus manifest (classification, packs, prefixes).

Builds an exhaustively classified, provenance-verified manifest of the
non-distributed SPIR-V benchmark corpus for multi-module compression research.
The output follows the gate-1 coordination contract (schema_version 1):

  modules       one entry per *logical* corpus entry (canonical files and every
                duplicate alias), keyed by "<source>:<input_path>"
  packs         frozen member lists per relationship tier
  prefix_cases  frozen prefix/target selections for conditional-cost probes

Everything not representable in the contract schema (classification rule,
category, language, debug status, evidence, counts, unavailable comparisons)
goes into a sidecar ``manifest_extras.json`` keyed by the same ids.

Subcommands:
  build    verify inputs and write manifest.json + extras + counts to --out
  publish  copy a built manifest to a coordination dir atomically, then write
           the ready marker LAST (requires a passing baseline verification
           report from multi_module_baselines.py)

No corpus content is written by this script; it only reads the corpus and the
pinned dependency checkouts given on the command line.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import random
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOCK = ROOT / "deps" / "benchmark-sources.lock.json"
SCHEMA_VERSION = 1
CONTRACT_VERSION = 1
GLOBAL_SEED = "arixv-gate1a-v1"
MANIFEST_GENERATOR = "scripts/bench/multi_module_manifest.py"

# Prefix-selection parameters, frozen before any probe was run.
SAME_COLLECTION_BUDGET = 32 * 1024   # bytes of prefix for the stand-alone sc tier
BYTE_MATCH_LO = 0.80                 # matched prefix must be within [lo, hi] x ref
BYTE_MATCH_HI = 1.25
PREFIX_SAMPLES = 3                   # seeded samples per sampled tier
STAGE_REF_MAX_GAP = 8                # lines between adjacent shader references
VARIANT_REF_MAX_SPAN = 40            # lines spanned by corroborating references

# --------------------------------------------------------------------------
# SPIR-V header / light instruction scan
# --------------------------------------------------------------------------

SPIRV_MAGIC = 0x07230203
EXEC_MODELS = {
    0: "vertex", 1: "tess_control", 2: "tess_eval", 3: "geometry",
    4: "fragment", 5: "compute", 6: "kernel",
    5267: "task_nv", 5268: "mesh_nv",
    5313: "raygen", 5314: "intersection", 5315: "anyhit",
    5316: "closesthit", 5317: "miss", 5318: "callable",
    5364: "task", 5365: "mesh",
}
STAGE_EXT = {
    "vert": "vertex", "frag": "fragment", "comp": "compute", "geom": "geometry",
    "tesc": "tess_control", "tese": "tess_eval", "mesh": "mesh", "task": "task",
    "rgen": "raygen", "rmiss": "miss", "rchit": "closesthit", "rahit": "anyhit",
    "rint": "intersection", "rcall": "callable",
}
DEBUG_OPS = {2: "OpSourceContinued", 3: "OpSource", 4: "OpSourceExtension",
             5: "OpName", 6: "OpMemberName", 7: "OpString", 8: "OpLine",
             317: "OpNoLine", 330: "OpModuleProcessed"}


@dataclass
class SpirvInfo:
    valid_header: bool
    byte_swapped: bool
    spirv_version: int
    generator_word: int
    stages: list[str]
    debug_ops: dict[str, int]
    debug_ext_sets: list[str]
    parse_error: str | None

    @property
    def stage(self) -> str | None:
        if not self.stages:
            return None
        uniq = sorted(set(self.stages))
        return uniq[0] if len(uniq) == 1 else "multi:" + "+".join(uniq)

    @property
    def debug_class(self) -> str:
        strong = ("OpName", "OpMemberName", "OpString", "OpLine")
        if self.debug_ext_sets or any(self.debug_ops.get(k) for k in strong):
            return "debug_bearing"
        if self.debug_ops.get("OpSource") or self.debug_ops.get("OpSourceExtension") \
                or self.debug_ops.get("OpModuleProcessed"):
            return "source_only"
        return "stripped"


def scan_spirv(data: bytes) -> SpirvInfo:
    """Header and light instruction scan; never raises on malformed input."""
    info = SpirvInfo(False, False, 0, 0, [], {}, [], None)
    if len(data) < 20 or len(data) % 4:
        info.parse_error = "size<20 or not multiple of 4"
        if len(data) >= 12:
            w = struct.unpack_from("<3I", data)
            info.spirv_version, info.generator_word = w[1], w[2]
        return info
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != SPIRV_MAGIC:
        swapped = struct.unpack(f">{len(data) // 4}I", data)
        if swapped[0] == SPIRV_MAGIC:
            info.byte_swapped = True
            words = swapped
        else:
            info.parse_error = "bad magic"
            info.spirv_version, info.generator_word = words[1], words[2]
            return info
    info.valid_header = True
    info.spirv_version, info.generator_word = words[1], words[2]
    i = 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0 or i + wc > len(words):
            info.parse_error = f"bad word count at word {i}"
            break
        if op in DEBUG_OPS:
            name = DEBUG_OPS[op]
            info.debug_ops[name] = info.debug_ops.get(name, 0) + 1
        elif op == 15 and wc >= 2:  # OpEntryPoint
            info.stages.append(EXEC_MODELS.get(words[i + 1], f"model{words[i + 1]}"))
        elif op == 11 and wc >= 3:  # OpExtInstImport
            raw = struct.pack(f"<{wc - 2}I", *words[i + 2:i + wc])
            name = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
            if "DebugInfo" in name:
                info.debug_ext_sets.append(name)
        i += wc
    return info


# --------------------------------------------------------------------------
# Input verification
# --------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def git_head(path: Path) -> str | None:
    try:
        return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def git_dirty(path: Path) -> bool | None:
    try:
        out = subprocess.check_output(["git", "-C", str(path), "status", "--porcelain",
                                       "--untracked-files=no"], text=True,
                                      stderr=subprocess.DEVNULL)
        return bool(out.strip())
    except (OSError, subprocess.CalledProcessError):
        return None


class VerificationError(SystemExit):
    pass


def verify_inputs(corpus: Path, deps: Path | None, lock_path: Path) -> dict:
    """Check lockfile, sources.tsv, every canonical file and every alias."""
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    if lock.get("schema_version") != 1:
        raise VerificationError(f"unsupported lockfile schema: {lock_path}")
    by_name = {s["name"]: s for s in lock["sources"]}
    sources = read_tsv(corpus / "sources.tsv")
    errors: list[str] = []
    for row in sources:
        locked = by_name.get(row["source"])
        if locked is None:
            errors.append(f"sources.tsv lists unlocked source {row['source']}")
        elif locked["revision"] != row["revision"] or locked["url"] != row["git_url"]:
            errors.append(f"sources.tsv {row['source']} revision/url differs from lockfile")
    if {r["source"] for r in sources} != set(by_name):
        errors.append("sources.tsv and lockfile source sets differ")

    deps_report = {}
    if deps is not None:
        for name, s in by_name.items():
            d = deps / s["directory"]
            head = git_head(d)
            dirty = git_dirty(d)
            deps_report[name] = {"path": str(d), "head": head, "dirty_tracked": dirty,
                                 "locked": s["revision"], "ok": head == s["revision"]}
            if head != s["revision"]:
                errors.append(f"dependency {d} at {head}, lock expects {s['revision']}")
            if dirty:
                errors.append(f"dependency {d} has modified tracked files")

    manifest = read_tsv(corpus / "manifest.tsv")
    duplicates = read_tsv(corpus / "duplicates.tsv")
    spv = corpus / "spv"
    canon: dict[str, dict] = {}
    for row in manifest:
        p = spv / row["canonical_name"]
        if not p.is_file():
            errors.append(f"missing canonical file {p}")
            continue
        size = p.stat().st_size
        digest = sha256_file(p)
        if digest != row["sha256"] or size != int(row["bytes"]) or \
                row["canonical_name"] != f"{digest}.spv":
            errors.append(f"canonical file mismatch {p}")
        canon[row["canonical_name"]] = row
    for row in duplicates:
        c = canon.get(row["canonical_name"])
        if c is None or c["sha256"] != row["sha256"] or c["bytes"] != row["bytes"]:
            errors.append(f"duplicate alias {row['source']}:{row['input_path']} "
                          "does not match a canonical file")
    on_disk = {p.name for p in spv.glob("*.spv")}
    if on_disk != set(canon):
        errors.append(f"spv/ has {len(on_disk)} files, manifest.tsv {len(canon)}")
    counts = Counter(r["source"] for r in manifest)
    cand = Counter(r["source"] for r in manifest + duplicates)
    for row in sources:
        if int(row["unique_selected"]) != counts.get(row["source"], 0) or \
                int(row["candidates"]) != cand.get(row["source"], 0):
            errors.append(f"sources.tsv counts for {row['source']} disagree with TSVs")

    # Every logical entry must also match the pinned upstream file byte-for-byte.
    upstream_checked = 0
    if deps is not None:
        for row in manifest + duplicates:
            s = by_name[row["source"]]
            up = deps / s["directory"] / row["input_path"]
            if not up.is_file():
                errors.append(f"upstream file missing: {up}")
                continue
            if sha256_file(up) != row["sha256"]:
                errors.append(f"upstream file differs from corpus: {up}")
            upstream_checked += 1
    if errors:
        raise VerificationError("input verification failed:\n  " + "\n  ".join(errors[:50]))
    return {
        "lockfile": str(lock_path),
        "lockfile_sha256": sha256_file(lock_path),
        "corpus": str(corpus),
        "manifest_tsv_sha256": sha256_file(corpus / "manifest.tsv"),
        "duplicates_tsv_sha256": sha256_file(corpus / "duplicates.tsv"),
        "sources_tsv_sha256": sha256_file(corpus / "sources.tsv"),
        "canonical_files_verified": len(canon),
        "aliases_verified": len(duplicates),
        "upstream_files_verified": upstream_checked,
        "deps": deps_report,
        "lock": by_name,
        "manifest_rows": manifest,
        "duplicate_rows": duplicates,
        "sources_rows": sources,
    }


# --------------------------------------------------------------------------
# Classification rule table (exhaustive; first match wins)
# --------------------------------------------------------------------------

CATEGORY_REPRESENTATIVE = {"application_derived", "sample_collection"}
PROXY_CATEGORIES = {"compiler_test", "conformance_test", "shader_examples",
                    "transform_derived"}

LANG_DIRS = {"glsl", "hlsl", "slang", "spirv"}


@dataclass
class Classification:
    rule: str
    category: str
    collection_id: str
    origin_id: str | None
    base_fold: str
    language: str | None = None
    sample: str | None = None
    notes: str = ""
    in_packs: bool = True


def vks_layout(input_path: str) -> tuple[str | None, str | None, str]:
    """Return (sample, language-dir, basename) for a Vulkan-Samples shader path."""
    parts = input_path.split("/")
    assert parts[0] == "shaders"
    base = parts[-1]
    middle = parts[1:-1]
    if not middle:
        return None, None, base
    lang = None
    if middle[-1] in LANG_DIRS:
        lang = middle[-1]
        middle = middle[:-1]
    return "/".join(middle), lang, base


def vks_source_language(deps: Path | None, lock: dict, input_path: str) -> str | None:
    if deps is None:
        return None
    root = deps / lock["vulkan-samples"]["directory"]
    spv = root / input_path
    stem = spv.name[:-len(".spv")]
    if (spv.parent / f"{stem}.hlsl").exists():
        return "hlsl"
    if (spv.parent / f"{stem}.slang").exists():
        return "slang"
    if stem.endswith(".spvasm") or (spv.parent / stem).suffix == ".spvasm":
        return "spirv_asm"
    if (spv.parent / stem).exists():
        return "glsl"
    return None


RULES = [
    # (rule id, source, regex on input_path, description)
    ("smolv-dota2", "smol-v", r"^tests/spirv-dumps/dota2/",
     "Dota 2 application-derived dump collection; numeric names do not establish variant families"),
    ("smolv-blender", "smol-v", r"^tests/spirv-dumps/blender/",
     "Blender application-derived dumps; not assumed to be one load set"),
    ("smolv-shadertoy", "smol-v", r"^tests/spirv-dumps/shadertoy/",
     "Shadertoy examples by individual authors (via glslang); unrelated content assumed"),
    ("smolv-glslang", "smol-v", r"^tests/spirv-dumps/glslang/",
     "glslang test-suite outputs; compiler test collection"),
    ("smolv-dxc", "smol-v", r"^tests/spirv-dumps/dxc/",
     "single DXC-compiled module; compiler test collection"),
    ("smolv-talos", "smol-v", r"^tests/spirv-dumps/talos/",
     "Talos Principle dumps (not *.spv upstream; normally absent)"),
    ("smolv-unity", "smol-v", r"^tests/spirv-dumps/unity/",
     "Unity dumps (not *.spv upstream; normally absent)"),
    ("spvdb-gl_cts", "spirv-database", r"^vulkan/gl_cts/(zink|angle)/",
     "GL CTS shaders captured through Zink/ANGLE; conformance proxy collections"),
    ("spvdb-naga", "spirv-database", r"^vulkan/naga/",
     "Naga re-emission of other SPIRV-Database inputs; transformation-derived"),
    ("spvdb-dawn", "spirv-database", r"^vulkan/dawn/",
     "Dawn end2end test shader dumps; conformance/test collection"),
    ("spvdb-tint", "spirv-database", r"^vulkan/tint/",
     "Tint test-suite dumps; compiler test collection"),
    ("spvdb-clspv", "spirv-database", r"^vulkan/clspv/",
     "clspv OpenCL C test outputs (Kernel); labeled separately from graphics"),
    ("spvdb-glslang", "spirv-database", r"^vulkan/glslang/",
     "glslang gtests outputs; compiler test collection"),
    ("spvdb-sascha", "spirv-database", r"^vulkan/SaschaWillemsVulkan/(glsl|hlsl)/",
     "SaschaWillems/Vulkan sample shaders by sample and frontend"),
    ("spvdb-bigwheels", "spirv-database", r"^vulkan/bigwheels/",
     "google/bigwheels sample shaders"),
    ("spvdb-nzsl", "spirv-database", r"^vulkan/nzsl/",
     "Nazara shading language shaders"),
    ("vks-root", "vulkan-samples", r"^shaders/[^/]+$",
     "Vulkan-Samples root-level framework/support shaders; separate control collection"),
    ("vks-sample", "vulkan-samples", r"^shaders/.+/",
     "Vulkan-Samples per-sample shaders, split by sample and source language"),
    ("spirv-tools-fuzz", "spirv-tools", r"^test/fuzzers/",
     "SPIRV-Tools fuzzer seed corpus; robustness only, not an application set"),
    ("yariv-mirror", "yari-v", r"^test/external/(dota2|shadertoy|talos|unity)/",
     "yari-v mirror of smol-v dumps; attached to the evidenced origin collection, "
     "not an independent observation"),
]


def classify(source: str, input_path: str, deps: Path | None, lock: dict) -> Classification:
    for rule_id, rsrc, rx, _ in RULES:
        if rsrc != source or not re.search(rx, input_path):
            continue
        parts = input_path.split("/")
        if rule_id == "smolv-dota2":
            return Classification(rule_id, "application_derived", "smolv/dota2",
                                  "valve-dota2", "origin:valve-dota2")
        if rule_id == "smolv-blender":
            return Classification(rule_id, "application_derived", "smolv/blender",
                                  "blender", "origin:blender")
        if rule_id == "smolv-talos":
            return Classification(rule_id, "application_derived", "smolv/talos",
                                  "croteam-talos", "origin:croteam-talos")
        if rule_id == "smolv-unity":
            return Classification(rule_id, "application_derived", "smolv/unity",
                                  "unity", "origin:unity")
        if rule_id == "smolv-shadertoy":
            return Classification(rule_id, "shader_examples", "smolv/shadertoy", None,
                                  "collection:smolv/shadertoy",
                                  notes="individual authors; origin unknown")
        if rule_id == "smolv-glslang":
            return Classification(rule_id, "compiler_test", "smolv/glslang",
                                  "glslang-tests", "origin:glslang-tests")
        if rule_id == "smolv-dxc":
            return Classification(rule_id, "compiler_test", "smolv/dxc", None,
                                  "collection:smolv/dxc", notes="origin unknown")
        if rule_id == "spvdb-gl_cts":
            coll = "spvdb/" + "/".join(parts[1:-1])
            front = parts[2]
            return Classification(rule_id, "conformance_test", coll, "khronos-gl-cts",
                                  "origin:khronos-gl-cts",
                                  notes=f"captured through {front}")
        if rule_id == "spvdb-naga":
            return Classification(rule_id, "transform_derived", "spvdb/naga/remaps", None,
                                  "collection:spvdb/naga/remaps",
                                  notes="inputs are unidentified SPIRV-Database modules; "
                                        "holdout leakage against other SPIRV-Database "
                                        "collections cannot be excluded")
        if rule_id == "spvdb-dawn":
            return Classification(rule_id, "conformance_test", "spvdb/dawn",
                                  "dawn-end2end-tests", "origin:dawn-end2end-tests")
        if rule_id == "spvdb-tint":
            return Classification(rule_id, "compiler_test", "spvdb/tint", "tint-tests",
                                  "origin:tint-tests")
        if rule_id == "spvdb-clspv":
            return Classification(rule_id, "compiler_test", "spvdb/clspv", "clspv-tests",
                                  "origin:clspv-tests", notes="OpenCL kernels")
        if rule_id == "spvdb-glslang":
            return Classification(rule_id, "compiler_test", "spvdb/glslang",
                                  "glslang-tests", "origin:glslang-tests")
        if rule_id == "spvdb-sascha":
            lang, sample = parts[2], parts[3] if len(parts) > 4 else None
            return Classification(rule_id, "sample_collection",
                                  f"spvdb/sascha/{sample}/{lang}", "saschawillems-vulkan",
                                  "origin:saschawillems-vulkan", language=lang, sample=sample)
        if rule_id == "spvdb-bigwheels":
            return Classification(rule_id, "sample_collection", "spvdb/bigwheels",
                                  "google-bigwheels", "origin:google-bigwheels")
        if rule_id == "spvdb-nzsl":
            return Classification(rule_id, "sample_collection", "spvdb/nzsl",
                                  "nazara-engine", "origin:nazara-engine")
        if rule_id == "vks-root":
            return Classification(rule_id, "control", "vks/_root", "khronos-vulkan-samples",
                                  "origin:khronos-vulkan-samples",
                                  language=vks_source_language(deps, lock, input_path),
                                  notes="framework/support shaders")
        if rule_id == "vks-sample":
            sample, lang_dir, _ = vks_layout(input_path)
            lang = lang_dir or vks_source_language(deps, lock, input_path) or "unknown"
            return Classification(rule_id, "sample_collection", f"vks/{sample}/{lang}",
                                  "khronos-vulkan-samples", "origin:khronos-vulkan-samples",
                                  language=lang, sample=sample)
        if rule_id == "spirv-tools-fuzz":
            return Classification(rule_id, "fuzz_test", "spirv-tools/fuzzers", None,
                                  "collection:spirv-tools/fuzzers")
        if rule_id == "yariv-mirror":
            sub = parts[2]
            origin = {"dota2": "valve-dota2", "shadertoy": None, "talos": "croteam-talos",
                      "unity": "unity"}[sub]
            fold = f"origin:{origin}" if origin else f"collection:smolv/{sub}"
            return Classification(rule_id, "mirror_alias", f"mirror/yari-v/{sub}", origin,
                                  fold, in_packs=False,
                                  notes=f"mirror of smol-v tests/spirv-dumps/{sub}")
    return Classification("unmatched", "unclassified", f"unclassified/{source}", None,
                          f"collection:unclassified/{source}", in_packs=True)


# Folds that are merged without content connectivity because provenance shows
# a plausible common origin (ported samples / shared authorship).
EXTRA_FOLD_MERGES = [
    ("origin:khronos-vulkan-samples", "origin:saschawillems-vulkan",
     "Many Khronos Vulkan-Samples API samples were contributed/ported by Sascha "
     "Willems; keep both in one holdout fold to avoid related-sample leakage"),
]


# --------------------------------------------------------------------------
# Vulkan-Samples source-reference evidence (stage pairs, variant families)
# --------------------------------------------------------------------------

SPV_LITERAL = re.compile(r'"([^"\n]*?\.spv)"')


def index_vks_references(vks_root: Path) -> dict[str, list[tuple[str, int, str]]]:
    """basename -> [(source path relative to repo, line, literal)]."""
    refs: dict[str, list[tuple[str, int, str]]] = defaultdict(list)
    for path in sorted((vks_root / "samples").rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp") or not path.is_file():
            continue
        rel = path.relative_to(vks_root).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for lit in SPV_LITERAL.findall(line):
                refs[lit.rsplit("/", 1)[-1]].append((rel, lineno, lit))
    return refs


def ref_matches_sample(ref: tuple[str, int, str], sample_top: str) -> bool:
    rel, _, lit = ref
    return lit.startswith(sample_top + "/") or f"/{sample_top}/" in f"/{rel}"


def stage_from_basename(base: str) -> str | None:
    parts = base.split(".")
    if len(parts) >= 3 and parts[-1] == "spv":
        return STAGE_EXT.get(parts[-2])
    return None


@dataclass
class EvidencePack:
    tier: str
    collection_id: str
    module_ids: list[str]
    evidence: str


def find_vks_relationships(mods: dict[str, dict], extras: dict[str, dict],
                           refs: dict[str, list[tuple[str, int, str]]], vks_rev: str
                           ) -> tuple[list[EvidencePack], list[dict]]:
    """Stage-pair bundles and stem-variant families corroborated by sample sources."""
    by_coll: dict[str, list[str]] = defaultdict(list)
    for mid, ex in extras.items():
        if ex["rule"] == "vks-sample":
            by_coll[mods[mid]["collection_id"]].append(mid)
    packs: list[EvidencePack] = []
    rejected: list[dict] = []
    for coll, mids in sorted(by_coll.items()):
        sample_top = extras[mids[0]]["sample"].split("/")[0]
        base_to_mid: dict[str, str] = {}
        for mid in sorted(mids):
            base = mods[mid]["logical_key"].rsplit("/", 1)[-1]
            base_to_mid.setdefault(base, mid)
        # Gather references per source file for basenames in this collection.
        per_file: dict[str, list[tuple[int, str]]] = defaultdict(list)
        for base in base_to_mid:
            for ref in refs.get(base, []):
                if ref_matches_sample(ref, sample_top):
                    per_file[ref[0]].append((ref[1], base))
        seen_sets: set[tuple[str, ...]] = set()
        # Stage-pair bundles: adjacent references with distinct stages.
        for src, items in sorted(per_file.items()):
            items = sorted(set(items))
            cluster: list[tuple[int, str]] = []

            def flush(cl: list[tuple[int, str]]) -> None:
                stages = {stage_from_basename(b) for _, b in cl}
                stages.discard(None)
                bases = tuple(sorted({b for _, b in cl}))
                if len(bases) >= 2 and len(stages) == len(bases) and bases not in seen_sets:
                    seen_sets.add(bases)
                    lines = ",".join(str(l) for l, _ in cl)
                    packs.append(EvidencePack(
                        "stage_pair", coll, [base_to_mid[b] for b in bases],
                        f"Vulkan-Samples@{vks_rev[:12]} {src}:{lines} references "
                        f"{' + '.join(bases)} within {STAGE_REF_MAX_GAP} lines "
                        "(source-adjacent pipeline stage references)"))

            for line, base in items:
                st = stage_from_basename(base)
                if cluster and (line - cluster[-1][0] > STAGE_REF_MAX_GAP or
                                st in {stage_from_basename(b) for _, b in cluster}):
                    flush(cluster)
                    cluster = []
                cluster.append((line, base))
            if cluster:
                flush(cluster)
        # Stem variants: root stem plus root_* stems with the same stage suffix.
        stems = defaultdict(dict)  # stage ext -> stem -> base
        for base in base_to_mid:
            p = base.split(".")
            if len(p) >= 3:
                stems[p[-2]][".".join(p[:-2])] = base
        for ext, sm in stems.items():
            for root_stem in sorted(sm):
                fam = sorted(s for s in sm if s == root_stem or s.startswith(root_stem + "_"))
                if len(fam) < 2:
                    continue
                if any(other != root_stem and root_stem.startswith(other + "_") for other in sm):
                    continue  # only take maximal families rooted at the shortest stem
                bases = [sm[s] for s in fam]
                corroborated = None
                for src, items in per_file.items():
                    lines = {b: l for l, b in items if b in bases}
                    if len(lines) == len(bases) and \
                            max(lines.values()) - min(lines.values()) <= VARIANT_REF_MAX_SPAN:
                        corroborated = (src, sorted(lines.values()))
                        break
                if corroborated:
                    packs.append(EvidencePack(
                        "variant_family", coll, [base_to_mid[b] for b in bases],
                        f"Vulkan-Samples@{vks_rev[:12]} {corroborated[0]}:"
                        f"{','.join(map(str, corroborated[1]))} selects among stem variants "
                        f"{' / '.join(fam)} (.{ext}) within {VARIANT_REF_MAX_SPAN} lines"))
                else:
                    rejected.append({"collection_id": coll, "stems": fam, "stage_ext": ext,
                                     "reason": "no sample source references all stems "
                                               f"within {VARIANT_REF_MAX_SPAN} lines"})
    return packs, rejected


# --------------------------------------------------------------------------
# Folds (connected duplicates never cross folds)
# --------------------------------------------------------------------------

class UnionFind:
    def __init__(self) -> None:
        self.parent: dict[str, str] = {}

    def find(self, x: str) -> str:
        self.parent.setdefault(x, x)
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x

    def union(self, a: str, b: str) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            if rb < ra:
                ra, rb = rb, ra
            self.parent[rb] = ra


# --------------------------------------------------------------------------
# Prefix-case selection
# --------------------------------------------------------------------------

def case_seed(case_id: str) -> int:
    return int(hashlib.sha256(f"{GLOBAL_SEED}|{case_id}".encode()).hexdigest()[:8], 16)


def byte_matched_sample(pool: list[str], sizes: dict[str, int], ref_bytes: int,
                        seed: int) -> list[str] | None:
    """Seeded greedy selection whose total bytes fall in [lo, hi] x ref_bytes."""
    lo, hi = BYTE_MATCH_LO * ref_bytes, BYTE_MATCH_HI * ref_bytes
    order = sorted(pool)
    random.Random(seed).shuffle(order)
    chosen, total = [], 0
    for mid in order:
        if total + sizes[mid] <= hi:
            chosen.append(mid)
            total += sizes[mid]
            if total >= lo:
                return chosen
    return None


def build_prefix_cases(mods: dict[str, dict], extras: dict[str, dict],
                       packs: list[dict]) -> tuple[list[dict], dict]:
    sizes = {m: mods[m]["byte_size"] for m in mods}
    sha = {m: mods[m]["sha256"] for m in mods}
    # Targets: unique-content canonical-within-collection members of classified,
    # non-robustness collections.
    eligible_cats = CATEGORY_REPRESENTATIVE | PROXY_CATEGORIES
    coll_members: dict[str, list[str]] = defaultdict(list)
    for p in packs:
        if p["tier"] == "same_collection" and p["id"].endswith("#unique"):
            coll_members[p["collection_id"]] = list(p["module_ids"])
    for p in packs:
        if p["tier"] == "same_collection" and not p["id"].endswith("#unique") \
                and p["collection_id"] not in coll_members:
            # No duplicates: logical == unique.
            coll_members[p["collection_id"]] = list(p["module_ids"])
    strong_partners: dict[str, dict[str, list[tuple[str, list[str]]]]] = defaultdict(
        lambda: defaultdict(list))
    for p in packs:
        if p["tier"] in ("stage_pair", "variant_family"):
            for t in p["module_ids"]:
                others = [m for m in p["module_ids"] if m != t and sha[m] != sha[t]]
                if others:
                    strong_partners[t][p["tier"]].append((p["id"], others))
    unrelated_pool_all = []
    pool_seen: set[str] = set()
    for m in sorted(mods):
        if extras[m]["category"] in eligible_cats and mods[m]["origin_id"] is not None \
                and extras[m]["in_packs"] and extras[m]["canonical_in_collection"] \
                and sha[m] not in pool_seen:
            pool_seen.add(sha[m])
            unrelated_pool_all.append(m)

    cases: list[dict] = []
    stats = Counter()

    def add(case_id: str, target: str, prefix: list[str] | None, tier: str,
            reason: str | None) -> None:
        avail = prefix is not None
        cases.append({"id": case_id, "target_id": target,
                      "prefix_ids": list(prefix) if prefix else [],
                      "tier": tier, "availability": "available" if avail else "unavailable",
                      "reason": reason, "seed": case_seed(case_id)})
        stats[(tier, "available" if avail else "unavailable")] += 1

    def controls(anchor_id: str, target: str, ref_bytes: int, exclude: set[str]) -> None:
        tm = mods[target]
        pool_u = [m for m in unrelated_pool_all
                  if mods[m]["fold_id"] != tm["fold_id"]
                  and mods[m]["origin_id"] != tm["origin_id"]
                  and sha[m] != sha[target] and m not in exclude]
        pool_t = [m for m in pool_u
                  if mods[m]["generator_word"] == tm["generator_word"]
                  and mods[m]["spirv_version"] == tm["spirv_version"]]
        for tier, pool, label in (("unrelated", pool_u, "u"),
                                  ("unrelated_toolchain_matched", pool_t, "t")):
            for k in range(PREFIX_SAMPLES):
                cid = f"{anchor_id}|{label}{k}"
                if not pool:
                    add(cid, target, None, tier,
                        "no candidate in another fold/origin"
                        + (" with matching generator word and SPIR-V version"
                           if tier.endswith("matched") else ""))
                    continue
                pick = byte_matched_sample(pool, sizes, ref_bytes, case_seed(cid))
                add(cid, target, pick, tier, None if pick else
                    f"cannot match {ref_bytes} prefix bytes within "
                    f"[{BYTE_MATCH_LO}, {BYTE_MATCH_HI}]x from {len(pool)} candidates")

    for coll in sorted(coll_members):
        members = coll_members[coll]
        if not members or extras[members[0]]["category"] not in eligible_cats:
            continue
        for target in members:
            base = f"pc|{target}"
            add(f"{base}|none", target, [], "none", None)
            # Strong tiers first (stage pair, variant family).
            strong_ids: set[str] = set()
            for tier in ("stage_pair", "variant_family"):
                lst = strong_partners[target].get(tier, [])
                for _, others in lst:
                    strong_ids.update(others)
            for tier, label in (("stage_pair", "sp"), ("variant_family", "vf")):
                lst = strong_partners[target].get(tier, [])
                if not lst:
                    add(f"{base}|{label}", target, None, tier,
                        f"no provenance-supported {tier.replace('_', ' ')} for target")
                    continue
                for j, (pack_id, others) in enumerate(lst):
                    anchor = f"{base}|{label}{j}"
                    add(anchor, target, others, tier, None)
                    ref = sum(sizes[m] for m in others)
                    # Same-collection prefix byte-matched to the strong prefix,
                    # excluding stronger-tier partners and target content.
                    pool_sc = [m for m in members if m != target and sha[m] != sha[target]
                               and m not in strong_ids]
                    for k in range(PREFIX_SAMPLES):
                        cid = f"{anchor}|c{k}"
                        pick = byte_matched_sample(pool_sc, sizes, ref, case_seed(cid)) \
                            if pool_sc else None
                        add(cid, target, pick, "same_collection", None if pick else
                            f"no same-collection prefix matching {ref} bytes "
                            f"({len(pool_sc)} candidates)")
                    controls(anchor, target, ref, set())
            # Stand-alone same-collection tier at a fixed byte budget.
            pool_sc = [m for m in members if m != target and sha[m] != sha[target]
                       and m not in strong_ids]
            budget_total = sum(sizes[m] for m in pool_sc)
            for k in range(PREFIX_SAMPLES):
                anchor = f"{base}|sc{k}"
                if not pool_sc:
                    add(anchor, target, None, "same_collection",
                        "no other distinct-content member in collection")
                    continue
                order = sorted(pool_sc)
                random.Random(case_seed(anchor)).shuffle(order)
                pick, tot = [], 0
                for m in order:
                    pick.append(m)
                    tot += sizes[m]
                    if tot >= SAME_COLLECTION_BUDGET:
                        break
                if k > 0 and budget_total <= SAME_COLLECTION_BUDGET and len(pool_sc) == 1:
                    add(anchor, target, None, "same_collection",
                        "only one possible same-collection prefix (sample 0)")
                    continue
                add(anchor, target, pick, "same_collection", None)
                controls(anchor, target, tot, set())
    return cases, {f"{t}/{a}": n for (t, a), n in sorted(stats.items())}


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------

def build(args: argparse.Namespace) -> int:
    corpus = args.corpus.resolve()
    deps = args.deps.resolve() if args.deps else None
    ver = verify_inputs(corpus, deps, args.lock.resolve())
    lock = ver["lock"]
    spv = corpus / "spv"

    mods: dict[str, dict] = {}
    extras: dict[str, dict] = {}
    alias_of: dict[str, str] = {}
    canon_by_name = {r["canonical_name"]: r for r in ver["manifest_rows"]}
    rows = [(r, False) for r in ver["manifest_rows"]] + \
           [(r, True) for r in ver["duplicate_rows"]]
    info_cache: dict[str, SpirvInfo] = {}
    for row, is_alias in rows:
        mid = f"{row['source']}:{row['input_path']}"
        if mid in mods:
            raise SystemExit(f"duplicate logical id {mid}")
        path = spv / row["canonical_name"]
        if row["sha256"] not in info_cache:
            info_cache[row["sha256"]] = scan_spirv(path.read_bytes())
        info = info_cache[row["sha256"]]
        cls = classify(row["source"], row["input_path"], deps, lock)
        mods[mid] = {
            "path": str(path), "sha256": row["sha256"], "byte_size": int(row["bytes"]),
            "logical_key": mid, "origin_id": cls.origin_id,
            "collection_id": cls.collection_id, "fold_id": cls.base_fold,
            "stage": info.stage, "spirv_version": info.spirv_version,
            "generator_word": info.generator_word,
        }
        if is_alias:
            c = canon_by_name[row["canonical_name"]]
            alias_of[mid] = f"{c['source']}:{c['input_path']}"
        extras[mid] = {
            "rule": cls.rule, "category": cls.category, "language": cls.language,
            "sample": cls.sample, "notes": cls.notes, "in_packs": cls.in_packs,
            "corpus_alias_of": alias_of.get(mid),
            "source_revision": lock[row["source"]]["revision"],
            "upstream_path": f"{lock[row['source']]['directory']}/{row['input_path']}",
            "valid_header": info.valid_header, "byte_swapped": info.byte_swapped,
            "parse_error": info.parse_error, "stages": sorted(set(info.stages)),
            "entry_points": len(info.stages), "debug_class": info.debug_class,
            "debug_ops": info.debug_ops, "debug_ext_sets": info.debug_ext_sets,
            "generator_tool": info.generator_word >> 16,
            "generator_tool_version": info.generator_word & 0xFFFF,
        }

    # Folds: base fold per origin/collection, merged by content connectivity.
    uf = UnionFind()
    merges: list[dict] = []
    by_sha: dict[str, list[str]] = defaultdict(list)
    for mid, m in mods.items():
        uf.find(m["fold_id"])
        by_sha[m["sha256"]].append(mid)
    for digest, mids in by_sha.items():
        folds = sorted({mods[m]["fold_id"] for m in mids})
        for f in folds[1:]:
            if uf.find(folds[0]) != uf.find(f):
                merges.append({"folds": [folds[0], f], "reason": f"shared content {digest[:16]}"})
            uf.union(folds[0], f)
    for a, b, why in EXTRA_FOLD_MERGES:
        if a in uf.parent and b in uf.parent:
            merges.append({"folds": [a, b], "reason": why})
            uf.union(a, b)
    fold_members: dict[str, set[str]] = defaultdict(set)
    for m in mods.values():
        fold_members[uf.find(m["fold_id"])].add(m["fold_id"])
    fold_name = {root: "fold:" + "+".join(sorted(s)) for root, s in fold_members.items()}
    for mid, m in mods.items():
        extras[mid]["base_fold"] = m["fold_id"]
        m["fold_id"] = fold_name[uf.find(m["fold_id"])]

    # Canonical-in-collection flag (first logical entry per content per collection).
    by_coll: dict[str, list[str]] = defaultdict(list)
    for mid in sorted(mods):
        by_coll[mods[mid]["collection_id"]].append(mid)
    for coll, mids in by_coll.items():
        seen: set[str] = set()
        for mid in mids:
            extras[mid]["canonical_in_collection"] = mods[mid]["sha256"] not in seen
            seen.add(mods[mid]["sha256"])

    # Packs.
    packs: list[dict] = []
    coll_summary = {}
    for coll, mids in sorted(by_coll.items()):
        ex0 = extras[mids[0]]
        cat = ex0["category"]
        m0 = mods[mids[0]]
        origins = {mods[m]["origin_id"] for m in mids}
        folds = {mods[m]["fold_id"] for m in mids}
        assert len(origins) == 1 and len(folds) == 1, coll
        uniq = [m for m in mids if extras[m]["canonical_in_collection"]]
        coll_summary[coll] = {"category": cat, "rule": ex0["rule"], "members": len(mids),
                              "unique": len(uniq)}
        if not ex0["in_packs"]:
            continue
        tier = "unclassified" if cat == "unclassified" else (
            "control" if cat == "control" else "same_collection")
        rep = cat in CATEGORY_REPRESENTATIVE and len(uniq) >= 2
        base_ev = f"rule {ex0['rule']}: " + next(d for r, _, _, d in RULES if r == ex0["rule"]) \
            if ex0["rule"] != "unmatched" else "no rule matched; unclassified"
        packs.append({"id": f"coll:{coll}", "module_ids": mids, "tier": tier,
                      "origin_id": m0["origin_id"], "collection_id": coll,
                      "fold_id": m0["fold_id"], "representative": rep,
                      "evidence": base_ev + "; logical entries incl. in-collection duplicate "
                                            "aliases, lexicographic logical-key order (not a "
                                            "load order)"})
        if len(uniq) != len(mids):
            packs.append({"id": f"coll:{coll}#unique", "module_ids": uniq, "tier": tier,
                          "origin_id": m0["origin_id"], "collection_id": coll,
                          "fold_id": m0["fold_id"], "representative": False,
                          "evidence": base_ev + "; unique-content companion of the logical "
                                                "pack (first logical entry per content)"})

    # Vulkan-Samples relationship packs.
    vks_root = deps / lock["vulkan-samples"]["directory"] if deps else None
    rejected_variants: list[dict] = []
    if vks_root is not None:
        refs = index_vks_references(vks_root)
        rel_packs, rejected_variants = find_vks_relationships(
            mods, extras, refs, lock["vulkan-samples"]["revision"])
        counters = Counter()
        for rp in rel_packs:
            label = "pair" if rp.tier == "stage_pair" else "variant"
            n = counters[(label, rp.collection_id)]
            counters[(label, rp.collection_id)] += 1
            m0 = mods[rp.module_ids[0]]
            packs.append({"id": f"{label}:{rp.collection_id}:{n}",
                          "module_ids": sorted(rp.module_ids), "tier": rp.tier,
                          "origin_id": m0["origin_id"], "collection_id": rp.collection_id,
                          "fold_id": m0["fold_id"], "representative": True,
                          "evidence": rp.evidence})

    # Cross-frontend controls: the same Vulkan-Samples sample across languages.
    by_sample: dict[str, list[str]] = defaultdict(list)
    for mid, ex in extras.items():
        if ex["rule"] == "vks-sample":
            by_sample[ex["sample"]].append(mid)
    for sample, mids in sorted(by_sample.items()):
        langs = {extras[m]["language"] for m in mids}
        if len(langs) >= 2:
            m0 = mods[mids[0]]
            packs.append({"id": f"control:cross-frontend:vks/{sample}",
                          "module_ids": sorted(mids), "tier": "control",
                          "origin_id": m0["origin_id"], "collection_id": f"vks/{sample}/*",
                          "fold_id": m0["fold_id"], "representative": False,
                          "evidence": f"control: union of {sorted(langs)} frontends of "
                                      f"sample {sample}; not an assumed shipping set"})

    # Unrelated cross-origin mixes and singleton robustness controls.
    rep_canon = sorted(m for m in mods if extras[m]["category"] in
                       (CATEGORY_REPRESENTATIVE | PROXY_CATEGORIES)
                       and mods[m]["origin_id"] is not None
                       and extras[m]["canonical_in_collection"])
    for n in (8, 32):
        for k in range(2):
            pid = f"control:unrelated-mix:n{n}:s{k}"
            rng = random.Random(case_seed(pid))
            order = rep_canon[:]
            rng.shuffle(order)
            chosen, used_colls, seen_sha = [], set(), set()
            for m in order:
                c = mods[m]["collection_id"]
                if c in used_colls or mods[m]["sha256"] in seen_sha:
                    continue
                chosen.append(m)
                used_colls.add(c)
                seen_sha.add(mods[m]["sha256"])
                if len(chosen) == n:
                    break
            packs.append({"id": pid, "module_ids": chosen, "tier": "control",
                          "origin_id": None, "collection_id": "control/unrelated-mix",
                          "fold_id": "mixed", "representative": False,
                          "evidence": f"control: seeded ({GLOBAL_SEED}|{pid}) shuffle of "
                                      "known-origin canonical modules, at most one per "
                                      "collection; unrelated by construction"})
    canon_all = sorted((mods[m]["byte_size"], m) for m in mods
                       if extras[m]["corpus_alias_of"] is None)
    for label, (_, m) in (("smallest", canon_all[0]), ("largest", canon_all[-1])):
        packs.append({"id": f"control:singleton:{label}", "module_ids": [m],
                      "tier": "control", "origin_id": mods[m]["origin_id"],
                      "collection_id": mods[m]["collection_id"],
                      "fold_id": mods[m]["fold_id"], "representative": False,
                      "evidence": f"control: singleton ({label} corpus module); "
                                  "measures pure framing overhead"})

    cases, case_stats = build_prefix_cases(mods, extras, packs)

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "snapshot": {
            "source_lock_sha256": ver["lockfile_sha256"],
            "manifest_sha256": ver["manifest_tsv_sha256"],
            "duplicates_sha256": ver["duplicates_tsv_sha256"],
            "sources_sha256": ver["sources_tsv_sha256"],
        },
        "modules": {k: mods[k] for k in sorted(mods)},
        "packs": packs,
        "prefix_cases": cases,
    }
    counts = compute_counts(mods, extras, packs, cases)
    extras_doc = {
        "schema": "arixv-gate1a-manifest-extras/1",
        "generator": MANIFEST_GENERATOR,
        "global_seed": GLOBAL_SEED,
        "parameters": {
            "same_collection_budget_bytes": SAME_COLLECTION_BUDGET,
            "byte_match_window": [BYTE_MATCH_LO, BYTE_MATCH_HI],
            "prefix_samples": PREFIX_SAMPLES,
            "stage_ref_max_gap_lines": STAGE_REF_MAX_GAP,
            "variant_ref_max_span_lines": VARIANT_REF_MAX_SPAN,
            "pack_order": "lexicographic logical key (source:input_path)",
            "prefix_case_id_grammar":
                "pc|<target>|none, |sp<j>/|vf<j> (strong tier), |sp<j>|c<k> "
                "(same-collection byte-matched to sp<j>), |sp<j>|u<k> / |t<k> (unrelated / "
                "toolchain-matched byte-matched controls), |sc<k> (32 KiB same-collection), "
                "|sc<k>|u<k'> / |t<k'>; unavailable strong tiers are |sp / |vf",
        },
        "inputs": {k: v for k, v in ver.items()
                   if k not in ("manifest_rows", "duplicate_rows", "sources_rows", "lock")},
        "rules": [{"id": r, "source": s, "pattern": p, "interpretation": d}
                  for r, s, p, d in RULES],
        "fold_merges": merges,
        "modules": {k: extras[k] for k in sorted(extras)},
        "collections": coll_summary,
        "rejected_variant_candidates": rejected_variants,
        "prefix_case_availability": case_stats,
        "counts": counts,
        "known_unavailable": known_unavailable(counts, deps),
    }
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    write_json_atomic(out / "manifest.json", manifest)
    write_json_atomic(out / "manifest_extras.json", extras_doc)
    write_json_atomic(out / "group_counts.json", counts)
    write_counts_csv(out / "group_counts.csv", counts)
    print(f"modules={len(mods)} packs={len(packs)} prefix_cases={len(cases)}")
    print(f"manifest sha256 {sha256_file(out / 'manifest.json')}")
    for k, v in case_stats.items():
        print(f"  prefix {k}: {v}")
    return 0


def compute_counts(mods: dict, extras: dict, packs: list[dict], cases: list[dict]) -> dict:
    def summarize(mids: list[str]) -> dict:
        shas = [mods[m]["sha256"] for m in mids]
        uniq = {}
        for m in mids:
            uniq.setdefault(mods[m]["sha256"], mods[m]["byte_size"])
        stages = Counter(mods[m]["stage"] or "none" for m in mids)
        debug = Counter(extras[m]["debug_class"] for m in mids)
        return {"logical_members": len(mids), "unique_members": len(uniq),
                "duplicate_entries": len(shas) - len(uniq),
                "logical_bytes": sum(mods[m]["byte_size"] for m in mids),
                "unique_bytes": sum(uniq.values()),
                "stages": dict(sorted(stages.items())),
                "debug_classes": dict(sorted(debug.items()))}

    by_rule: dict[str, list[str]] = defaultdict(list)
    by_cat: dict[str, list[str]] = defaultdict(list)
    by_origin: dict[str, list[str]] = defaultdict(list)
    by_fold: dict[str, list[str]] = defaultdict(list)
    by_coll: dict[str, list[str]] = defaultdict(list)
    for m in mods:
        by_rule[extras[m]["rule"]].append(m)
        by_cat[extras[m]["category"]].append(m)
        by_origin[mods[m]["origin_id"] or "(unknown)"].append(m)
        by_fold[mods[m]["fold_id"]].append(m)
        by_coll[mods[m]["collection_id"]].append(m)
    tiers = Counter(p["tier"] for p in packs)
    pair_counts = defaultdict(Counter)
    for c in cases:
        coll = mods[c["target_id"]]["collection_id"]
        pair_counts[coll][f"{c['tier']}/{c['availability']}"] += 1
    rule_counts = {r: summarize(by_rule.get(r, [])) for r, _, _, _ in RULES}
    rule_counts["unmatched"] = summarize(by_rule.get("unmatched", []))
    return {
        "total": summarize(list(mods)),
        "by_rule": rule_counts,
        "by_category": {k: summarize(v) for k, v in sorted(by_cat.items())},
        "by_origin": {k: summarize(v) for k, v in sorted(by_origin.items())},
        "by_fold": {k: summarize(v) for k, v in sorted(by_fold.items())},
        "by_collection": {k: {**summarize(v), "category": extras[v[0]]["category"],
                              "prefix_cases": dict(sorted(pair_counts[k].items()))}
                          for k, v in sorted(by_coll.items())},
        "packs_by_tier": dict(sorted(tiers.items())),
        "packs": [{"id": p["id"], "tier": p["tier"], "representative": p["representative"],
                   **summarize(p["module_ids"])} for p in packs],
    }


def known_unavailable(counts: dict, deps: Path | None) -> list[dict]:
    out = []
    for rule, c in counts["by_rule"].items():
        if c["logical_members"] == 0:
            out.append({"comparison": f"rule {rule}", "reason": "no corpus members"})
    out += [
        {"comparison": "stage_pair tier outside Vulkan-Samples",
         "reason": "no pipeline metadata in pinned sources (Dota 2/Blender dumps, "
                   "SPIRV-Database, test collections)"},
        {"comparison": "variant_family tier outside Vulkan-Samples",
         "reason": "no family metadata; numeric Dota 2 names and shared directories do not "
                   "establish families"},
        {"comparison": "true same-family (material permutation) tier",
         "reason": "no family metadata in any pinned source"},
        {"comparison": "smol-v talos (100) and unity (124) dumps",
         "reason": "present in pinned smol-v checkout but not *.spv, so excluded by "
                   "build_spv_corpus.py; would add two application-derived origins"},
        {"comparison": "independent-project validation for unknown-origin collections",
         "reason": "collection holdout only (shadertoy, dxc, naga, fuzzers)"},
    ]
    if deps is None:
        out.append({"comparison": "stage_pair / variant_family",
                    "reason": "--deps not supplied; sample sources not inspected"})
    return out


def write_counts_csv(path: Path, counts: dict) -> None:
    tmp = path.with_suffix(".csv.tmp")
    with tmp.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["group_kind", "group", "category", "logical_members", "unique_members",
                    "duplicate_entries", "logical_bytes", "unique_bytes", "stages",
                    "debug_classes", "prefix_cases"])
        for kind in ("by_rule", "by_category", "by_origin", "by_fold", "by_collection"):
            for g, c in counts[kind].items():
                w.writerow([kind, g, c.get("category", ""), c["logical_members"],
                            c["unique_members"], c["duplicate_entries"], c["logical_bytes"],
                            c["unique_bytes"], json.dumps(c["stages"]),
                            json.dumps(c["debug_classes"]),
                            json.dumps(c.get("prefix_cases", {}))])
        for p in counts["packs"]:
            w.writerow(["pack", p["id"], p["tier"], p["logical_members"], p["unique_members"],
                        p["duplicate_entries"], p["logical_bytes"], p["unique_bytes"],
                        json.dumps(p["stages"]), json.dumps(p["debug_classes"]), ""])
    os.replace(tmp, path)


def write_json_atomic(path: Path, obj) -> None:
    data = (json.dumps(obj, indent=1, sort_keys=False) + "\n").encode("utf-8")
    write_bytes_atomic(path, data)


def write_bytes_atomic(path: Path, data: bytes) -> None:
    tmp = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    with tmp.open("wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# --------------------------------------------------------------------------
# Validation (also used by tests and by the publish step)
# --------------------------------------------------------------------------

PACK_TIERS = {"same_collection", "stage_pair", "variant_family", "control", "unclassified"}
CASE_TIERS = {"none", "unrelated", "unrelated_toolchain_matched", "same_collection",
              "stage_pair", "variant_family"}


def validate_manifest(man: dict, check_files: bool = True) -> list[str]:
    errs = []
    if man.get("schema_version") != SCHEMA_VERSION:
        errs.append("schema_version")
    snap = man.get("snapshot", {})
    for k in ("source_lock_sha256", "manifest_sha256", "duplicates_sha256", "sources_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", str(snap.get(k, ""))):
            errs.append(f"snapshot.{k}")
    mods = man.get("modules", {})
    for mid, m in mods.items():
        for k, t in (("path", str), ("sha256", str), ("byte_size", int), ("logical_key", str),
                     ("collection_id", str), ("fold_id", str), ("spirv_version", int),
                     ("generator_word", int)):
            if not isinstance(m.get(k), t):
                errs.append(f"module {mid}.{k}")
        if m.get("origin_id") is not None and not isinstance(m["origin_id"], str):
            errs.append(f"module {mid}.origin_id")
        if check_files:
            p = Path(m["path"])
            if not p.is_file() or p.stat().st_size != m["byte_size"] or \
                    sha256_file(p) != m["sha256"]:
                errs.append(f"module {mid} file mismatch")
    fold_of_sha: dict[str, set[str]] = defaultdict(set)
    for m in mods.values():
        fold_of_sha[m["sha256"]].add(m["fold_id"])
    for digest, folds in fold_of_sha.items():
        if len(folds) > 1:
            errs.append(f"content {digest[:12]} crosses folds {sorted(folds)}")
    ids = set()
    for p in man.get("packs", []):
        if p["id"] in ids:
            errs.append(f"duplicate pack id {p['id']}")
        ids.add(p["id"])
        if p["tier"] not in PACK_TIERS:
            errs.append(f"pack {p['id']} tier")
        if not p["module_ids"] or any(m not in mods for m in p["module_ids"]):
            errs.append(f"pack {p['id']} members")
        if not isinstance(p.get("representative"), bool) or not isinstance(p.get("evidence"), str):
            errs.append(f"pack {p['id']} fields")
    cids = set()
    for c in man.get("prefix_cases", []):
        if c["id"] in cids:
            errs.append(f"duplicate case id {c['id']}")
        cids.add(c["id"])
        if c["tier"] not in CASE_TIERS or c["availability"] not in ("available", "unavailable"):
            errs.append(f"case {c['id']} tier/availability")
        if c["target_id"] not in mods or any(m not in mods for m in c["prefix_ids"]):
            errs.append(f"case {c['id']} ids")
        if c["availability"] == "unavailable" and (c["prefix_ids"] or not c["reason"]):
            errs.append(f"case {c['id']} unavailable must have reason and no prefix")
        if c["availability"] == "available" and c["tier"] != "none" and not c["prefix_ids"]:
            errs.append(f"case {c['id']} empty prefix")
        t = mods.get(c["target_id"])
        if t and any(mods[m]["sha256"] == t["sha256"] for m in c["prefix_ids"]):
            errs.append(f"case {c['id']} prefix contains target content")
        if t and c["tier"].startswith("unrelated") and any(
                mods[m]["fold_id"] == t["fold_id"] for m in c["prefix_ids"]):
            errs.append(f"case {c['id']} unrelated prefix shares fold")
        if not isinstance(c.get("seed"), int):
            errs.append(f"case {c['id']} seed")
    return errs


# --------------------------------------------------------------------------
# Publish
# --------------------------------------------------------------------------

def publish(args: argparse.Namespace) -> int:
    src = args.manifest.resolve()
    data = src.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    man = json.loads(data)
    errs = validate_manifest(man, check_files=True)
    if errs:
        raise SystemExit("manifest invalid:\n  " + "\n  ".join(errs[:30]))
    report = json.loads(args.verification.read_text(encoding="utf-8"))
    if report.get("manifest_sha256") != digest:
        raise SystemExit("baseline verification report is for a different manifest")
    verified = bool(report.get("baseline_identity_verified"))
    if not verified and not args.allow_unverified:
        raise SystemExit("baseline identity not verified; refusing to publish ready marker")
    dest_manifest = args.manifest_dest
    dest_ready = args.ready_dest
    for p in (dest_manifest, dest_ready):
        if p.exists() and not args.replace_unready:
            existing_ready = dest_ready.exists()
            raise SystemExit(
                f"{p} exists{' (ready snapshot published)' if existing_ready else ''}; "
                "never modify a published snapshot — publish a versioned replacement with "
                "--manifest-dest/--ready-dest instead")
    dest_manifest.parent.mkdir(parents=True, exist_ok=True)
    write_bytes_atomic(dest_manifest, data)
    if hashlib.sha256(dest_manifest.read_bytes()).hexdigest() != digest:
        raise SystemExit("published manifest hash mismatch")
    for extra in args.extra or []:
        write_bytes_atomic(dest_manifest.parent / extra.name, extra.read_bytes())
    ready = {
        "contract_version": CONTRACT_VERSION,
        "manifest_sha256": digest,
        "baseline_identity_verified": verified,
        "notes": report.get("notes", ""),
    }
    write_json_atomic(dest_ready, ready)
    print(f"published {dest_manifest} sha256={digest}")
    print(f"ready {dest_ready} sha256={sha256_file(dest_ready)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="verify inputs and build the frozen manifest")
    b.add_argument("--corpus", type=Path, required=True,
                   help="corpus dir with manifest.tsv, duplicates.tsv, sources.tsv, spv/")
    b.add_argument("--deps", type=Path, default=None,
                   help="pinned benchmark-deps checkout dir (verified against the lockfile)")
    b.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    b.add_argument("--out", type=Path, default=ROOT / "build" / "gate1a" / "manifest")
    p = sub.add_parser("publish", help="atomically publish manifest then ready marker")
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--verification", type=Path, required=True,
                   help="baseline_verification.json from multi_module_baselines.py")
    p.add_argument("--manifest-dest", type=Path, required=True)
    p.add_argument("--ready-dest", type=Path, required=True)
    p.add_argument("--extra", type=Path, action="append",
                   help="additional handoff file copied next to the manifest")
    p.add_argument("--allow-unverified", action="store_true",
                   help="publish ready with baseline_identity_verified=false")
    p.add_argument("--replace-unready", action="store_true",
                   help="allow replacing files when no ready marker was published yet")
    v = sub.add_parser("validate", help="validate a manifest against the contract schema")
    v.add_argument("manifest", type=Path)
    v.add_argument("--no-files", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd == "build":
        return build(args)
    if args.cmd == "publish":
        if args.replace_unready and args.ready_dest.exists():
            raise SystemExit("ready marker already published; use a versioned destination")
        return publish(args)
    errs = validate_manifest(json.loads(args.manifest.read_text()), not args.no_files)
    for e in errs:
        print(e)
    print("ok" if not errs else f"{len(errs)} errors")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
