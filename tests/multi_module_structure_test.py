#!/usr/bin/env python3
"""Tests for the gate-2 repeated-structure probe (multi_module_structure.py).

Run: python3 -m unittest tests/multi_module_structure_test.py
Set ARIXV_STRUCTURE_DUMP to an arixv_structure_dump binary (default:
build/research/arixv_structure_dump if present) to also check the dump tool on
synthetic SPIR-V: payload/cost consistency and ID-role masks.
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import multi_module_structure as mms  # noqa: E402


def spirv(body_insts, bound):
    words = [0x07230203, 0x00010300, 0, bound, 0]
    for op, operands in body_insts:
        words.append(((1 + len(operands)) << 16) | op)
        words.extend(operands)
    return struct.pack(f"<{len(words)}I", *words)


def module_insts(shift=0, extra=False):
    """A small valid-looking module; `shift` renumbers every ID."""
    s = shift
    insts = [
        (17, [1]),                          # OpCapability Shader
        (14, [0, 1]),                       # OpMemoryModel
        (19, [1 + s]),                      # OpTypeVoid %1
        (33, [2 + s, 1 + s]),               # OpTypeFunction %2 %1
        (22, [3 + s, 32]),                  # OpTypeFloat %3 32
        (23, [4 + s, 3 + s, 4]),            # OpTypeVector %4 %3 4
        (43, [3 + s, 5 + s, 0x3F800000]),   # OpConstant %3 %5 1.0
        (54, [1 + s, 6 + s, 0, 2 + s]),     # OpFunction
        (248, [7 + s]),                     # OpLabel
    ]
    if extra:
        insts.append((43, [3 + s, 9 + s, 0x40000000]))
    insts += [(253, []), (56, [])]          # OpReturn, OpFunctionEnd
    return insts


def fake_mod(path, insts, bits_per_inst=20.0):
    """A Mod as read_dump would produce, with a hand-written ID mask."""
    roles = {19: [1], 33: [1, 2], 22: [1, 0], 23: [1, 2, 0], 43: [2, 1, 0],
             54: [2, 1, 0, 2], 248: [1]}
    m = mms.Mod()
    m.path = path
    m.status, m.swapped, m.size, m.payload = 0, 0, 0, 0
    m.header_bits = 8.0
    m.words, m.mask, m.bits = [], [], []
    for op, ops in insts:
        w = tuple([((1 + len(ops)) << 16) | op] + ops)
        r = roles.get(op, [0] * len(ops))
        m.words.append(w)
        m.mask.append(bytes([0] + r))
        m.bits.append(bits_per_inst)
    m.n = len(m.words)
    return m


def prepared(mods):
    """Mirror of mms.prepare() without reading files (bound set directly)."""
    ex, ab = {}, {}
    for m in mods:
        m.trace_bits = m.header_bits + sum(m.bits)
        m.section, m.idpos, m.tok_exact, m.tok_abs = [], [], [], []
        for w, mk in zip(m.words, m.mask):
            m.section.append("function")
            ids = tuple((i, mk[i]) for i in range(len(w)) if mk[i])
            m.idpos.append(ids)
            m.tok_exact.append(ex.setdefault(w, len(ex)))
            aw = list(w)
            for i, role in ids:
                aw[i] = -role
            m.tok_abs.append(ab.setdefault(tuple(aw), len(ab)))
        m.bound = 64
    return {m.path: m for m in mods}


class ParserTest(unittest.TestCase):
    def test_gamma(self):
        self.assertEqual([mms.gamma(n) for n in (1, 2, 3, 4, 8)],
                         [1, 3, 3, 5, 7])

    def test_shifted_ids_copy_only_when_abstract(self):
        a = fake_mod("a", module_insts())
        b = fake_mod("b", module_insts(shift=10, extra=True))
        prepared([a, b])
        for abstract, expect_copy in ((False, False), (True, True)):
            p = mms.Parser([a, b], abstract, 8)
            st0, _ = p.parse_module(0)
            self.assertEqual(st0["copies"], 0)
            st1, cov = p.parse_module(1)  # verifies reconstruction
            self.assertEqual(st1["copies"] > 0, expect_copy)
            if expect_copy:
                est = (st1["literal_bits"] + st1["flag_bits"] +
                       st1["src_bits"] + st1["len_bits"] + st1["id_bits"])
                self.assertLess(est, sum(b.bits))
                # Only the first shifted ID is explicit; the delta rule (and
                # then the correspondence map) predicts every later one.
                self.assertEqual(st1[("id_choice", 2)], 1)

    def test_identical_module_exact_copy(self):
        a = fake_mod("a", module_insts())
        b = fake_mod("b", module_insts())
        prepared([a, b])
        p = mms.Parser([a, b], False, 8)
        p.parse_module(0)
        st, _ = p.parse_module(1)
        self.assertEqual(st["literal_bits"], 0)
        self.assertEqual(st["copies"], 1)

    def test_reconstruction_detects_corruption(self):
        a = fake_mod("a", module_insts())
        b = fake_mod("b", module_insts(shift=3))
        prepared([a, b])
        nid = sum(len(x) for x in a.idpos)
        first = next(w[i] for w, x in zip(b.words, b.idpos) for i, _ in x)
        # First ID explicit, then the delta rule for every other ID: OK.
        ops = [("copy", 0, 0, b.n, [first] + [-2] * (nid - 1), 0)]
        mms.check_reconstruction([a, b], b, ops, True)
        bad = [("copy", 0, 0, b.n, [-2] * nid, 0)]  # wrong first delta
        with self.assertRaises((SystemExit, KeyError)):
            mms.check_reconstruction([a, b], b, bad, True)
        with self.assertRaises(SystemExit):
            mms.check_reconstruction([a, b], b, [("lit", a.words[0])], True)


class BlocksTest(unittest.TestCase):
    def test_split_blocks(self):
        members = {f"m{i}": {"byte_size": b}
                   for i, b in enumerate((10, 10, 30, 5, 50))}
        pack = {"module_ids": [f"m{i}" for i in range(5)]}
        ids = lambda bl: [[int(m[1:]) for m in b] for b in bl]
        self.assertEqual(ids(mms.split_blocks(pack, members, count=2)),
                         [[0, 1], [2, 3], [4]])
        self.assertEqual(ids(mms.split_blocks(pack, members)),
                         [[0, 1, 2, 3, 4]])
        # A block closes before a member would exceed the cap; an oversized
        # member still forms its own block.
        self.assertEqual(ids(mms.split_blocks(pack, members, cap=40)),
                         [[0, 1], [2, 3], [4]])


def structure_dump() -> Path | None:
    env = os.environ.get("ARIXV_STRUCTURE_DUMP")
    p = Path(env) if env else ROOT / "build" / "research" / \
        "arixv_structure_dump"
    return p if p.is_file() else None


@unittest.skipIf(structure_dump() is None, "arixv_structure_dump not built")
class DumpToolTest(unittest.TestCase):
    def test_dump_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            files = []
            for i, insts in enumerate((module_insts(),
                                       module_insts(5, True))):
                f = Path(d) / f"m{i}.spv"
                f.write_bytes(spirv(insts, 32))
                files.append(str(f))
            out = Path(d) / "dump.bin"
            subprocess.run([str(structure_dump()), str(out)] + files,
                           check=True, stderr=subprocess.DEVNULL)
            mods = mms.read_dump(out)
            mms.prepare(mods)
            for f in files:
                m = mods[f]
                self.assertEqual(m.status, 0)
                self.assertGreater(m.payload, 0)
                # Entropy trace is close to (and below) the flushed payload.
                self.assertLessEqual(m.trace_bits / 8, m.payload + 1)
                self.assertGreater(m.trace_bits / 8, 0.5 * m.payload - 8)
                self.assertEqual(m.bound, 32)
                vec = [i for i, w in enumerate(m.words)
                       if w[0] & 0xFFFF == 23][0]
                self.assertEqual(list(m.mask[vec]), [0, 1, 2, 0])
                const = [i for i, w in enumerate(m.words)
                         if w[0] & 0xFFFF == 43][0]
                self.assertEqual(list(m.mask[const]), [0, 2, 1, 0])


if __name__ == "__main__":
    unittest.main()
