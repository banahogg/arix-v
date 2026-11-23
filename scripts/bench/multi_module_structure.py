#!/usr/bin/env python3
# Copyright (c) 2026, Eric Werness
# SPDX-License-Identifier: BSD-2-Clause
"""RESEARCH ONLY (Track B gate 2): bounded repeated-structure opportunity probe.

Python stdlib only. Consumes gate 1a's frozen manifest (validated through
multi_module_probe.load_validated), a structural dump from
build/research/arixv_structure_dump (per-instruction words, grammar ID-role
masks, and the unchanged single-module model's in-context entropy per
instruction), gate 1a's pack baselines, and gate 1b's packs.csv.

For each pack in manifest order, every module is parsed against *prior
modules only* into literal instructions and copies of instruction runs. Every
signaling decision is charged with an explicit, decoder-reproducible cost
model (adaptive binary flags, Elias-gamma lengths, uniform source positions,
per-ID-word restoration choices, uniform explicit IDs). Literal instructions
cost what the unchanged ARX2 model spent on them in context. Each parse is
reconstructed by an independent decoder simulation and byte-compared with the
module; a mismatch aborts the run.

These are ENCODER-SIDE ESTIMATES, not realized compressed sizes: they assume a
decoder can pass copied instructions through the ARX2 model/tracker state
updates without coding them, so literal instructions keep their standalone
in-context cost. See docs/development/multi-module-gate2.md.

Subcommands:
  run      dump (unless --dump given), parse every pack, write results
  analyze  tables by source/tier, mechanism attribution, coverage, curves
"""

import argparse
import collections
import concurrent.futures
import csv
import hashlib
import json
import math
import multiprocessing
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import multi_module_probe as mmp  # noqa: E402

DUMP_MAGIC = b"AXSD"
G = 3                 # hashed n-gram length and minimum new-copy length
DEPTH_BOUNDED = 8     # predeclared candidate budget (hash-chain depth)
DEPTH_DEEP = 256      # search-sensitivity diagnostic (not an oracle)
MODES = (
    # name, abstract IDs, chain depth
    ("exact", False, DEPTH_BOUNDED),
    ("idabs", True, DEPTH_BOUNDED),
    ("idabs_deep", True, DEPTH_DEEP),
)

DEBUG_OPS = {2, 3, 4, 5, 6, 7, 8, 317, 330}
ANNOT_OPS = {71, 72, 73, 74, 75, 332, 5632, 5633}
PREAMBLE_OPS = {10, 11, 14, 15, 16, 17, 331}
SECTIONS = ("preamble", "debug", "annotation", "global", "function")


# ---------------------------------------------------------------- dump input

class Mod:
    __slots__ = ("path", "status", "swapped", "size", "payload", "header_bits",
                 "words", "mask", "bits", "n", "bound", "section", "idpos",
                 "tok_exact", "tok_abs", "trace_bits")


def read_dump(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != DUMP_MAGIC:
        raise SystemExit("not an AXSD dump")
    version, count = struct.unpack_from("<II", data, 4)
    if version != 1:
        raise SystemExit(f"dump version {version} unsupported")
    off = 12
    mods = {}
    for _ in range(count):
        (plen,) = struct.unpack_from("<I", data, off)
        off += 4
        m = Mod()
        m.path = data[off:off + plen].decode()
        off += plen
        m.status, m.swapped, m.size, m.payload = struct.unpack_from(
            "<BBII", data, off)
        off += 10
        m.header_bits, _tail, n = struct.unpack_from("<ddI", data, off)
        off += 20
        m.words, m.mask, m.bits = [], [], []
        for _ in range(n):
            (nw,) = struct.unpack_from("<I", data, off)
            off += 4
            m.words.append(struct.unpack_from(f"<{nw}I", data, off))
            off += 4 * nw
            m.mask.append(data[off:off + nw])
            off += nw
            m.bits.append(struct.unpack_from("<d", data, off)[0])
            off += 8
        m.n = n
        mods[m.path] = m
    return mods


def prepare(mods):
    """Interned tokens, ID positions, and sections (deterministic)."""
    ex, ab = {}, {}
    for path in sorted(mods):
        m = mods[path]
        m.trace_bits = m.header_bits + sum(m.bits)
        m.bound = 0
        m.section, m.idpos, m.tok_exact, m.tok_abs = [], [], [], []
        in_func = False
        for w, mk in zip(m.words, m.mask):
            op = w[0] & 0xFFFF
            if op == 54:
                in_func = True
            if op in PREAMBLE_OPS:
                sec = "preamble"
            elif op in DEBUG_OPS:
                sec = "debug"
            elif op in ANNOT_OPS:
                sec = "annotation"
            elif not in_func:
                sec = "global"
            else:
                sec = "function"
            m.section.append(sec)
            ids = tuple((i, mk[i]) for i in range(len(w)) if mk[i])
            m.idpos.append(ids)
            m.tok_exact.append(ex.setdefault(w, len(ex)))
            if ids:
                aw = list(w)
                for i, role in ids:
                    aw[i] = -role
                key = tuple(aw)
            else:
                key = w
            m.tok_abs.append(ab.setdefault(key, len(ab)))
        if m.status == 0 and m.n:
            with open(m.path, "rb") as f:
                head = f.read(16)
            b = struct.unpack("<4I", head)
            bound = b[3]
            if m.swapped:
                bound = int.from_bytes(bound.to_bytes(4, "little"), "big")
            m.bound = max(bound, 2)


# ------------------------------------------------------------- cost models

class Bin:
    """Adaptive binary model (KT-style counts); cost in bits."""
    __slots__ = ("n0", "n1")

    def __init__(self):
        self.n0 = 0
        self.n1 = 0

    def cost(self, bit):
        c = self.n1 if bit else self.n0
        return -math.log2((c + 0.4) / (self.n0 + self.n1 + 0.8))

    def code(self, bit):
        c = self.cost(bit)
        if bit:
            self.n1 += 1
        else:
            self.n0 += 1
        return c


def gamma(n):
    """Elias-gamma length in bits for n >= 1."""
    return 2 * (n.bit_length() - 1) + 1


class PackModels:
    """Adaptive state persisting across one pack (decoder-reproducible)."""

    def __init__(self):
        self.dup = Bin()
        self.start = [Bin(), Bin()]     # ctx: previous instruction copied
        self.rep = Bin()
        # ID restoration: ctx role (1 def, 2 use)
        self.map_hit = {1: Bin(), 2: Bin()}
        self.delta_hit = {(r, h): Bin() for r in (1, 2) for h in (0, 1)}


def id_cost(models, role, b, t, mp, delta, bound, commit):
    """Cost of restoring target ID t from base ID b; returns (bits, choice)."""
    bits = 0.0
    p1 = mp.get(b)
    has = p1 is not None
    if has:
        hit = p1 == t
        bits += models.map_hit[role].code(hit) if commit else \
            models.map_hit[role].cost(hit)
        if hit:
            return bits, 0
    hit2 = b + delta == t
    m2 = models.delta_hit[(role, 1 if has else 0)]
    bits += m2.code(hit2) if commit else m2.cost(hit2)
    if hit2:
        return bits, 1
    return bits + math.log2(bound), 2


# --------------------------------------------------------------- the parser

class Parser:
    def __init__(self, mods_in_pack, abstract, depth):
        self.mods = mods_in_pack
        self.abstract = abstract
        self.depth = depth
        self.index = collections.defaultdict(list)
        self.models = PackModels()

    def tok(self, m):
        return m.tok_abs if self.abstract else m.tok_exact

    def add_to_index(self, k):
        t = self.tok(self.mods[k])
        for p in range(len(t) - G + 1):
            self.index[(t[p], t[p + 1], t[p + 2])].append((k, p))

    def evaluate(self, T, pos, k, m, p, is_rep, maps, deltas, flag1, flag0):
        B = self.mods[m]
        tt, bt = self.tok(T), self.tok(B)
        limit = min(T.n - pos, B.n - p)
        mp = maps.get(m, {})
        delta = deltas.get(m, 0)
        overlay = {}
        sig = flag1
        if is_rep:
            sig += self.models.rep.cost(True)
            min_len = 1
        else:
            if k > 0:
                sig += self.models.rep.cost(False) if self.rep_possible else 0
            sig += gamma(k - m) + math.log2(max(B.n, 1))
            min_len = G
        benefit = 0.0
        idb = 0.0
        best = (0.0, 0)
        for j in range(limit):
            if tt[pos + j] != bt[p + j]:
                break
            benefit += T.bits[pos + j] + flag0
            if self.abstract:
                bw = B.words[p + j]
                tw = T.words[pos + j]
                for wi, role in B.idpos[p + j]:
                    b, t = bw[wi], tw[wi]
                    cur = overlay.get(b)
                    if cur is None:
                        cur = mp.get(b)
                    pred_map = cur
                    if pred_map is not None and pred_map == t:
                        idb += self.models.map_hit[role].cost(True)
                    else:
                        if pred_map is not None:
                            idb += self.models.map_hit[role].cost(False)
                        hit2 = b + delta == t
                        idb += self.models.delta_hit[
                            (role, 1 if pred_map is not None else 0)].cost(hit2)
                        if not hit2:
                            idb += math.log2(T.bound)
                    overlay[b] = t
                    delta = t - b
            L = j + 1
            if L >= min_len:
                lbits = gamma(L) if is_rep else gamma(L - G + 1)
                net = benefit - idb - sig - lbits
                if net > best[0]:
                    best = (net, L)
        return best

    def parse_module(self, k, verify=True):
        """Greedy cost-based parse of module k; returns stats dict."""
        T = self.mods[k]
        md = self.models
        st = collections.Counter()
        cov = collections.Counter()
        maps, deltas = {}, {}
        ops = []
        pos = 0
        prev_copy = 0
        rep = None
        self.rep_possible = False
        while pos < T.n:
            flag1 = md.start[prev_copy].cost(True)
            flag0 = md.start[prev_copy].cost(False)
            cands = []
            if rep is not None and rep[1] < self.mods[rep[0]].n:
                cands.append((rep[0], rep[1], True))
                self.rep_possible = True
            else:
                self.rep_possible = False
            if pos + G <= T.n and k > 0:
                t = self.tok(T)
                lst = self.index.get((t[pos], t[pos + 1], t[pos + 2]))
                if lst:
                    for c in lst[-1:-self.depth - 1:-1]:
                        if rep is None or c != (rep[0], rep[1]):
                            cands.append((c[0], c[1], False))
            best = None
            for m, p, is_rep in cands:
                net, L = self.evaluate(T, pos, k, m, p, is_rep, maps, deltas,
                                       flag1, flag0)
                if L and (best is None or net > best[0]):
                    best = (net, L, m, p, is_rep)
            if best is not None and best[0] > 0:
                _, L, m, p, is_rep = best
                B = self.mods[m]
                st["flag_bits"] += md.start[prev_copy].code(True)
                if self.rep_possible:
                    st["src_bits"] += md.rep.code(is_rep)
                if not is_rep:
                    st["src_bits"] += gamma(k - m) + math.log2(max(B.n, 1))
                st["len_bits"] += gamma(L) if is_rep else gamma(L - G + 1)
                mp = maps.setdefault(m, {})
                choices = []
                for j in range(L):
                    tw, bw = T.words[pos + j], B.words[p + j]
                    # Exact mode: tokens include ID values; nothing to restore.
                    for wi, role in (B.idpos[p + j] if self.abstract else ()):
                        b, t = bw[wi], tw[wi]
                        bits, ch = id_cost(md, role, b, t, mp,
                                           deltas.get(m, 0), T.bound, True)
                        st["id_bits"] += bits
                        st[("id_choice", ch)] += 1
                        choices.append(t if ch == 2 else -1 - ch)
                        mp[b] = t
                        deltas[m] = t - b
                    sec = T.section[pos + j]
                    cov[("insts", sec)] += 1
                    cov[("words", sec)] += len(tw)
                    cov[("bits", sec)] += T.bits[pos + j]
                ops.append(("copy", m, p, L, choices, pos))
                st["copies"] += 1
                st["rep_copies"] += 1 if is_rep else 0
                pos += L
                rep = (m, p + L)
                prev_copy = 1
            else:
                st["flag_bits"] += md.start[prev_copy].code(False)
                st["literal_bits"] += T.bits[pos]
                ops.append(("lit", T.words[pos]))
                pos += 1
                prev_copy = 0
        if verify:
            check_reconstruction(self.mods, T, ops, self.abstract)
        self.add_to_index(k)
        self.last_ops = ops
        return st, cov


def check_reconstruction(mods, T, ops, abstract):
    """Independent decoder simulation: rebuild T's body from the parse."""
    out = []
    maps, deltas = {}, {}
    for op in ops:
        if op[0] == "lit":
            out.append(op[1])
            continue
        _, m, p, L, choices, _at = op
        B = mods[m]
        mp = maps.setdefault(m, {})
        ci = 0
        for j in range(L):
            w = list(B.words[p + j])
            for wi, _role in (B.idpos[p + j] if abstract else ()):
                b = w[wi]
                c = choices[ci]
                ci += 1
                if c == -1:
                    t = mp[b]
                elif c == -2:
                    t = b + deltas.get(m, 0)
                else:
                    t = c
                mp[b] = t
                deltas[m] = t - b
                w[wi] = t
            out.append(tuple(w))
        if ci != len(choices):
            raise SystemExit(f"reconstruction choice count mismatch: {T.path}")
    if out != T.words:
        raise SystemExit(f"RECONSTRUCTION FAILURE: {T.path}")


# ------------------------------------------------------------ pack analysis

def analyze_pack(pack, members, mods, blob_bytes):
    """All modes for one pack; returns per-mode totals and per-module rows."""
    plist = [mods[members[mid]["path"]] for mid in pack["module_ids"]]
    shas = [members[mid]["sha256"] for mid in pack["module_ids"]]
    out = {"pack_id": pack["id"], "modes": {}, "modules": []}
    # Exact duplicates within the pack (first occurrence is the base).
    first = {}
    dup_of = []
    for k, s in enumerate(shas):
        dup_of.append(first.get(s))
        first.setdefault(s, k)
    for mode, abstract, depth in MODES:
        # Unique (non-duplicate, structurally parsed) modules form the
        # reference sequence; duplicates are charged as references.
        uniq = [k for k in range(len(plist)) if dup_of[k] is None]
        seq = [plist[k] for k in uniq]
        parser = Parser(seq, abstract, depth)
        dupm = Bin()
        total_saved_bits = 0.0
        dup_ref_bits = 0.0
        dup_blob_bytes = 0
        agg = collections.Counter()
        covagg = collections.Counter()
        rows = []
        ui = 0
        for k, T in enumerate(plist):
            blob = blob_bytes[shas[k]]
            if dup_of[k] is not None:
                c = dupm.code(True) + math.log2(max(ui, 1))
                dup_ref_bits += c
                dup_blob_bytes += blob
                rows.append({"pack_id": pack["id"], "mode": mode,
                             "position": k + 1, "module_id":
                             pack["module_ids"][k], "dup": True,
                             "standalone_bytes": blob,
                             "est_bytes": c / 8, "saved_bytes": blob - c / 8})
                continue
            dup_flag = dupm.code(False)
            if T.status != 0 or T.n == 0:
                parser.add_to_index(ui)  # zero instructions: no entries
                ui += 1
                rows.append({"pack_id": pack["id"], "mode": mode,
                             "position": k + 1, "module_id":
                             pack["module_ids"][k], "dup": False,
                             "standalone_bytes": blob,
                             "est_bytes": blob + dup_flag / 8,
                             "saved_bytes": -dup_flag / 8,
                             "structural": False})
                total_saved_bits -= dup_flag
                continue
            st, cov = parser.parse_module(ui)
            ui += 1
            est_bits = (T.header_bits + st["literal_bits"] + st["flag_bits"]
                        + st["src_bits"] + st["len_bits"] + st["id_bits"]
                        + dup_flag)
            saved = T.trace_bits - est_bits
            total_saved_bits += saved
            agg.update(st)
            covagg.update(cov)
            covered_bits = sum(v for (kind, _), v in cov.items()
                               if kind == "bits")
            rows.append({"pack_id": pack["id"], "mode": mode,
                         "position": k + 1, "module_id": pack["module_ids"][k],
                         "dup": False, "structural": True,
                         "standalone_bytes": blob,
                         "est_bytes": blob - saved / 8,
                         "saved_bytes": saved / 8,
                         "instructions": T.n,
                         "covered_insts": sum(v for (kd, _), v in cov.items()
                                              if kd == "insts"),
                         "covered_model_bytes": covered_bits / 8,
                         "module_model_bytes": T.trace_bits / 8,
                         "copies": st["copies"],
                         "signal_bytes": (st["flag_bits"] + st["src_bits"]
                                          + st["len_bits"] + dup_flag) / 8,
                         "id_bytes": st["id_bits"] / 8})
        out["modes"][mode] = {
            "saved_struct_bytes": total_saved_bits / 8,
            "dup_blob_bytes": dup_blob_bytes,
            "dup_ref_bytes": dup_ref_bits / 8,
            "copies": agg["copies"], "rep_copies": agg["rep_copies"],
            "literal_model_bytes": agg["literal_bits"] / 8,
            "flag_bytes": agg["flag_bits"] / 8,
            "src_bytes": agg["src_bits"] / 8,
            "len_bytes": agg["len_bits"] / 8,
            "id_bytes": agg["id_bits"] / 8,
            "id_choices": [agg[("id_choice", c)] for c in range(3)],
            "covered": {f"{kd}:{sec}": v for (kd, sec), v in
                        sorted(covagg.items())},
        }
        out["modules"].extend(rows)
    # Inventory: uncharged exact-instruction coverage (diagnostic only).
    out["inventory"] = inventory(plist, dup_of)
    return out


def inventory(plist, dup_of):
    """Uncharged coverage diagnostics: fraction of non-duplicate modules'
    instructions/model bits whose exact (or ID-abstracted) instruction token
    occurs anywhere in prior non-duplicate modules, and exact string reuse.
    Coverage is not a compressed size."""
    seen_ex, seen_ab, seen_str = set(), set(), set()
    c = collections.Counter()
    for k, T in enumerate(plist):
        if dup_of[k] is not None or T.status != 0 or T.n == 0:
            continue
        for i in range(T.n):
            sec = T.section[i]
            b = T.bits[i]
            c[("bits", sec)] += b
            if T.tok_exact[i] in seen_ex:
                c[("exact_bits", sec)] += b
            if T.tok_abs[i] in seen_ab:
                c[("abs_bits", sec)] += b
        for i in range(T.n):
            seen_ex.add(T.tok_exact[i])
            seen_ab.add(T.tok_abs[i])
    return {f"{a}:{s}": v / 8 for (a, s), v in sorted(c.items())}


# ----------------------------------------------------------------- driver

_G = {}


def _worker(pack):
    return analyze_pack(pack, _G["members"], _G["mods"], _G["blob"])


def load_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def cmd_run(args):
    manifest, ready, digest, counts, snap, ready_path = mmp.load_validated(args)
    os.makedirs(args.out, exist_ok=True)
    members = manifest["modules"]
    dump = args.dump
    if dump is None:
        dump = os.path.join(args.out, "structure_dump.bin")
        paths = sorted({v["path"] for v in members.values()})
        lst = os.path.join(args.out, "paths.txt")
        with open(lst, "w") as f:
            f.write("\n".join(paths) + "\n")
        with open(os.path.join(args.out, "structure_dump.log"), "w") as log:
            subprocess.run([args.structure_dump, dump, "@" + lst],
                           check=True, stderr=log)
    mods = read_dump(dump)
    for v in members.values():
        if v["path"] not in mods:
            raise SystemExit(f"dump lacks {v['path']}")
        m = mods[v["path"]]
        if m.size != v["byte_size"]:
            raise SystemExit(f"size mismatch {v['path']}")
    prepare(mods)
    per_module = {r["sha256"]: r for r in load_csv(args.gate1a_per_module)}
    blob = {s: int(r["arx2_bytes"]) for s, r in per_module.items()}
    for v in members.values():
        m = mods[v["path"]]
        r = per_module[v["sha256"]]
        if m.status == 0 and m.n and \
                int(r["arx2_bytes"]) - int(r["arx2_header_bytes"]) != m.payload:
            raise SystemExit(f"payload differs from gate 1a blob: {v['path']}")
    _G.update(members=members, mods=mods, blob=blob)
    packs = manifest["packs"]
    # fork: workers inherit the parsed dump (_G) read-only.
    with concurrent.futures.ProcessPoolExecutor(
            args.jobs, mp_context=multiprocessing.get_context("fork")) as ex:
        results = list(ex.map(_worker, packs, chunksize=1))
    base = collections.defaultdict(dict)
    for r in load_csv(args.gate1a_baselines):
        base[r["pack_id"]][r["method"]] = r
    g1b = {r["pack_id"]: r for r in load_csv(args.gate1b_packs)}
    pack_rows = []
    for pack, res in zip(packs, results):
        b = base[pack["id"]]
        framed = int(b["arx2_framed"]["total_bytes"])
        row = {
            "pack_id": pack["id"], "tier": pack["tier"],
            "representative": pack["representative"],
            "origin_id": pack.get("origin_id"),
            "collection_id": pack.get("collection_id"),
            "members": len(pack["module_ids"]),
            "original_bytes": int(b["arx2_framed"]["original_bytes"]),
            "arx2_bare_sum": int(b["arx2_bare_sum"]["total_bytes"]),
            "arx2_framed": framed,
            "arx2_shared_header": int(b["arx2_shared_header"]["total_bytes"]),
            "arx2_dedup": int(b["arx2_dedup"]["total_bytes"]),
            "dup_members": sum(1 for x in res["modules"]
                               if x["mode"] == "exact" and x["dup"]),
        }
        g = g1b.get(pack["id"])
        row["g1b_cont_oce_carry"] = (int(g["cont:oce:carry"])
                                     if g and g.get("status") == "ok" else None)
        gen = {}
        for meth in ("orig_solid_zstd19", "orig_solid_zstd22w27",
                     "orig_solid_xz9e", "smolv_solid_zstd19",
                     "smolv_solid_zstd22w27", "smolv_solid_xz9e"):
            r = b.get(meth)
            if r and r["verified"] == "True" and r["total_bytes"]:
                gen[meth] = int(r["total_bytes"])
        row["best_generic"] = min(gen.values()) if gen else None
        row["best_generic_method"] = min(gen, key=gen.get) if gen else None
        row["orig_solid_xz9e"] = gen.get("orig_solid_xz9e")
        row["smolv_solid_xz9e"] = gen.get("smolv_solid_xz9e")
        dmode = res["modes"]["exact"]
        row["est_dedup_only"] = math.ceil(
            framed - dmode["dup_blob_bytes"] + dmode["dup_ref_bytes"])
        for mode, _, _ in MODES:
            md = res["modes"][mode]
            row[f"est_{mode}"] = math.ceil(
                framed - md["dup_blob_bytes"] + md["dup_ref_bytes"]
                - md["saved_struct_bytes"])
            for key in ("copies", "id_bytes", "flag_bytes", "src_bytes",
                        "len_bytes"):
                row[f"{mode}_{key}"] = round(md[key], 1)
        pack_rows.append(row)
    doc = {
        "tool": "multi_module_structure.py run",
        "manifest_sha256": digest, "ready": ready,
        "snapshot_check": snap,
        "parameters": {"G": G, "depth_bounded": DEPTH_BOUNDED,
                       "depth_deep": DEPTH_DEEP,
                       "modes": [m[0] for m in MODES]},
        "dump_sha256": mmp.sha256_file(dump),
        "structure_dump_sha256": (mmp.sha256_file(args.structure_dump)
                                  if args.structure_dump else None),
        "packs": results,
    }
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(doc, f)
    with open(os.path.join(args.out, "packs.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(pack_rows[0]))
        w.writeheader()
        w.writerows(pack_rows)
    mod_rows = [x for r in results for x in r["modules"]]
    keys = sorted({k for x in mod_rows for k in x})
    with open(os.path.join(args.out, "modules.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(mod_rows)
    print(f"{len(packs)} packs; reconstruction verified for every parse")



# ----------------------------------------------------------------- analysis

SOURCE_GROUPS = (
    ("Dota 2", lambda r: r["pack_id"] == "coll:smolv/dota2"),
    ("Dota 2 (#unique)", lambda r: r["pack_id"] == "coll:smolv/dota2#unique"),
    ("Blender", lambda r: r["pack_id"] == "coll:smolv/blender"),
    ("Shadertoy", lambda r: r["pack_id"] == "coll:smolv/shadertoy"),
    ("Naga remaps", lambda r: r["pack_id"] == "coll:spvdb/naga/remaps"),
    ("Naga remaps (#unique)",
     lambda r: r["pack_id"] == "coll:spvdb/naga/remaps#unique"),
    ("GL CTS (zink/angle)", lambda r: "gl_cts" in r["pack_id"]),
    ("Vulkan-Samples collections (rep)",
     lambda r: r["tier"] == "same_collection" and r["representative"]
     and r["pack_id"].startswith("coll:vks/")),
    ("Stage-pair packs", lambda r: r["tier"] == "stage_pair"),
    ("Variant-family packs", lambda r: r["tier"] == "variant_family"),
    ("Control: unrelated mixes",
     lambda r: r["pack_id"].startswith("control:unrelated")),
    ("Control: cross-frontend",
     lambda r: r["pack_id"].startswith("control:cross-frontend")),
    ("Control: singletons",
     lambda r: r["pack_id"].startswith("control:singleton")),
)
TIER_GROUPS = (
    ("rep same_collection", lambda r: r["representative"]
     and r["tier"] == "same_collection"),
    ("  no exact duplicates", lambda r: r["representative"]
     and r["tier"] == "same_collection" and r["dup_members"] == 0),
    ("  with exact duplicates", lambda r: r["representative"]
     and r["tier"] == "same_collection" and r["dup_members"] > 0),
    ("rep stage_pair", lambda r: r["representative"]
     and r["tier"] == "stage_pair"),
    ("rep variant_family", lambda r: r["representative"]
     and r["tier"] == "variant_family"),
    ("diag control", lambda r: not r["representative"]
     and r["tier"] == "control"),
    ("diag same_collection", lambda r: not r["representative"]
     and r["tier"] == "same_collection"),
)
RATIO_COLS = (
    ("arx2_bare_sum", "ARX2"), ("arx2_dedup", "ARX2+dedup"),
    ("g1b_cont_oce_carry", "warm start"), ("est_exact", "exact copy*"),
    ("est_idabs", "ID-abs copy*"), ("est_idabs_deep", "ID-abs deep*"),
    ("best_generic", "best generic"),
)
POS_BUCKETS = ((1, 1), (2, 2), (3, 4), (5, 8), (9, 16), (17, 32), (33, 10**9))


def _num(v):
    if v in ("", "None", None):
        return None
    return int(v) if str(v).lstrip("-").isdigit() else float(v)


def _typed(rows):
    out = []
    for r in rows:
        t = dict(r)
        t["representative"] = r["representative"] == "True"
        for k, v in r.items():
            if k.startswith(("est_", "arx2_", "g1b_", "best_generic",
                             "orig_", "smolv_")) and k != "best_generic_method":
                t[k] = _num(v)
        for k in ("members", "original_bytes", "dup_members"):
            t[k] = int(r[k])
        out.append(t)
    return out


def _pct(a, b):
    return 100.0 * (a - b) / b


def group_table(rows, groups):
    lines = []
    head = ("| Group | Packs | Original B | " +
            " | ".join(c[1] for c in RATIO_COLS) +
            " | ID-abs vs ARX2 | ID-abs vs best generic | ID-abs < generic"
            " | ID-abs < warm |")
    lines.append(head)
    lines.append("|" + "---|" * (head.count("|") - 1))
    js = {}
    for name, pred in groups:
        sel = [r for r in rows if pred(r)]
        ok = [r for r in sel if all(r[c] is not None for c, _ in RATIO_COLS)]
        if not ok:
            continue
        o = sum(r["original_bytes"] for r in ok)
        tot = {c: sum(r[c] for r in ok) for c, _ in RATIO_COLS}
        win_g = sum(1 for r in ok if r["est_idabs"] < r["best_generic"])
        win_w = sum(1 for r in ok if r["est_idabs"] < r["g1b_cont_oce_carry"])
        npk = f"{len(ok)}" + (f" (of {len(sel)})" if len(ok) != len(sel)
                              else "")
        lines.append(
            f"| {name} | {npk} | {o:,} | " +
            " | ".join(f"{tot[c] / o:.3f}" for c, _ in RATIO_COLS) +
            f" | {_pct(tot['est_idabs'], tot['arx2_bare_sum']):+.1f}%"
            f" | {_pct(tot['est_idabs'], tot['best_generic']):+.1f}%"
            f" | {win_g}/{len(ok)} | {win_w}/{len(ok)} |")
        js[name] = {"packs": len(ok), "skipped": len(sel) - len(ok),
                    "original_bytes": o, "totals": tot,
                    "idabs_lt_generic": win_g, "idabs_lt_warm": win_w}
    return lines, js


def attribution_table(rows, P, groups):
    """Mechanism split for est_idabs, in bytes, per group."""
    lines = ["| Group | ARX2 | Exact dedup | Exact-copy net | ID-abs increment"
             " | Gross covered | of which debug | annotation | global decl"
             " | function | Signaling | ID restoration | ID choices map/delta"
             "/explicit | Bounded vs deep |",
             "|" + "---|" * 15]
    js = {}
    for name, pred in groups:
        sel = [r for r in rows if pred(r) and r["arx2_bare_sum"] is not None]
        if not sel:
            continue
        a = sum(r["arx2_bare_sum"] for r in sel)
        dedup = sum(r["arx2_framed"] - r["est_dedup_only"] for r in sel)
        exact = sum(r["est_dedup_only"] - r["est_exact"] for r in sel)
        inc = sum(r["est_exact"] - r["est_idabs"] for r in sel)
        deep = sum(r["est_idabs"] - r["est_idabs_deep"] for r in sel)
        cov = collections.Counter()
        sig = idb = 0.0
        ch = [0, 0, 0]
        for r in sel:
            md = P[r["pack_id"]]["modes"]["idabs"]
            for k, v in md["covered"].items():
                kind, sec = k.split(":")
                if kind == "bits":
                    cov[sec] += v / 8
            sig += md["flag_bytes"] + md["src_bytes"] + md["len_bytes"]
            idb += md["id_bytes"]
            ch = [x + y for x, y in zip(ch, md["id_choices"])]
        gross = sum(cov.values())
        lines.append(
            f"| {name} | {a:,} | {dedup:,} | {exact:,} | {inc:,} | "
            f"{gross:,.0f} | {cov['debug']:,.0f} | {cov['annotation']:,.0f} | "
            f"{cov['global'] + cov['preamble']:,.0f} | {cov['function']:,.0f} | "
            f"{sig:,.0f} | {idb:,.0f} | {ch[0]:,}/{ch[1]:,}/{ch[2]:,} | "
            f"{deep:+,} |")
        js[name] = {"arx2": a, "dedup": dedup, "exact_net": exact,
                    "idabs_increment": inc, "gross_covered": gross,
                    "covered_by_section": dict(cov), "signaling": sig,
                    "id_restoration": idb, "id_choices": ch,
                    "deep_minus_bounded": -deep}
    return lines, js


def inventory_table(rows, P, groups):
    lines = ["| Group | Non-dup model B | Exact-token seen | ID-abs-token seen"
             " | Charged ID-abs covered | Debug model B | Debug ID-abs seen |",
             "|" + "---|" * 7]
    for name, pred in groups:
        sel = [r for r in rows if pred(r)]
        if not sel:
            continue
        inv = collections.Counter()
        cov = 0.0
        for r in sel:
            for k, v in P[r["pack_id"]]["inventory"].items():
                inv[k] += v
            for k, v in P[r["pack_id"]]["modes"]["idabs"]["covered"].items():
                if k.startswith("bits:"):
                    cov += v / 8
        tot = sum(v for k, v in inv.items() if k.startswith("bits:"))
        ex = sum(v for k, v in inv.items() if k.startswith("exact_bits:"))
        ab = sum(v for k, v in inv.items() if k.startswith("abs_bits:"))
        if not tot:
            continue
        dbg = inv["bits:debug"]
        lines.append(
            f"| {name} | {tot:,.0f} | {100 * ex / tot:.1f}% | "
            f"{100 * ab / tot:.1f}% | {100 * cov / tot:.1f}% | {dbg:,.0f} | "
            f"{(100 * inv['abs_bits:debug'] / dbg) if dbg else 0:.1f}% |")
    return lines


def curve_table(mods_rows, rows_by_id, groups, mode="idabs"):
    head = "| Group | " + " | ".join(
        f"{a}" if a == b else (f"{a}+" if b > 10**6 else f"{a}-{b}")
        for a, b in POS_BUCKETS) + " |"
    lines = [head, "|" + "---|" * (len(POS_BUCKETS) + 1)]
    js = {}
    for name, pred in groups:
        acc = collections.defaultdict(lambda: [0.0, 0.0, 0])
        for x in mods_rows:
            if x["mode"] != mode or x["dup"] == "True":
                continue
            r = rows_by_id[x["pack_id"]]
            if not pred(r):
                continue
            k = int(x["position"])
            for bi, (a, b) in enumerate(POS_BUCKETS):
                if a <= k <= b:
                    acc[bi][0] += float(x["est_bytes"])
                    acc[bi][1] += float(x["standalone_bytes"])
                    acc[bi][2] += 1
        if not acc:
            continue
        cells = []
        for bi in range(len(POS_BUCKETS)):
            e, s, n = acc[bi]
            cells.append(f"{e / s:.3f} (n={n})" if n else "—")
        lines.append(f"| {name} | " + " | ".join(cells) + " |")
        js[name] = {str(POS_BUCKETS[bi]): acc[bi] for bi in acc}
    return lines, js


def cmd_analyze(args):
    with open(os.path.join(args.results, "packs.csv"), newline="") as f:
        rows = _typed(list(csv.DictReader(f)))
    with open(os.path.join(args.results, "results.json")) as f:
        doc = json.load(f)
    P = {p["pack_id"]: p for p in doc["packs"]}
    with open(os.path.join(args.results, "modules.csv"), newline="") as f:
        mrows = list(csv.DictReader(f))
    by_id = {r["pack_id"]: r for r in rows}
    out = ["# Gate 2 structural opportunity analysis (generated)", "",
           f"Manifest SHA-256 `{doc['manifest_sha256']}`; dump SHA-256 "
           f"`{doc['dump_sha256']}`; parameters {doc['parameters']}.", "",
           "Columns marked * are encoder-side estimates (see report); all "
           "others are verified byte counts from gates 1a/1b.", "",
           "## By source (byte-weighted ratio, lower is better)", ""]
    t, js_src = group_table(rows, SOURCE_GROUPS)
    out += t + ["", "## By manifest tier (gate 1b grouping)", ""]
    t, js_tier = group_table(rows, TIER_GROUPS)
    out += t + ["", "## Mechanism attribution for ID-abs copy (bytes)", ""]
    t, js_attr = attribution_table(rows, P, SOURCE_GROUPS + TIER_GROUPS)
    out += t + ["", "## Uncharged coverage inventory (diagnostic)", ""]
    out += inventory_table(rows, P, SOURCE_GROUPS + TIER_GROUPS)
    out += ["", "## Incremental cost: est bytes / standalone ARX2 by position "
            "(non-duplicate members, ID-abs copy)", ""]
    t, js_curve = curve_table(mrows, by_id, SOURCE_GROUPS + TIER_GROUPS)
    out += t + [""]
    with open(os.path.join(args.results, "analysis.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    with open(os.path.join(args.results, "analysis.json"), "w") as f:
        json.dump({"by_source": js_src, "by_tier": js_tier,
                   "attribution": js_attr, "curves": js_curve}, f, indent=1)
    print("\n".join(out))


# ------------------------------------------------------------ gate 3 driver

GATE3_CONFIGS = (
    # name, probe flags, use plan
    ("dedup", "signal", False),
    ("dedup_warm", "signal,warm", False),
    ("copy", "signal", True),
    ("copy_warm", "signal,warm", True),
    # Gate 4 decode-cost probe: copied instructions advance tracker state
    # only (no probability training).
    ("copy_warm_noadapt", "signal,warm,noadapt", True),
    # Gate 4 native planner (arixv::research::PlanStructPack) in the encoder.
    ("copy_warm_native", "signal,warm", "native"),
)


def plan_pack(pack, members, mods, abstract=True, depth=DEPTH_BOUNDED):
    """Encoder-side copy plan (member-indexed) from the gate-2 parser."""
    plist = [mods[members[mid]["path"]] for mid in pack["module_ids"]]
    shas = [members[mid]["sha256"] for mid in pack["module_ids"]]
    first, uniq = {}, []
    for k, s in enumerate(shas):
        if s not in first:
            first[s] = k
            uniq.append(k)
    seq = [plist[k] for k in uniq]
    parser = Parser(seq, abstract, depth)
    lines = []
    for ui, k in enumerate(uniq):
        T = seq[ui]
        if T.status != 0 or T.n == 0 or T.swapped:
            parser.add_to_index(ui)
            continue
        parser.parse_module(ui)
        for op in parser.last_ops:
            if op[0] == "copy":
                lines.append(f"{k} {op[5]} {uniq[op[1]]} {op[2]} {op[3]}")
    return lines


def _gate3_worker(task):
    pack, out_dir, probe, time_runs, configs = task
    members, mods = _G["members"], _G["mods"]
    safe = hashlib.sha256(pack["id"].encode()).hexdigest()[:16]
    lst = os.path.join(out_dir, "packs", safe + ".list")
    plan = os.path.join(out_dir, "packs", safe + ".plan")
    with open(lst, "w") as f:
        f.write("\n".join(members[m]["path"] for m in pack["module_ids"]) +
                "\n")
    lines = plan_pack(pack, members, mods)
    with open(plan, "w") as f:
        f.write(f"# {pack['id']}\n" + "\n".join(lines) + "\n")
    res = {"pack_id": pack["id"], "planned_copies": len(lines)}
    for name, flags, use_plan in configs:
        argv = [probe, "pack", "--id", pack["id"], "--flags", flags]
        if use_plan == "native":
            argv += ["--native"]
        elif use_plan:
            argv += ["--plan", plan]
        if time_runs:
            argv += ["--time", str(time_runs)]
        argv += ["--", "@" + lst]
        p = subprocess.run(argv, capture_output=True, text=True)
        try:
            j = json.loads(p.stdout)
        except ValueError:
            j = {"status": "error", "error": p.stderr[-500:]}
        if p.returncode != 0 and j.get("status") == "ok":
            j["status"] = "error"
        res[name] = j
    return res


def cmd_gate3(args):
    manifest, ready, digest, counts, snap, ready_path = mmp.load_validated(args)
    os.makedirs(os.path.join(args.out, "packs"), exist_ok=True)
    members = manifest["modules"]
    mods = read_dump(args.dump)
    prepare(mods)
    _G.update(members=members, mods=mods)
    packs = manifest["packs"]
    if args.only:
        packs = [p for p in packs if p["id"] in set(args.only)]
    configs = [c for c in GATE3_CONFIGS
               if not args.configs or c[0] in args.configs]
    tasks = [(p, args.out, args.probe, args.time, configs) for p in packs]
    with concurrent.futures.ProcessPoolExecutor(
            args.jobs, mp_context=multiprocessing.get_context("fork")) as ex:
        results = list(ex.map(_gate3_worker, tasks, chunksize=1))
    g2 = {r["pack_id"]: r for r in load_csv(args.gate2_packs)}
    rows = []
    failures = 0
    for pack, res in zip(packs, results):
        row = dict(g2[pack["id"]])
        row["planned_copies"] = res["planned_copies"]
        for name, _, _ in configs:
            j = res[name]
            ok = j.get("status") == "ok" and j.get("verified") and \
                j.get("arx2_rejects")
            if not ok:
                failures += 1
            row[f"axst_{name}"] = j.get("stream_bytes") if ok else None
            row[f"axst_{name}_status"] = "ok" if ok else \
                (j.get("error") or "unverified")
            for key in ("copies", "copied_insts", "explicit_ids", "map_ids",
                        "delta_ids", "axst_decode_ms", "arx2_decode_ms",
                        "encode_ms", "plan_ms"):
                if name.startswith("copy") or key.endswith("_ms"):
                    row[f"{name}_{key}"] = j.get(key)
        rows.append(row)
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump({"tool": "multi_module_structure.py gate3",
                   "manifest_sha256": digest, "ready": ready,
                   "snapshot_check": snap,
                   "probe_sha256": mmp.sha256_file(args.probe),
                   "dump_sha256": mmp.sha256_file(args.dump),
                   "configs": configs, "packs": results}, f)
    keys = list(rows[0])
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(os.path.join(args.out, "packs.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)
    print(f"{len(packs)} packs; {failures} config failures")
    if failures:
        sys.exit(1)


G3_COLS = (
    ("arx2_bare_sum", "ARX2"), ("arx2_dedup", "ARX2+dedup"),
    ("g1b_cont_oce_carry", "warm (1b)"), ("axst_dedup", "AXST dedup"),
    ("axst_dedup_warm", "AXST dedup+warm"), ("axst_copy", "AXST copy"),
    ("axst_copy_warm", "AXST copy+warm"), ("est_idabs", "gate-2 est*"),
    ("best_generic", "best generic"),
)


def _median(xs):
    xs = sorted(xs)
    n = len(xs)
    return (xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2) \
        if xs else None


def _iqr(xs):
    xs = sorted(xs)
    if len(xs) < 4:
        return None
    q = lambda f: xs[min(len(xs) - 1, int(f * (len(xs) - 1) + 0.5))]
    return q(0.25), q(0.75)


def g3_table(rows, groups):
    head = ("| Group | Packs | Original B | " +
            " | ".join(c[1] for c in G3_COLS) +
            " | copy+warm vs ARX2 | median (IQR) | vs best generic"
            " | < generic | < warm (1b) | worst vs framed ARX2 |")
    lines = [head, "|" + "---|" * (head.count("|") - 1)]
    js = {}
    for name, pred in groups:
        sel = [r for r in rows if pred(r)]
        ok = [r for r in sel if all(r[c] is not None for c, _ in G3_COLS)]
        if not ok:
            continue
        o = sum(r["original_bytes"] for r in ok)
        tot = {c: sum(r[c] for r in ok) for c, _ in G3_COLS}
        per = [_pct(r["axst_copy_warm"], r["arx2_bare_sum"]) for r in ok]
        med = _median(per)
        iqr = _iqr(per)
        wg = sum(1 for r in ok if r["axst_copy_warm"] < r["best_generic"])
        ww = sum(1 for r in ok
                 if r["axst_copy_warm"] < r["g1b_cont_oce_carry"])
        worst = max(_pct(r["axst_copy_warm"], r["arx2_framed"]) for r in ok)
        npk = f"{len(ok)}" + (f" (of {len(sel)})" if len(ok) != len(sel)
                              else "")
        lines.append(
            f"| {name} | {npk} | {o:,} | " +
            " | ".join(f"{tot[c] / o:.4f}" for c, _ in G3_COLS) +
            f" | {_pct(tot['axst_copy_warm'], tot['arx2_bare_sum']):+.1f}%"
            f" | {med:+.1f}%" +
            (f" ({iqr[0]:+.1f}…{iqr[1]:+.1f})" if iqr else "") +
            f" | {_pct(tot['axst_copy_warm'], tot['best_generic']):+.1f}%"
            f" | {wg}/{len(ok)} | {ww}/{len(ok)} | {worst:+.2f}% |")
        js[name] = {"packs": len(ok), "skipped": len(sel) - len(ok),
                    "original_bytes": o, "totals": tot, "median_pct": med,
                    "iqr_pct": iqr, "lt_generic": wg, "lt_warm": ww,
                    "worst_vs_framed_pct": worst}
    return lines, js


def cmd_analyze3(args):
    with open(os.path.join(args.results, "packs.csv"), newline="") as f:
        raw = list(csv.DictReader(f))
    rows = _typed(raw)
    for r, x in zip(rows, raw):
        for k in x:
            if k.startswith("axst_") and not k.endswith("_status"):
                r[k] = _num(x[k])
    out = ["# Gate 3 validated structural prototype (generated)", "",
           "All AXST columns are complete research streams, decoded and "
           "byte-compared for every member (and rejected by the 1.0 decoder)."
           " `gate-2 est*` is the gate-2 estimate, for reference.", "",
           "## By source (byte-weighted ratio)", ""]
    t, js_src = g3_table(rows, SOURCE_GROUPS)
    out += t + ["", "## By manifest tier", ""]
    t, js_tier = g3_table(rows, TIER_GROUPS)
    out += t + [""]
    # Full-set totals without the gate-1b column (includes packs 1b skipped).
    full = [r for r in rows if r["axst_copy_warm"] is not None]
    o = sum(r["original_bytes"] for r in full)
    out += [f"All {len(full)} packs (incl. gate-1b skips): ARX2 "
            f"{sum(r['arx2_bare_sum'] for r in full) / o:.4f}, AXST copy+warm "
            f"{sum(r['axst_copy_warm'] for r in full) / o:.4f}.", ""]
    # Reject gates (agreed as trivial-reject thresholds only).
    nd = js_tier.get("  no exact duplicates")
    du = [r for r in rows if r["pack_id"] == "coll:smolv/dota2#unique"][0]
    rep = [r for r in rows if r["representative"]
           and r["axst_copy_warm"] is not None]
    worst = max(_pct(r["axst_copy_warm"], r["arx2_framed"]) for r in rep)
    g1 = _pct(nd["totals"]["axst_copy_warm"], nd["totals"]["arx2_bare_sum"])
    out += ["## Reject gates", "",
            "| Gate | Threshold | Result | Pass |", "|---|---|---|---|",
            f"| Duplicate-free rep collections vs ARX2 | ≤ −12% | {g1:+.1f}% |"
            f" {'yes' if g1 <= -12 else 'no'} |",
            f"| Dota 2 (#unique) vs best generic | < 0 | "
            f"{_pct(du['axst_copy_warm'], du['best_generic']):+.1f}% "
            f"({du['axst_copy_warm']:,} vs {du['best_generic']:,} "
            f"{du['best_generic_method']}) | "
            f"{'yes' if du['axst_copy_warm'] < du['best_generic'] else 'no'} |",
            f"| Worst representative pack vs framed ARX2 | ≤ +0.5% | "
            f"{worst:+.2f}% | {'yes' if worst <= 0.5 else 'no'} |", ""]
    js_time = None
    if args.timing:
        t, js_time = timing_table(args.timing, raw)
        out += t
    with open(os.path.join(args.results, "analysis.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    with open(os.path.join(args.results, "analysis.json"), "w") as f:
        json.dump({"by_source": js_src, "by_tier": js_tier,
                   "timing": js_time}, f, indent=1)
    print("\n".join(out))


def timing_table(timing_dir, size_rows):
    """Single-worker full-set decode, best of N (informational until the
    performance contract run): AXST configs versus independent ARX2 blobs."""
    with open(os.path.join(timing_dir, "packs.csv"), newline="") as f:
        raw = list(csv.DictReader(f))
    sizes = {r["pack_id"]: r for r in size_rows}
    mism = sum(1 for r in raw for c, _, _ in GATE3_CONFIGS
               if r.get(f"axst_{c}") != sizes[r["pack_id"]].get(f"axst_{c}"))
    rows = _typed(raw)
    for r, x in zip(rows, raw):
        for k, v in x.items():
            if k.endswith("_ms"):
                r[k] = float(v) if v not in ("", "None") else None
    lines = ["## Full-set decode time (single worker, best of N)", "",
             f"Timing build sizes differ from the size run in {mism} "
             "pack×config cells.", "",
             "| Group | Original MiB | ARX2 ms | ARX2 MiB/s | AXST dedup+warm"
             " | AXST copy | AXST copy+warm | copy+warm MiB/s"
             " | copy+warm / ARX2 |", "|" + "---|" * 9]
    js = {}
    for name, pred in SOURCE_GROUPS + TIER_GROUPS:
        sel = [r for r in rows if pred(r) and r["copy_warm_axst_decode_ms"]]
        if not sel:
            continue
        o = sum(r["original_bytes"] for r in sel) / 2 ** 20
        a = sum(r["copy_warm_arx2_decode_ms"] for r in sel)
        cw = sum(r["copy_warm_axst_decode_ms"] for r in sel)
        c = sum(r["copy_axst_decode_ms"] for r in sel)
        dw = sum(r["dedup_warm_axst_decode_ms"] for r in sel)
        lines.append(f"| {name} | {o:.2f} | {a:.1f} | {o / (a / 1000):.0f} |"
                     f" {dw:.1f} | {c:.1f} | {cw:.1f} | "
                     f"{o / (cw / 1000):.0f} | {cw / a:.2f}× |")
        js[name] = {"original_mib": o, "arx2_ms": a, "dedup_warm_ms": dw,
                    "copy_ms": c, "copy_warm_ms": cw}
    return lines + [""], js


def cmd_decode_gap(args):
    """Gate 4: size and single-worker decode time per group for selected
    configs. Several timing runs are pooled by per-cell minimum."""
    with open(os.path.join(args.results, "packs.csv"), newline="") as f:
        size_raw = {r["pack_id"]: r for r in csv.DictReader(f)}
    best = {}
    for d in args.timing:
        with open(os.path.join(d, "packs.csv"), newline="") as f:
            for r in csv.DictReader(f):
                cur = best.setdefault(r["pack_id"], {})
                for k, v in r.items():
                    if not k.endswith("_ms") or v in ("", "None"):
                        continue
                    if k.endswith("_encode_ms"):
                        continue
                    if k.endswith("arx2_decode_ms"):
                        k = "arx2_decode_ms"
                    cur[k] = min(cur.get(k, float("inf")), float(v))
    rows = []
    for pid, raw in size_raw.items():
        r = {k: v for k, v in _typed([raw])[0].items()
             if not k.endswith("_ms")}
        for c in args.configs:
            r[f"axst_{c}"] = _num(raw.get(f"axst_{c}", ""))
        r.update(best.get(pid, {}))
        rows.append(r)
    head = ("| Group | Original MiB | ARX2 B/orig | " +
            " | ".join(f"{c} size vs ARX2" for c in args.configs) +
            " | ARX2 ms | " +
            " | ".join(f"{c} / ARX2 time" for c in args.configs) + " |")
    out = ["# Decode gap (generated)", "",
           f"Timing runs pooled (per-cell min): {len(args.timing)}.", "",
           head, "|" + "---|" * (head.count("|") - 1)]
    for name, pred in SOURCE_GROUPS + TIER_GROUPS:
        sel = [r for r in rows if pred(r) and
               all(r.get(f"axst_{c}") is not None for c in args.configs)]
        if not sel:
            continue
        o = sum(r["original_bytes"] for r in sel)
        a = sum(r["arx2_bare_sum"] for r in sel)
        t = sum(r.get("arx2_decode_ms") or 0 for r in sel)

        def tm(c):
            if not all(r.get(f"{c}_axst_decode_ms") for r in sel) or not t:
                return "—"
            return f"{sum(r[f'{c}_axst_decode_ms'] for r in sel) / t:.3f}×"
        out.append(
            f"| {name} | {o / 2 ** 20:.2f} | {a / o:.4f} | " +
            " | ".join(f"{_pct(sum(r[f'axst_{c}'] for r in sel), a):+.1f}%"
                       for c in args.configs) +
            f" | {t:.1f} | " +
            " | ".join(tm(c) for c in args.configs) + " |")
    print("\n".join(out))


# ------------------------------------------------ gate 4 parallel decode

PARALLEL_POLICIES = (
    # label, arixv_struct_parallel args (beyond --blocks/--shared lists)
    ("solid", ["--blocks", "0"]),
    ("blocks", ["--blocks", "16,8,4,2"]),
    ("shared base (1 block)", ["--blocks", "16,8,4,2", "--shared", "2,4,8,16",
                               "--pick-shared", "1", "--base-blocks", "0"]),
    ("shared base (parallel)", ["--blocks", "16,8,4,2", "--shared",
                                "2,4,8,16", "--pick-shared", "1",
                                "--base-blocks", "1"]),
)


def cmd_parallel(args):
    """Block-parallel decode sweep (arixv_struct_parallel) on selected packs:
    one JSON line per pack x policy, plus a markdown frontier table."""
    os.makedirs(args.out, exist_ok=True)
    rows = []
    for pid in args.packs:
        safe = hashlib.sha256(pid.encode()).hexdigest()[:16]
        lst = os.path.join(args.lists, safe + ".list")
        for label, extra in PARALLEL_POLICIES:
            argv = [args.tool, "--id", pid, "--runs", str(args.runs),
                    "--workers", args.workers, "--flags", args.flags] + \
                extra + ["--", "@" + lst]
            p = subprocess.run(argv, capture_output=True, text=True)
            if p.returncode != 0:
                raise SystemExit(f"{pid} {label}: {p.stderr[-400:]}")
            for line in p.stdout.splitlines():
                d = json.loads(line)
                if "--shared" in extra and not d["shared"]:
                    continue  # the tool also prints its plain-block rows
                d["policy"] = label
                rows.append(d)
    with open(os.path.join(args.out, "parallel.jsonl"), "w") as f:
        for d in rows:
            f.write(json.dumps(d) + "\n")
    ws = [int(w) for w in args.workers.split(",")]
    out = ["# Block-parallel full-set decode (generated)", "",
           f"Flags `{args.flags}`; best of {args.runs}; times in ms; "
           "ratio = (streams + block index) / original. Shared policies "
           "include stage 1 (base sub-blocks) in the time.", ""]
    for pid in args.packs:
        sel = [d for d in rows if d["id"] == pid]
        if not sel:
            continue
        d0 = sel[0]
        out += [f"## {pid}", "",
                f"{d0['members']} members, {d0['original_bytes']:,} B; "
                f"ARX2 ratio {d0['arx2_bytes'] / d0['original_bytes']:.4f}.",
                "", "| Policy | shared | base blocks | block cap | blocks | "
                "ratio | " + " | ".join(f"{w}w ms" for w in ws) + " |",
                "|" + "---|" * (6 + len(ws))]
        arx = {w["w"]: w["arx2_ms"] for w in d0["workers"]}
        out.append("| ARX2 (per module) | | | | | "
                   f"{d0['arx2_bytes'] / d0['original_bytes']:.4f} | " +
                   " | ".join(f"{arx[w]:.2f}" for w in ws) + " |")
        for d in sel:
            ratio = (d["stream_bytes"] + d["index_bytes"]) / d["original_bytes"]
            t = {w["w"]: w["axst_ms"] for w in d["workers"]}
            out.append(f"| {d['policy']} | {d['shared'] or ''} | "
                       f"{d.get('base_blocks') or ''} | "
                       f"{d['block_count_cap'] or 'whole'} | {d['blocks']} | "
                       f"{ratio:.4f} | " +
                       " | ".join(f"{t[w]:.2f}" for w in ws) + " |")
        out.append("")
    with open(os.path.join(args.out, "parallel.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))


# ------------------------------------------------- independent-block sweep

BLOCK_COUNTS = (1, 2, 4, 8, 16, 32, 64, 0)       # 0 = whole pack
BLOCK_BYTES = (65536, 262144, 1048576)           # uncompressed-byte caps


def split_blocks(pack, members, count=0, cap=0):
    """Consecutive member blocks: at most `count` members, or (cap) a block
    closes before a member would push it past `cap` original bytes."""
    ids = pack["module_ids"]
    blocks, cur, cur_b = [], [], 0
    for mid in ids:
        b = members[mid]["byte_size"]
        full = (count and len(cur) >= count) or \
            (cap and cur and cur_b + b > cap)
        if full:
            blocks.append(cur)
            cur, cur_b = [], 0
        cur.append(mid)
        cur_b += b
    if cur:
        blocks.append(cur)
    return blocks


def _blocks_worker(task):
    pack, out_dir, probe, flags = task
    members, mods = _G["members"], _G["mods"]
    res = {"pack_id": pack["id"], "rows": []}
    variants = [("count", c) for c in BLOCK_COUNTS] + \
        [("bytes", c) for c in BLOCK_BYTES]
    for kind, val in variants:
        blocks = split_blocks(pack, members, count=val if kind == "count"
                              else 0, cap=val if kind == "bytes" else 0)
        total, ok = 0, True
        for bi, ids in enumerate(blocks):
            sub = dict(pack, id=f"{pack['id']}|{kind}{val}|{bi}",
                       module_ids=ids)
            safe = hashlib.sha256(sub["id"].encode()).hexdigest()[:16]
            lst = os.path.join(out_dir, "blocks", safe + ".list")
            plan = os.path.join(out_dir, "blocks", safe + ".plan")
            with open(lst, "w") as f:
                f.write("\n".join(members[m]["path"] for m in ids) + "\n")
            with open(plan, "w") as f:
                f.write("\n".join(plan_pack(sub, members, mods)) + "\n")
            p = subprocess.run([probe, "pack", "--flags", flags, "--plan",
                                plan, "--", "@" + lst],
                               capture_output=True, text=True)
            try:
                j = json.loads(p.stdout)
            except ValueError:
                j = {}
            if p.returncode != 0 or not j.get("verified"):
                ok = False
                break
            total += j["stream_bytes"]
        res["rows"].append({"kind": kind, "value": val, "blocks": len(blocks),
                            "bytes": total if ok else None})
    return res


def cmd_blocks(args):
    manifest, ready, digest, counts, snap, ready_path = mmp.load_validated(args)
    os.makedirs(os.path.join(args.out, "blocks"), exist_ok=True)
    members = manifest["modules"]
    mods = read_dump(args.dump)
    prepare(mods)
    _G.update(members=members, mods=mods)
    packs = [p for p in manifest["packs"] if len(p["module_ids"]) > 1]
    tasks = [(p, args.out, args.probe, args.flags) for p in packs]
    with concurrent.futures.ProcessPoolExecutor(
            args.jobs, mp_context=multiprocessing.get_context("fork")) as ex:
        results = list(ex.map(_blocks_worker, tasks, chunksize=1))
    g3 = {r["pack_id"]: r for r in load_csv(args.gate3_packs)}
    rows = []
    bad = 0
    for pack, res in zip(packs, results):
        g = g3[pack["id"]]
        for r in res["rows"]:
            bad += r["bytes"] is None
            rows.append({"pack_id": pack["id"], "tier": pack["tier"],
                         "representative": pack["representative"],
                         "members": len(pack["module_ids"]),
                         "original_bytes": int(g["original_bytes"]),
                         "arx2_bare_sum": int(g["arx2_bare_sum"]),
                         "kind": r["kind"], "value": r["value"],
                         "blocks": r["blocks"], "bytes": r["bytes"]})
    with open(os.path.join(args.out, "blocks.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    # Summary: total bytes relative to the whole-pack stream, per group.
    by = collections.defaultdict(dict)
    for r in rows:
        by[r["pack_id"]][(r["kind"], r["value"])] = r
    out = ["# Gate 3 independent-block sweep (generated)", "",
           f"Flags `{args.flags}`; every block is a complete, decode-verified "
           "AXST stream with its own header, duplicate detection and plan "
           "(bases inside the block only). Ratio = serialized / original.",
           ""]
    cols = [("count", c) for c in BLOCK_COUNTS] + \
        [("bytes", c) for c in BLOCK_BYTES]
    def lab(k, v):
        if k == "count":
            return "whole" if v == 0 else f"{v} mod"
        return f"≤{v // 1024} KiB"
    out.append("| Group | Packs | ARX2 | " +
               " | ".join(lab(k, v) for k, v in cols) + " |")
    out.append("|" + "---|" * (len(cols) + 3))
    for name, pred in SOURCE_GROUPS + TIER_GROUPS:
        sel = [pid for pid, d in by.items()
               if pred(_typed([dict(g3[pid])])[0])]
        sel = [pid for pid in sel if all(by[pid][c]["bytes"] is not None
                                         for c in cols)]
        if not sel:
            continue
        o = sum(by[pid][cols[0]]["original_bytes"] for pid in sel)
        a = sum(by[pid][cols[0]]["arx2_bare_sum"] for pid in sel)
        cells = [f"{sum(by[pid][c]['bytes'] for pid in sel) / o:.4f}"
                 for c in cols]
        out.append(f"| {name} | {len(sel)} | {a / o:.4f} | " +
                   " | ".join(cells) + " |")
    with open(os.path.join(args.out, "blocks.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))
    if bad:
        sys.exit(f"{bad} block configurations failed verification")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--manifest", required=True)
    r.add_argument("--ready")
    r.add_argument("--corpus-dir")
    r.add_argument("--source-lock")
    r.add_argument("--structure-dump",
                   help="arixv_structure_dump binary (dumps if --dump absent)")
    r.add_argument("--dump", help="existing AXSD dump")
    r.add_argument("--gate1a-baselines", required=True)
    r.add_argument("--gate1a-per-module", required=True)
    r.add_argument("--gate1b-packs", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--jobs", type=int, default=os.cpu_count())
    g = sub.add_parser("gate3")
    g.add_argument("--manifest", required=True)
    g.add_argument("--ready")
    g.add_argument("--corpus-dir")
    g.add_argument("--source-lock")
    g.add_argument("--dump", required=True)
    g.add_argument("--probe", required=True,
                   help="arixv_struct_probe (retired after 99df5ed; build that "
                        "commit to rerun gate 3)")
    g.add_argument("--gate2-packs", required=True)
    g.add_argument("--out", required=True)
    g.add_argument("--only", nargs="*")
    g.add_argument("--time", type=int, default=0)
    g.add_argument("--configs", nargs="*",
                   help="subset of GATE3_CONFIGS names (default all)")
    g.add_argument("--jobs", type=int, default=os.cpu_count())
    b = sub.add_parser("blocks")
    b.add_argument("--manifest", required=True)
    b.add_argument("--ready")
    b.add_argument("--corpus-dir")
    b.add_argument("--source-lock")
    b.add_argument("--dump", required=True)
    b.add_argument("--probe", required=True,
                   help="arixv_struct_probe (retired after 99df5ed)")
    b.add_argument("--gate3-packs", required=True)
    b.add_argument("--flags", default="signal,warm")
    b.add_argument("--out", required=True)
    b.add_argument("--jobs", type=int, default=os.cpu_count())
    a3 = sub.add_parser("analyze3")
    a3.add_argument("--results", required=True)
    a3.add_argument("--timing", help="gate3 output dir run with --time")
    a = sub.add_parser("analyze")
    a.add_argument("--results", required=True)
    pp = sub.add_parser("parallel")
    pp.add_argument("--tool", required=True, help="arixv_struct_parallel")
    pp.add_argument("--lists", required=True,
                    help="gate3 packs/ dir (<sha16>.list member lists)")
    pp.add_argument("--packs", nargs="+", required=True)
    pp.add_argument("--workers", default="1,2,4,8,16")
    pp.add_argument("--runs", type=int, default=7)
    pp.add_argument("--flags", default="signal,warm")
    pp.add_argument("--out", required=True)
    a4 = sub.add_parser("decode-gap")
    a4.add_argument("--results", required=True,
                    help="gate3 size run (all configs)")
    a4.add_argument("--timing", required=True, nargs="+",
                    help="gate3 --time runs (pooled: per-cell minimum)")
    a4.add_argument("--configs", nargs="+",
                    default=["copy_warm", "copy_warm_noadapt"])
    args = ap.parse_args()
    if args.cmd == "gate3":
        cmd_gate3(args)
    if args.cmd == "blocks":
        cmd_blocks(args)
    if args.cmd == "analyze3":
        cmd_analyze3(args)
    if args.cmd == "analyze":
        cmd_analyze(args)
    if args.cmd == "parallel":
        cmd_parallel(args)
    if args.cmd == "decode-gap":
        cmd_decode_gap(args)
    if args.cmd == "run":
        if args.dump is None and args.structure_dump is None:
            ap.error("need --structure-dump or --dump")
        cmd_run(args)


if __name__ == "__main__":
    main()
