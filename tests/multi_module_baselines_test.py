#!/usr/bin/env python3
"""Tests for the gate-1a multi-module manifest and baseline tools.

Run: python3 -m unittest tests/multi_module_baselines_test.py
Set ARIXV_CLI to an arixv binary (default: build/default/arixv if present) to
also exercise exact ARX2 reconstruction + decode through the unchanged CLI.
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import multi_module_baselines as mmb  # noqa: E402
import multi_module_manifest as mmm  # noqa: E402

GOLDEN = ROOT / "tests" / "golden"


def arixv_cli() -> Path | None:
    p = Path(os.environ.get("ARIXV_CLI", ROOT / "build" / "default" / "arixv"))
    return p if p.is_file() and os.access(p, os.X_OK) else None


def spirv(words: list[int], version: int = 0x10000, generator: int = 0x80001) -> bytes:
    hdr = [mmm.SPIRV_MAGIC, version, generator, 100, 0]
    return struct.pack(f"<{len(hdr) + len(words)}I", *hdr, *words)


def inst(op: int, *operands: int) -> list[int]:
    return [((len(operands) + 1) << 16) | op, *operands]


def literal_string(s: str) -> list[int]:
    b = s.encode() + b"\0"
    b += b"\0" * (-len(b) % 4)
    return list(struct.unpack(f"<{len(b) // 4}I", b))


class CompactUint32Test(unittest.TestCase):
    # Boundaries from src/container.h.
    CASES = [(0, b"\x00"), (191, b"\xbf"), (192, b"\xc0\x00"), (15551, b"\xfb\xff"),
             (15552, b"\xfc\x00\x00"), (81087, b"\xfc\xff\xff"),
             (81088, b"\xfd\x00\x00\x00"), (16858303, b"\xfd\xff\xff\xff"),
             (16858304, b"\xfe" + struct.pack("<I", 16858304)),
             (0xFFFFFFFF, b"\xfe\xff\xff\xff\xff")]

    def test_boundaries(self):
        for v, enc in self.CASES:
            self.assertEqual(mmb.compact_u32(v), enc, v)
            self.assertEqual(mmb.read_compact_u32(enc + b"x", 0), (v, len(enc)))

    def test_rejects_invalid(self):
        with self.assertRaises(ValueError):
            mmb.read_compact_u32(b"\xff", 0)
        with self.assertRaises(ValueError):  # non-canonical 5-byte form
            mmb.read_compact_u32(b"\xfe\x01\x00\x00\x00", 0)
        with self.assertRaises(ValueError):
            mmb.compact_u32(-1)

    def test_matches_golden_arx2_headers(self):
        # Golden blobs carry the canonical size field written by the C++ encoder.
        for p in sorted(GOLDEN.glob("*.arx2")):
            blob = p.read_bytes()
            mode, size_field, off = mmb.parse_arx2(blob)
            self.assertEqual(blob[5:off], mmb.compact_u32(size_field), p.name)
            self.assertIn(mode, (0, 1, 2))


class FramingTest(unittest.TestCase):
    def test_mmf_roundtrip_and_overhead(self):
        for members in ([], [b""], [b"a"], [b"x" * 200, b"", b"y" * 16000]):
            f = mmb.mmf_frame(members)
            self.assertEqual(mmb.mmf_parse(f), members)
            self.assertEqual(len(f), mmb.mmf_overhead(members) + sum(map(len, members)))

    def test_mmf_rejects_trailing_and_truncated(self):
        f = mmb.mmf_frame([b"abc"])
        with self.assertRaises(ValueError):
            mmb.mmf_parse(f + b"\0")
        with self.assertRaises(ValueError):
            mmb.mmf_parse(f[:-1])

    def golden_blobs(self) -> list[bytes]:
        return [p.read_bytes() for p in sorted(GOLDEN.glob("*.arx2"))]

    def test_shared_header_reconstructs_exact_blobs(self):
        blobs = self.golden_blobs()
        self.assertGreaterEqual(len(blobs), 4)
        for subset in (blobs, blobs[:1], blobs[::-1] + blobs[:2]):
            packed = mmb.shared_header_pack(subset)
            self.assertEqual(packed[:4], mmb.SET_MAGIC)
            self.assertEqual(mmb.shared_header_unpack(packed), subset)
            payload = sum(len(b) - mmb.parse_arx2(b)[2] for b in subset)
            self.assertEqual(len(packed), mmb.shared_header_metadata(subset) + payload)
            # The wrapper saves exactly the repeated per-blob magic, net of its own
            # set magic, relative to MMF framing of the standalone blobs (the
            # per-member payload length replaces the MMF blob length and is at most
            # the same size).
            framed = mmb.mmf_frame(subset)
            self.assertLessEqual(len(packed), len(framed) - 4 * (len(subset) - 1))

    def test_shared_header_rejects_corruption(self):
        packed = mmb.shared_header_pack(self.golden_blobs())
        with self.assertRaises(ValueError):
            mmb.shared_header_unpack(b"ARX2" + packed[4:])
        with self.assertRaises(ValueError):
            mmb.shared_header_unpack(packed + b"\0")

    @unittest.skipIf(arixv_cli() is None, "arixv CLI not built")
    def test_shared_header_blobs_decode_through_cli(self):
        cli = arixv_cli()
        blobs = self.golden_blobs()
        rec = mmb.shared_header_unpack(mmb.shared_header_pack(blobs))
        with tempfile.TemporaryDirectory() as td:
            for i, (orig, back) in enumerate(zip(blobs, rec)):
                outs = []
                for tag, blob in (("o", orig), ("r", back)):
                    a, b = Path(td, f"{i}{tag}.arx2"), Path(td, f"{i}{tag}.spv")
                    a.write_bytes(blob)
                    subprocess.run([str(cli), "d", str(a), str(b)], check=True)
                    outs.append(b.read_bytes())
                self.assertEqual(outs[0], outs[1])

    @unittest.skipIf(arixv_cli() is None, "arixv CLI not built")
    def test_arx2_encode_roundtrip_helper(self):
        mod = spirv(inst(17, 1) + inst(14, 0, 1) + inst(15, 4, 1, *literal_string("main")))
        with tempfile.TemporaryDirectory() as td:
            src = Path(td, "m.spv")
            src.write_bytes(mod)
            r = mmb._arx2_one((str(arixv_cli()), str(src), str(Path(td, "m.arx2")), "x"))
            self.assertTrue(r["ok"], r)
            self.assertEqual(r["bytes"], Path(td, "m.arx2").stat().st_size)

    def test_dedup_roundtrip(self):
        a, b, c = b"A" * 10, b"B" * 300, b"C"
        members = [a, b, a, c, b, b]
        d = mmb.dedup_pack(members)
        self.assertEqual(mmb.dedup_unpack(d), members)
        # Each unique blob stored exactly once.
        self.assertEqual(d.count(b"B" * 300), 1)
        with self.assertRaises(ValueError):
            mmb.dedup_unpack(mmb.compact_u32(1) + mmb.compact_u32(1))

    def test_canonical_tar_deterministic(self):
        members = [b"one", b"", b"three" * 1000]
        t1, t2 = mmb.canonical_tar(members, ".spv"), mmb.canonical_tar(members, ".spv")
        self.assertEqual(t1, t2)
        self.assertEqual(mmb.untar(t1), members)
        self.assertEqual(len(t1) % 10240, 0)


class CodecTest(unittest.TestCase):
    @unittest.skipIf(mmb._zstd is None, "compression.zstd unavailable")
    def test_zstd_pinned_window_and_rawdict(self):
        data = os.urandom(4096) * 4
        z = mmb.zstd_c(data)
        self.assertEqual(mmb.zstd_d(z), data)
        self.assertGreaterEqual(mmb.zstd_frame_window(z), len(data))
        big = os.urandom(1 << 16) * 160  # 10 MiB > 2^23 window
        w = mmb.zstd_frame_window(mmb.zstd_c(big))
        self.assertEqual(w, 1 << 23)
        prefix = os.urandom(8192)
        zd = mmb._zstd.ZstdDict(prefix, is_raw=True)
        target = prefix[100:6000]
        with_dict = mmb.zstd_c(target, "zstd19", zd)
        self.assertEqual(mmb.zstd_d(with_dict, zd), target)
        self.assertLess(len(with_dict), len(mmb.zstd_c(target)) // 4)

    def test_xz_conditional_is_finalized_difference(self):
        prefix = [os.urandom(3000), os.urandom(3000)]
        tgt = prefix[0][:2000]
        full = mmb.xz_c(mmb.mmf_frame(prefix + [tgt]))
        self.assertEqual(mmb.mmf_parse(mmb.xz_d(full)), prefix + [tgt])
        cost = len(full) - len(mmb.xz_c(mmb.mmf_frame(prefix)))
        self.assertLess(cost, len(mmb.xz_c(mmb.mmf_frame([tgt]))))


class SpirvScanTest(unittest.TestCase):
    def test_stage_debug_and_header(self):
        body = (inst(17, 1) + inst(14, 0, 1) + inst(15, 0, 1, *literal_string("vs")) +
                inst(15, 4, 2, *literal_string("fs")) + inst(5, 1, *literal_string("x")))
        info = mmm.scan_spirv(spirv(body, 0x10300, 0xd000b))
        self.assertTrue(info.valid_header)
        self.assertEqual(info.stage, "multi:fragment+vertex")
        self.assertEqual((info.spirv_version, info.generator_word), (0x10300, 0xd000b))
        self.assertEqual(info.debug_class, "debug_bearing")
        stripped = mmm.scan_spirv(spirv(inst(17, 1) + inst(15, 5, 1, *literal_string("m"))))
        self.assertEqual((stripped.stage, stripped.debug_class), ("compute", "stripped"))

    def test_byte_swapped_and_malformed(self):
        le = spirv(inst(17, 1) + inst(15, 4, 1, *literal_string("m")))
        be = b"".join(le[i:i + 4][::-1] for i in range(0, len(le), 4))
        info = mmm.scan_spirv(be)
        self.assertTrue(info.byte_swapped and info.valid_header)
        self.assertEqual(info.stage, "fragment")
        self.assertIsNotNone(mmm.scan_spirv(b"\x03\x02\x23\x07" + b"\0" * 17).parse_error)
        bad = mmm.scan_spirv(spirv([0xFFFF0011]))
        self.assertIn("bad word count", bad.parse_error)


class ClassificationTest(unittest.TestCase):
    LOCK = {"vulkan-samples": {"directory": "Vulkan-Samples"}}

    def c(self, source, path):
        return mmm.classify(source, path, None, self.LOCK)

    def test_rule_table(self):
        d = self.c("smol-v", "tests/spirv-dumps/dota2/366476.spv")
        self.assertEqual((d.collection_id, d.origin_id, d.category),
                         ("smolv/dota2", "valve-dota2", "application_derived"))
        v = self.c("vulkan-samples", "shaders/hdr/hlsl/bloom.frag.spv")
        self.assertEqual((v.collection_id, v.language, v.sample), ("vks/hdr/hlsl", "hlsl", "hdr"))
        nested = self.c("vulkan-samples",
                        "shaders/tensor_and_data_graph/compute_shaders_with_tensors/glsl/a.comp.spv")
        self.assertEqual(nested.collection_id,
                         "vks/tensor_and_data_graph/compute_shaders_with_tensors/glsl")
        root = self.c("vulkan-samples", "shaders/base.vert.spv")
        self.assertEqual((root.collection_id, root.category), ("vks/_root", "control"))
        cts = self.c("spirv-database", "vulkan/gl_cts/zink/functional/shader/es2/dump1.spv")
        self.assertEqual(cts.collection_id, "spvdb/gl_cts/zink/functional/shader/es2")
        self.assertEqual(cts.origin_id, "khronos-gl-cts")
        mirror = self.c("yari-v", "test/external/dota2/1.spv")
        self.assertFalse(mirror.in_packs)
        self.assertEqual(mirror.base_fold, "origin:valve-dota2")
        fuzz = self.c("spirv-tools", "test/fuzzers/corpora/spv/graphicsfuzz_000.spv")
        self.assertEqual(fuzz.category, "fuzz_test")

    def test_unmatched_defaults_to_unclassified(self):
        for src, path in (("smol-v", "tests/spirv-dumps/new/x.spv"), ("other", "a.spv"),
                          ("spirv-database", "vulkan/unknown/x.spv")):
            u = self.c(src, path)
            self.assertEqual((u.rule, u.category, u.origin_id), ("unmatched", "unclassified", None))
            self.assertFalse(u.category in mmm.CATEGORY_REPRESENTATIVE)


def mk_mod(coll, origin, fold, size, digest, gen=0x80001, ver=0x10000, stage="vertex"):
    return {"path": "/x", "sha256": digest, "byte_size": size, "logical_key": "",
            "origin_id": origin, "collection_id": coll, "fold_id": fold, "stage": stage,
            "spirv_version": ver, "generator_word": gen}


def mk_ex(cat="sample_collection", in_packs=True, canon=True):
    return {"category": cat, "in_packs": in_packs, "canonical_in_collection": canon}


class PrefixSelectionTest(unittest.TestCase):
    def setUp(self):
        h = lambda s: hashlib.sha256(s.encode()).hexdigest()  # noqa: E731
        self.mods = {
            "a/v": mk_mod("A", "oa", "fa", 1000, h("av")),
            "a/f": mk_mod("A", "oa", "fa", 1200, h("af"), stage="fragment"),
            "a/x": mk_mod("A", "oa", "fa", 900, h("ax")),
            "a/dup": mk_mod("A", "oa", "fa", 1000, h("av")),     # same content as a/v
            "b/1": mk_mod("B", "ob", "fb", 500, h("b1")),
            "b/2": mk_mod("B", "ob", "fb", 700, h("b2"), gen=0xd000b),
            "b/3": mk_mod("B", "ob", "fb", 600, h("b3")),
            "c/1": mk_mod("C", None, "fc", 1100, h("c1")),     # unknown origin
        }
        self.extras = {k: mk_ex() for k in self.mods}
        self.extras["a/dup"]["canonical_in_collection"] = False
        self.packs = [
            {"id": "coll:A", "tier": "same_collection", "collection_id": "A",
             "module_ids": ["a/dup", "a/f", "a/v", "a/x"]},
            {"id": "coll:A#unique", "tier": "same_collection", "collection_id": "A",
             "module_ids": ["a/f", "a/v", "a/x"]},
            {"id": "coll:B", "tier": "same_collection", "collection_id": "B",
             "module_ids": ["b/1", "b/2", "b/3"]},
            {"id": "pair:A:0", "tier": "stage_pair", "collection_id": "A",
             "module_ids": ["a/f", "a/v"]},
        ]

    def test_byte_matched_sample(self):
        sizes = {"p": 100, "q": 300, "r": 1000}
        pick = mmm.byte_matched_sample(list(sizes), sizes, 400, seed=1)
        self.assertIsNotNone(pick)
        self.assertTrue(0.8 * 400 <= sum(sizes[m] for m in pick) <= 1.25 * 400)
        self.assertEqual(pick, mmm.byte_matched_sample(list(sizes), sizes, 400, seed=1))
        self.assertIsNone(mmm.byte_matched_sample(["p"], sizes, 400, seed=1))

    def test_cases_respect_tiers_and_controls(self):
        cases, stats = mmm.build_prefix_cases(self.mods, self.extras, self.packs)
        by_id = {c["id"]: c for c in cases}
        man = {"modules": self.mods}
        # c/1 has no collection pack, so it is never a target.
        self.assertFalse(any(c["target_id"] == "c/1" for c in cases))
        # Every target has a no-prefix case.
        for t in ("a/v", "a/f", "a/x", "b/1", "b/2", "b/3"):
            self.assertEqual(by_id[f"pc|{t}|none"]["prefix_ids"], [])
        # Stage pair: partner only.
        self.assertEqual(by_id["pc|a/v|sp0"]["prefix_ids"], ["a/f"])
        self.assertEqual(by_id["pc|a/x|sp"]["availability"], "unavailable")
        # Strong-tier-matched same-collection prefix excludes the partner and target content.
        for k in range(mmm.PREFIX_SAMPLES):
            c = by_id[f"pc|a/v|sp0|c{k}"]
            self.assertNotIn("a/f", c["prefix_ids"])
            self.assertNotIn("a/dup", c["prefix_ids"])
        for c in cases:
            t = self.mods[c["target_id"]]
            for m in c["prefix_ids"]:
                self.assertNotEqual(self.mods[m]["sha256"], t["sha256"], c["id"])
                if c["tier"].startswith("unrelated"):
                    self.assertNotEqual(self.mods[m]["fold_id"], t["fold_id"])
                    self.assertIsNotNone(self.mods[m]["origin_id"])
                if c["tier"] == "unrelated_toolchain_matched":
                    self.assertEqual(self.mods[m]["generator_word"], t["generator_word"])
                    self.assertEqual(self.mods[m]["spirv_version"], t["spirv_version"])
            if c["availability"] == "unavailable":
                self.assertTrue(c["reason"])
                self.assertEqual(c["prefix_ids"], [])
        # b/2's generator word is unique: toolchain-matched control must be unavailable.
        self.assertTrue(all(c["availability"] == "unavailable" for c in cases
                            if c["target_id"] == "b/2"
                            and c["tier"] == "unrelated_toolchain_matched"))
        # Seeds are deterministic functions of the case id.
        again, _ = mmm.build_prefix_cases(self.mods, self.extras, self.packs)
        self.assertEqual(cases, again)
        self.assertTrue(all(c["seed"] == mmm.case_seed(c["id"]) for c in cases))
        self.assertEqual(sum(stats.values()), len(cases))
        del man


class ValidationTest(unittest.TestCase):
    def base(self):
        h = "0" * 64
        mods = {"m1": mk_mod("A", "o", "f1", 4, "1" * 64), "m2": mk_mod("A", "o", "f1", 4, "2" * 64)}
        return {"schema_version": 1,
                "snapshot": {k: h for k in ("source_lock_sha256", "manifest_sha256",
                                            "duplicates_sha256", "sources_sha256")},
                "modules": mods,
                "packs": [{"id": "p", "module_ids": ["m1", "m2"], "tier": "same_collection",
                           "origin_id": "o", "collection_id": "A", "fold_id": "f1",
                           "representative": True, "evidence": "e"}],
                "prefix_cases": [{"id": "c", "target_id": "m1", "prefix_ids": ["m2"],
                                  "tier": "same_collection", "availability": "available",
                                  "reason": None, "seed": 1}]}

    def test_valid(self):
        self.assertEqual(mmm.validate_manifest(self.base(), check_files=False), [])

    def test_detects_violations(self):
        m = self.base()
        m["modules"]["m3"] = mk_mod("B", "p", "f2", 4, "1" * 64)  # same content, other fold
        self.assertTrue(any("crosses folds" in e for e in mmm.validate_manifest(m, False)))
        m = self.base()
        m["prefix_cases"][0].update(availability="unavailable")
        self.assertTrue(any("unavailable" in e for e in mmm.validate_manifest(m, False)))
        m = self.base()
        m["modules"]["m2"]["sha256"] = "1" * 64
        self.assertTrue(any("target content" in e for e in mmm.validate_manifest(m, False)))
        m = self.base()
        m["packs"][0]["tier"] = "family"
        self.assertTrue(mmm.validate_manifest(m, False))


class VksRelationshipTest(unittest.TestCase):
    def test_stage_pairs_and_variants_need_source_evidence(self):
        names = ["a.vert.spv", "a.frag.spv", "b.vert.spv", "b.frag.spv",
                 "cull.comp.spv", "cull_fp16.comp.spv", "blur.comp.spv", "blur_x.comp.spv"]
        mods = {f"vks:shaders/s/glsl/{n}": {"collection_id": "vks/s/glsl",
                                             "logical_key": f"vks:shaders/s/glsl/{n}"}
                for n in names}
        extras = {k: {"rule": "vks-sample", "sample": "s"} for k in mods}
        src = "samples/api/s/s.cpp"
        refs = {
            "a.vert.spv": [(src, 10, "s/a.vert.spv")], "a.frag.spv": [(src, 11, "s/a.frag.spv")],
            # b.* too far apart: no stage-pair evidence
            "b.vert.spv": [(src, 100, "s/b.vert.spv")], "b.frag.spv": [(src, 150, "s/b.frag.spv")],
            "cull.comp.spv": [(src, 200, "s/cull.comp.spv")],
            "cull_fp16.comp.spv": [(src, 210, "s/cull_fp16.comp.spv")],
            # blur_x only referenced from a different sample's source
            "blur.comp.spv": [(src, 300, "s/blur.comp.spv")],
            "blur_x.comp.spv": [("samples/api/t/t.cpp", 5, "t/blur_x.comp.spv")],
        }
        packs, rejected = mmm.find_vks_relationships(mods, extras, refs, "0" * 40)
        got = {(p.tier, tuple(sorted(m.rsplit("/", 1)[1] for m in p.module_ids))) for p in packs}
        self.assertIn(("stage_pair", ("a.frag.spv", "a.vert.spv")), got)
        self.assertNotIn(("stage_pair", ("b.frag.spv", "b.vert.spv")), got)
        self.assertIn(("variant_family", ("cull.comp.spv", "cull_fp16.comp.spv")), got)
        self.assertFalse(any("blur" in "".join(k[1]) for k in got))
        self.assertTrue(any(r["stems"] == ["blur", "blur_x"] for r in rejected))


class InputVerificationTest(unittest.TestCase):
    def make_corpus(self, td: Path) -> tuple[Path, Path, Path]:
        corpus, deps = td / "corpus", td / "deps"
        (corpus / "spv").mkdir(parents=True)
        src_dir = deps / "smol-v" / "tests" / "spirv-dumps" / "dota2"
        src_dir.mkdir(parents=True)
        data = spirv(inst(17, 1))
        digest = hashlib.sha256(data).hexdigest()
        (corpus / "spv" / f"{digest}.spv").write_bytes(data)
        (src_dir / "1.spv").write_bytes(data)
        (src_dir / "2.spv").write_bytes(data)
        hdr = "sha256\tbytes\tsource\tinput_path\tcanonical_name\n"
        (corpus / "manifest.tsv").write_text(
            hdr + f"{digest}\t{len(data)}\tsmol-v\ttests/spirv-dumps/dota2/1.spv\t{digest}.spv\n")
        (corpus / "duplicates.tsv").write_text(
            hdr + f"{digest}\t{len(data)}\tsmol-v\ttests/spirv-dumps/dota2/2.spv\t{digest}.spv\n")
        rev = subprocess.run(["git", "init", "-q", str(deps / "smol-v")], check=True)
        del rev
        env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
               "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
        subprocess.run(["git", "-C", str(deps / "smol-v"), "add", "-A"], check=True, env=env)
        subprocess.run(["git", "-C", str(deps / "smol-v"), "commit", "-qm", "x"], check=True,
                       env=env)
        head = mmm.git_head(deps / "smol-v")
        (corpus / "sources.tsv").write_text(
            "source\tgit_url\trevision\tcandidates\tunique_selected\n"
            f"smol-v\thttps://example.invalid/smol-v.git\t{head}\t2\t1\n")
        lock = td / "lock.json"
        lock.write_text(json.dumps({"schema_version": 1, "sources": [
            {"name": "smol-v", "directory": "smol-v", "url": "https://example.invalid/smol-v.git",
             "revision": head}]}))
        return corpus, deps, lock

    def test_accepts_consistent_and_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as td:
            corpus, deps, lock = self.make_corpus(Path(td))
            ver = mmm.verify_inputs(corpus, deps, lock)
            self.assertEqual((ver["canonical_files_verified"], ver["aliases_verified"],
                              ver["upstream_files_verified"]), (1, 1, 2))
            # Tampered upstream alias source is detected.
            alias = deps / "smol-v" / "tests" / "spirv-dumps" / "dota2" / "2.spv"
            alias.write_bytes(alias.read_bytes() + b"\0\0\0\0")
            with self.assertRaises(SystemExit):
                mmm.verify_inputs(corpus, deps, lock)
            subprocess.run(["git", "-C", str(deps / "smol-v"), "checkout", "-q", "--", "."],
                           check=True)
            mmm.verify_inputs(corpus, deps, lock)
            # Corrupted canonical file is detected.
            f = next((corpus / "spv").glob("*.spv"))
            f.write_bytes(b"\0" + f.read_bytes()[1:])
            with self.assertRaises(SystemExit):
                mmm.verify_inputs(corpus, None, lock)


class PublishTest(unittest.TestCase):
    def test_ready_written_last_and_never_overwritten(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            spv_file = td / "m.spv"
            spv_file.write_bytes(spirv(inst(17, 1)))
            digest = mmm.sha256_file(spv_file)
            man = ValidationTest().base()
            man["modules"] = {"m1": {**mk_mod("A", "o", "f", spv_file.stat().st_size, digest),
                                     "path": str(spv_file)}}
            man["packs"][0]["module_ids"] = ["m1"]
            man["prefix_cases"] = []
            src = td / "manifest.json"
            mmm.write_json_atomic(src, man)
            msha = mmm.sha256_file(src)
            ver = td / "ver.json"
            ver.write_text(json.dumps({"manifest_sha256": "f" * 64,
                                       "baseline_identity_verified": True}))
            dest, ready = td / "coord" / "manifest.json", td / "coord" / "manifest.ready.json"
            argv = ["publish", "--manifest", str(src), "--verification", str(ver),
                    "--manifest-dest", str(dest), "--ready-dest", str(ready)]
            with self.assertRaises(SystemExit):   # report for another manifest
                mmm.main(argv)
            self.assertFalse(ready.exists())
            ver.write_text(json.dumps({"manifest_sha256": msha,
                                       "baseline_identity_verified": False}))
            with self.assertRaises(SystemExit):   # unverified baselines
                mmm.main(argv)
            ver.write_text(json.dumps({"manifest_sha256": msha, "notes": "n",
                                       "baseline_identity_verified": True}))
            self.assertEqual(mmm.main(argv), 0)
            r = json.loads(ready.read_text())
            self.assertEqual(r, {"contract_version": 1, "manifest_sha256": msha,
                                 "baseline_identity_verified": True, "notes": "n"})
            self.assertEqual(mmm.sha256_file(dest), msha)
            self.assertLessEqual(dest.stat().st_mtime_ns, ready.stat().st_mtime_ns)
            with self.assertRaises(SystemExit):   # published snapshot is immutable
                mmm.main(argv)
            with self.assertRaises(SystemExit):
                mmm.main(argv + ["--replace-unready"])


if __name__ == "__main__":
    unittest.main()
