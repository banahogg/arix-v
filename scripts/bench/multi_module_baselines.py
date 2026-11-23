#!/usr/bin/env python3
"""Gate 1a: size-only multi-module baselines and generic conditional costs.

Reads a frozen manifest from multi_module_manifest.py and measures, for every
pack, the serialized full-set size of a fixed method list with exact
reconstruction checks, plus generic conditional target costs for every prefix
case. No ARIX-V model changes: ARX2 blobs come from the unchanged ``arixv``
CLI, so this is the zero-modeling baseline.

Common framing (MMF, used by every framed method unless stated):
  compact_u32(count) || compact_u32(len_1) .. compact_u32(len_n) || payload_1 .. payload_n
where compact_u32 is ARX2's canonical 1..5-byte prefix code (src/container.h).
Member identity is the 0-based position in the pack's frozen ``module_ids``
order (the manifest maps position -> logical key); no key bytes and no order
map are charged to any method because every method uses manifest order.
Tar variants (secondary) carry their own names "%06d.spv"/"%06d.smolv".

Outputs go to --out (default build/gate1a/results); see --help.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import lzma
import os
import platform
import shutil
import statistics
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

try:
    from compression import zstd as _zstd  # Python >= 3.14
except ImportError:  # pragma: no cover - reported as unavailable
    _zstd = None

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOCK = ROOT / "deps" / "benchmark-sources.lock.json"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import multi_module_manifest as mmm  # noqa: E402

SET_MAGIC = b"AXS0"  # research-only shared-header wrapper magic; not an allocated format
ARX2_MAGIC = b"ARX2"

# --------------------------------------------------------------------------
# Framing primitives (mirror src/container.h exactly)
# --------------------------------------------------------------------------

L1 = 192
L2 = L1 + 60 * 256
L3 = L2 + (1 << 16)
L4 = L3 + (1 << 24)


def compact_u32(v: int) -> bytes:
    if not 0 <= v <= 0xFFFFFFFF:
        raise ValueError(v)
    if v < L1:
        return bytes([v])
    if v < L2:
        d = v - L1
        return bytes([0xC0 + (d >> 8), d & 0xFF])
    if v < L3:
        return b"\xFC" + struct.pack("<H", v - L2)
    if v < L4:
        return b"\xFD" + struct.pack("<I", v - L3)[:3]
    return b"\xFE" + struct.pack("<I", v)


def read_compact_u32(buf: bytes, pos: int) -> tuple[int, int]:
    b0 = buf[pos]
    if b0 < 0xC0:
        return b0, pos + 1
    if b0 < 0xFC:
        return L1 + ((b0 - 0xC0) << 8) + buf[pos + 1], pos + 2
    if b0 == 0xFC:
        return L2 + struct.unpack_from("<H", buf, pos + 1)[0], pos + 3
    if b0 == 0xFD:
        return L3 + (buf[pos + 1] | buf[pos + 2] << 8 | buf[pos + 3] << 16), pos + 4
    if b0 == 0xFE:
        v = struct.unpack_from("<I", buf, pos + 1)[0]
        if v < L4:
            raise ValueError("non-canonical compact u32")
        return v, pos + 5
    raise ValueError("invalid compact u32 prefix 0xFF")


def mmf_frame(members: list[bytes]) -> bytes:
    out = bytearray(compact_u32(len(members)))
    for m in members:
        out += compact_u32(len(m))
    for m in members:
        out += m
    return bytes(out)


def mmf_overhead(members: list[bytes]) -> int:
    return len(compact_u32(len(members))) + sum(len(compact_u32(len(m))) for m in members)


def mmf_parse(buf: bytes) -> list[bytes]:
    n, pos = read_compact_u32(buf, 0)
    lens = []
    for _ in range(n):
        v, pos = read_compact_u32(buf, pos)
        lens.append(v)
    out = []
    for ln in lens:
        if pos + ln > len(buf):
            raise ValueError("truncated MMF payload")
        out.append(buf[pos:pos + ln])
        pos += ln
    if pos != len(buf):
        raise ValueError("trailing bytes after MMF payloads")
    return out


def parse_arx2(blob: bytes) -> tuple[int, int, int]:
    """Return (mode, header_size_field, payload_offset) of a standalone ARX2 blob."""
    if blob[:4] != ARX2_MAGIC or len(blob) < 6:
        raise ValueError("not an ARX2 blob")
    mode = blob[4]
    size_field, pos = read_compact_u32(blob, 5)
    return mode, size_field, pos


def shared_header_pack(blobs: list[bytes]) -> bytes:
    """Zero-modeling wrapper: one set magic, per-member mode/size/payload length.

    Layout: "AXS0" || compact(count) ||
            for each member: mode u8 || compact(size_field) || compact(payload_len)
            || payloads in order.
    Every original payload byte is kept unchanged.
    """
    out = bytearray(SET_MAGIC + compact_u32(len(blobs)))
    payloads = []
    for b in blobs:
        mode, size_field, off = parse_arx2(b)
        out.append(mode)
        out += compact_u32(size_field)
        out += compact_u32(len(b) - off)
        payloads.append(b[off:])
    for p in payloads:
        out += p
    return bytes(out)


def shared_header_unpack(buf: bytes) -> list[bytes]:
    """Reconstruct the exact standalone ARX2 blobs."""
    if buf[:4] != SET_MAGIC:
        raise ValueError("bad set magic")
    n, pos = read_compact_u32(buf, 4)
    hdrs = []
    for _ in range(n):
        mode = buf[pos]
        size_field, pos = read_compact_u32(buf, pos + 1)
        plen, pos = read_compact_u32(buf, pos)
        hdrs.append((mode, size_field, plen))
    out = []
    for mode, size_field, plen in hdrs:
        if pos + plen > len(buf):
            raise ValueError("truncated wrapper payload")
        out.append(ARX2_MAGIC + bytes([mode]) + compact_u32(size_field) + buf[pos:pos + plen])
        pos += plen
    if pos != len(buf):
        raise ValueError("trailing bytes after wrapper payloads")
    return out


def shared_header_metadata(blobs: list[bytes]) -> int:
    meta = len(SET_MAGIC) + len(compact_u32(len(blobs)))
    for b in blobs:
        _, size_field, off = parse_arx2(b)
        meta += 1 + len(compact_u32(size_field)) + len(compact_u32(len(b) - off))
    return meta


def dedup_pack(blobs: list[bytes]) -> bytes:
    """Exact-file dedup control: compact(count) then per member either
    tag 0 || compact(len) || blob (new), or tag k (copy of k-th unique, 1-based)."""
    out = bytearray(compact_u32(len(blobs)))
    seen: dict[bytes, int] = {}
    for b in blobs:
        k = seen.get(b)
        if k is None:
            seen[b] = len(seen) + 1
            out += compact_u32(0) + compact_u32(len(b)) + b
        else:
            out += compact_u32(k)
    return bytes(out)


def dedup_unpack(buf: bytes) -> list[bytes]:
    n, pos = read_compact_u32(buf, 0)
    uniq: list[bytes] = []
    out = []
    for _ in range(n):
        tag, pos = read_compact_u32(buf, pos)
        if tag == 0:
            ln, pos = read_compact_u32(buf, pos)
            if pos + ln > len(buf):
                raise ValueError("truncated dedup blob")
            uniq.append(buf[pos:pos + ln])
            out.append(uniq[-1])
            pos += ln
        else:
            if tag > len(uniq):
                raise ValueError("dedup reference out of range")
            out.append(uniq[tag - 1])
    if pos != len(buf):
        raise ValueError("trailing bytes after dedup stream")
    return out


def canonical_tar(members: list[bytes], suffix: str) -> bytes:
    """ustar, names %06d<suffix>, mtime/uid/gid 0, empty owner names, mode 0644,
    Python tarfile default record padding (10240-byte records)."""
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for i, data in enumerate(members):
            ti = tarfile.TarInfo(f"{i:06d}{suffix}")
            ti.size = len(data)
            ti.mtime = 0
            ti.mode = 0o644
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            tf.addfile(ti, io.BytesIO(data))
    return bio.getvalue()


def untar(buf: bytes) -> list[bytes]:
    with tarfile.open(fileobj=io.BytesIO(buf), mode="r:") as tf:
        names = tf.getmembers()
        return [tf.extractfile(m).read() for m in sorted(names, key=lambda m: m.name)]


# --------------------------------------------------------------------------
# Generic codecs with pinned parameters
# --------------------------------------------------------------------------

ZSTD_SETTINGS = {
    "zstd19": {"level": 19, "window_log": 23},
    "zstd22w27": {"level": 22, "window_log": 27},
}
XZ_SETTINGS = {"xz9e": {"preset": "9|PRESET_EXTREME", "check": "CRC64",
                        "format": "FORMAT_XZ", "dict_size": 64 << 20}}


def zstd_options(name: str) -> dict:
    s = ZSTD_SETTINGS[name]
    P = _zstd.CompressionParameter
    return {P.compression_level: s["level"], P.window_log: s["window_log"],
            P.checksum_flag: 0, P.content_size_flag: 1, P.dict_id_flag: 0}


def zstd_c(data: bytes, name: str = "zstd19", zdict=None) -> bytes:
    return _zstd.compress(data, options=zstd_options(name), zstd_dict=zdict)


def zstd_d(data: bytes, zdict=None, window_log_max: int = 27) -> bytes:
    opts = {_zstd.DecompressionParameter.window_log_max: window_log_max}
    return _zstd.decompress(data, zstd_dict=zdict, options=opts)


def zstd_frame_window(frame: bytes) -> int:
    """Effective window size from a zstd frame header (RFC 8878 §3.1.1.1)."""
    fhd = frame[4]
    single = (fhd >> 5) & 1
    dict_flag = fhd & 3
    fcs_flag = fhd >> 6
    pos = 5
    if not single:
        wd = frame[pos]
        exp, mant = wd >> 3, wd & 7
        base = 1 << (10 + exp)
        return base + (base // 8) * mant
    pos += (0, 1, 2, 4)[dict_flag]
    fcs_size = (1, 2, 4, 8)[fcs_flag]
    v = int.from_bytes(frame[pos:pos + fcs_size], "little")
    return v + 256 if fcs_size == 2 else v


def xz_c(data: bytes) -> bytes:
    return lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64,
                         preset=9 | lzma.PRESET_EXTREME)


def xz_d(data: bytes) -> bytes:
    return lzma.decompress(data, format=lzma.FORMAT_XZ)


# --------------------------------------------------------------------------
# Per-module encodings (cached by content hash)
# --------------------------------------------------------------------------

SMOLV_RUNNER = r"""
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "smolv.h"
static bool Read(const std::string& p, std::vector<uint8_t>& d) {
  std::ifstream f(p, std::ios::binary); if (!f) return false;
  d.assign(std::istreambuf_iterator<char>(f), {}); return true;
}
static bool Write(const std::string& p, const uint8_t* d, size_t n) {
  std::ofstream f(p, std::ios::binary); if (!f) return false;
  f.write(reinterpret_cast<const char*>(d), (std::streamsize)n); return (bool)f;
}
// enc <outdir> <files...>: encode (flags 0, lossless), verify decode, write <outdir>/<basename>.smolv
// dec <in.smolv> <out.spv>: decode
int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "enc") {
    int failures = 0;
    for (int i = 3; i < argc; ++i) {
      std::string in = argv[i];
      std::vector<uint8_t> src; smolv::ByteArray enc;
      if (!Read(in, src)) { std::cout << "FAIL\tread\t" << in << "\n"; ++failures; continue; }
      if (!smolv::Encode(src.data(), src.size(), enc, smolv::kEncodeFlagNone)) {
        std::cout << "FAIL\tencode\t" << in << "\n"; ++failures; continue; }
      size_t n = smolv::GetDecodedBufferSize(enc.data(), enc.size());
      std::vector<uint8_t> dec(n);
      if (n != src.size() || !smolv::Decode(enc.data(), enc.size(), dec.data(), n) || dec != src) {
        std::cout << "FAIL\tverify\t" << in << "\n"; ++failures; continue; }
      std::string base = in.substr(in.find_last_of('/') + 1);
      if (!Write(std::string(argv[2]) + "/" + base + ".smolv", enc.data(), enc.size())) {
        std::cout << "FAIL\twrite\t" << in << "\n"; ++failures; continue; }
      std::cout << "OK\t" << enc.size() << "\t" << in << "\n";
    }
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "dec") {
    std::vector<uint8_t> enc; if (!Read(argv[2], enc)) return 1;
    size_t n = smolv::GetDecodedBufferSize(enc.data(), enc.size());
    std::vector<uint8_t> dec(n);
    if (!n || !smolv::Decode(enc.data(), enc.size(), dec.data(), n)) return 1;
    return Write(argv[3], dec.data(), n) ? 0 : 1;
  }
  std::cerr << "usage: smolv_gate1a enc <outdir> <files...> | dec <in> <out>\n";
  return 2;
}
"""


def compiler() -> str:
    for c in ("c++", "g++", "clang++"):
        if shutil.which(c):
            return c
    raise SystemExit("no C++ compiler")


def build_smolv(smolv_dir: Path, out: Path) -> tuple[Path, dict]:
    out.mkdir(parents=True, exist_ok=True)
    src = out / "smolv_gate1a.cc"
    src.write_text(SMOLV_RUNNER, encoding="utf-8")
    exe = out / "smolv_gate1a"
    cxx = compiler()
    flags = ["-O2", "-std=c++17", "-DNDEBUG"]
    cmd = [cxx, *flags, f"-I{smolv_dir / 'source'}", str(smolv_dir / "source" / "smolv.cpp"),
           str(src), "-o", str(exe)]
    subprocess.run(cmd, check=True)
    ver = subprocess.run([cxx, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    return exe, {"compiler": cxx, "compiler_version": ver, "flags": flags,
                 "encode_flags": "kEncodeFlagNone (no debug stripping)",
                 "runner_sha256": hashlib.sha256(SMOLV_RUNNER.encode()).hexdigest()}


def _arx2_one(args: tuple[str, str, str, str]) -> dict:
    arixv, src, dst, digest = args
    r = {"sha256": digest, "ok": False}
    p = subprocess.run([arixv, "c", src, dst], capture_output=True, text=True)
    if p.returncode:
        r["error"] = f"encode rc={p.returncode} {p.stderr.strip()[:200]}"
        return r
    back = dst + ".dec"
    p = subprocess.run([arixv, "d", dst, back], capture_output=True, text=True)
    if p.returncode:
        r["error"] = f"decode rc={p.returncode} {p.stderr.strip()[:200]}"
        return r
    same = Path(back).read_bytes() == Path(src).read_bytes()
    os.unlink(back)
    if not same:
        r["error"] = "decode mismatch"
        return r
    blob = Path(dst).read_bytes()
    mode, size_field, off = parse_arx2(blob)
    r.update(ok=True, bytes=len(blob), mode=mode, size_field=size_field, header_bytes=off)
    return r


def _arx2_decode_check(args: tuple[str, bytes, bytes]) -> bool:
    arixv, blob, original = args
    with tempfile.TemporaryDirectory() as td:
        a, b = Path(td, "m.arx2"), Path(td, "m.spv")
        a.write_bytes(blob)
        p = subprocess.run([arixv, "d", str(a), str(b)], capture_output=True)
        return p.returncode == 0 and b.read_bytes() == original


# --------------------------------------------------------------------------
# Pack evaluation (runs in worker processes)
# --------------------------------------------------------------------------

G: dict = {}


def _init_worker(cache: str) -> None:
    G["cache"] = Path(cache)


def _load(kind: str, digest: str) -> bytes | None:
    p = G["cache"] / kind / f"{digest}.{kind}"
    return p.read_bytes() if p.exists() else None


METHODS = [
    # id, family, role
    ("arx2_bare_sum", "arx2", "required"),
    ("arx2_framed", "arx2", "required"),
    ("arx2_shared_header", "arx2", "required"),
    ("arx2_dedup", "arx2", "required"),
    ("arx2_solid_zstd19", "arx2", "sanity"),
    ("orig_perfile_zstd19", "generic", "required"),
    ("orig_solid_zstd19", "generic", "required"),
    ("smolv_solid_zstd19", "generic", "required"),
    ("orig_tar_zstd19", "generic", "secondary"),
    ("smolv_tar_zstd19", "generic", "secondary"),
    ("orig_solid_zstd22w27", "generic", "strong_reference"),
    ("smolv_solid_zstd22w27", "generic", "strong_reference"),
    ("orig_solid_xz9e", "generic", "strong_reference"),
    ("smolv_solid_xz9e", "generic", "strong_reference"),
]
METHOD_ROLE = {m: r for m, _, r in METHODS}


def eval_pack(job: dict) -> dict:
    pid, shas = job["id"], job["shas"]
    orig = [_load("spv", d) for d in shas]
    arx = [_load("arx2", d) for d in shas]
    smv = [_load("smolv", d) for d in shas]
    logical = sum(len(o) for o in orig)
    rows = []

    def row(method, total, payload, meta, meta_compressed, verified, note="", extra=None):
        r = {"pack_id": pid, "method": method, "role": METHOD_ROLE[method],
             "members": len(shas), "original_bytes": logical, "total_bytes": total,
             "payload_bytes": payload, "metadata_bytes": meta,
             "metadata_compressed": meta_compressed,
             "ratio": (total / logical) if (total is not None and logical) else None,
             "verified": verified, "note": note}
        if extra:
            r.update(extra)
        rows.append(r)

    def unavailable(method, why):
        row(method, None, None, None, None, False, why)

    # ARX2 family
    if all(a is not None for a in arx):
        hdr = sum(parse_arx2(a)[2] for a in arx)
        row("arx2_bare_sum", sum(map(len, arx)), sum(map(len, arx)) - hdr, hdr, False, True,
            "sum of standalone blobs; no set framing; metadata = per-blob ARX2 headers",
            {"arx2_header_bytes": hdr})
        framed = mmf_frame(arx)
        ok = mmf_parse(framed) == arx
        row("arx2_framed", len(framed), sum(map(len, arx)), mmf_overhead(arx), False, ok,
            "MMF(standalone ARX2 blobs); metadata = MMF count+lengths (blob headers in payload)",
            {"arx2_header_bytes": hdr})
        sh = shared_header_pack(arx)
        rec = shared_header_unpack(sh)
        ok = rec == arx
        meta = shared_header_metadata(arx)
        row("arx2_shared_header", len(sh), len(sh) - meta, meta, False, ok,
            "AXS0 wrapper; reconstructs exact ARX2 blobs (decode checked separately)",
            {"reconstructed_blob_sha256": [hashlib.sha256(b).hexdigest() for b in rec]})
        dd = dedup_pack(arx)
        ok = dedup_unpack(dd) == arx
        uniq_bytes = sum(len(b) for b in {bytes(b): None for b in arx})
        row("arx2_dedup", len(dd), uniq_bytes, len(dd) - uniq_bytes, False, ok,
            "exact-blob dedup with per-member tag/ref")
        z = zstd_c(framed)
        ok = mmf_parse(zstd_d(z)) == arx
        row("arx2_solid_zstd19", len(z), None, mmf_overhead(arx), True, ok,
            "sanity control: zstd-19 over MMF(ARX2)", {"zstd_window": zstd_frame_window(z)})
    else:
        for m in ("arx2_bare_sum", "arx2_framed", "arx2_shared_header", "arx2_dedup",
                  "arx2_solid_zstd19"):
            unavailable(m, "ARX2 encode failed for a member")

    # Generic on original modules
    frames = [zstd_c(o) for o in orig]
    pf = mmf_frame(frames)
    ok = [zstd_d(f) for f in mmf_parse(pf)] == orig
    row("orig_perfile_zstd19", len(pf), sum(map(len, frames)), mmf_overhead(frames), False, ok,
        "MMF(per-file zstd-19 frames)")
    raw = mmf_frame(orig)
    for name, mid in (("zstd19", "orig_solid_zstd19"), ("zstd22w27", "orig_solid_zstd22w27")):
        z = zstd_c(raw, name)
        ok = mmf_parse(zstd_d(z)) == orig
        row(mid, len(z), None, mmf_overhead(orig), True, ok, f"{name} over MMF(original)",
            {"zstd_window": zstd_frame_window(z), "stream_bytes": len(raw)})
    x = xz_c(raw)
    row("orig_solid_xz9e", len(x), None, mmf_overhead(orig), True, mmf_parse(xz_d(x)) == orig,
        "xz -9e (liblzma preset 9e, CRC64) over MMF(original)", {"stream_bytes": len(raw)})
    t = canonical_tar(orig, ".spv")
    z = zstd_c(t)
    row("orig_tar_zstd19", len(z), None, len(t) - logical, True, untar(zstd_d(z)) == orig,
        "canonical ustar + zstd-19; metadata = tar headers+padding", {"tar_bytes": len(t)})

    # smol-v
    if all(s is not None for s in smv):
        sraw = mmf_frame(smv)
        for name, mid in (("zstd19", "smolv_solid_zstd19"), ("zstd22w27", "smolv_solid_zstd22w27")):
            z = zstd_c(sraw, name)
            ok = mmf_parse(zstd_d(z)) == smv
            row(mid, len(z), None, mmf_overhead(smv), True, ok,
                f"{name} over MMF(smol-v blobs); blobs verified to decode exactly",
                {"zstd_window": zstd_frame_window(z), "stream_bytes": len(sraw)})
        x = xz_c(sraw)
        row("smolv_solid_xz9e", len(x), None, mmf_overhead(smv), True, mmf_parse(xz_d(x)) == smv,
            "xz -9e over MMF(smol-v)", {"stream_bytes": len(sraw)})
        t = canonical_tar(smv, ".smolv")
        z = zstd_c(t)
        row("smolv_tar_zstd19", len(z), None, len(t) - sum(map(len, smv)), True,
            untar(zstd_d(z)) == smv, "canonical ustar(smol-v) + zstd-19", {"tar_bytes": len(t)})
    else:
        bad = [d for d, s in zip(shas, smv) if s is None]
        for m in ("smolv_solid_zstd19", "smolv_solid_zstd22w27", "smolv_solid_xz9e",
                  "smolv_tar_zstd19"):
            unavailable(m, f"smol-v unsupported for {len(bad)} member(s): "
                           + ",".join(b[:12] for b in bad[:5]))
    return {"pack_id": pid, "rows": rows}


def eval_curve(job: dict) -> dict:
    """Fully finalized prefix encodes: size(C(MMF(m_1..m_k))) for every k."""
    orig = [_load("spv", d) for d in job["shas"]]
    arx = [_load("arx2", d) for d in job["shas"]]
    out = []
    prev = {"zstd19": len(zstd_c(mmf_frame([]))), "xz9e": len(xz_c(mmf_frame([]))),
            "arx2_framed": len(mmf_frame([]))}
    for k in range(1, len(orig) + 1):
        cur = {"zstd19": len(zstd_c(mmf_frame(orig[:k]))),
               "xz9e": len(xz_c(mmf_frame(orig[:k])))}
        if all(a is not None for a in arx[:k]):
            cur["arx2_framed"] = len(mmf_frame(arx[:k]))
        for m, v in cur.items():
            out.append({"pack_id": job["id"], "method": m, "k": k,
                        "module_id": job["ids"][k - 1], "module_bytes": len(orig[k - 1]),
                        "cumulative_original_bytes": sum(len(o) for o in orig[:k]),
                        "cumulative_bytes": v, "marginal_bytes": v - prev.get(m, 0)})
            prev[m] = v
    return {"pack_id": job["id"], "rows": out}


def eval_cases(job: dict) -> list[dict]:
    """Conditional target costs for a batch of prefix cases with the same prefix."""
    prefix = [_load("spv", d) for d in job["prefix_shas"]]
    pre_raw = b"".join(prefix)
    pre_xz = len(xz_c(mmf_frame(prefix)))
    zd = _zstd.ZstdDict(pre_raw, is_raw=True) if pre_raw else None
    res = []
    for c in job["cases"]:
        tgt = _load("spv", c["target_sha"])
        z = zstd_c(tgt, "zstd19", zd)
        z_ok = zstd_d(z, zd) == tgt
        full = xz_c(mmf_frame(prefix + [tgt]))
        x_ok = mmf_parse(xz_d(full)) == prefix + [tgt]
        arx = _load("arx2", c["target_sha"])
        res.append({**c["row"], "prefix_bytes": len(pre_raw), "prefix_members": len(prefix),
                    "target_bytes": len(tgt),
                    "zstd_rawdict_target_bytes": len(z),
                    "xz_prefix_bytes": pre_xz, "xz_prefix_plus_target_bytes": len(full),
                    "xz_conditional_target_bytes": len(full) - pre_xz,
                    "arx2_standalone_target_bytes": len(arx) if arx else None,
                    "verified": z_ok and x_ok})
    return res


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def write_csv(path: Path, rows: list[dict], fields: list[str] | None = None) -> None:
    fields = fields or sorted({k for r in rows for k in r})
    tmp = path.with_name(f".{path.name}.tmp")
    with tmp.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow({k: (json.dumps(v) if isinstance(v, (list, dict)) else v)
                        for k, v in r.items()})
    os.replace(tmp, path)


def tool_versions(arixv: Path, smolv_meta: dict | None, smolv_dir: Path | None) -> dict:
    head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--", "src",
                            "include", "cli", "CMakeLists.txt"],
                           capture_output=True, text=True).stdout.strip()
    zcli = shutil.which("zstd")
    xcli = shutil.which("xz")
    return {
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "arixv_cli": str(arixv), "arixv_cli_sha256": mmm.sha256_file(arixv),
        "arixv_source_commit": head, "arixv_codec_sources_modified": bool(dirty),
        "libzstd": _zstd.zstd_version if _zstd else None,
        "zstd_settings": {k: {**v, "checksum_flag": 0, "content_size_flag": 1,
                              "dict_id_flag": 0, "strategy": "level default"}
                          for k, v in ZSTD_SETTINGS.items()},
        "zstd_cli": (subprocess.run([zcli, "--version"], capture_output=True, text=True)
                     .stdout.strip() if zcli else None),
        "liblzma": lzma.__name__ and _lzma_version(),
        "xz_settings": XZ_SETTINGS,
        "xz_cli": (subprocess.run([xcli, "--version"], capture_output=True, text=True)
                   .stdout.splitlines()[0] if xcli else None),
        "smolv": {**(smolv_meta or {}), "checkout": str(smolv_dir) if smolv_dir else None,
                  "revision": mmm.git_head(smolv_dir) if smolv_dir else None},
        "tar": "Python tarfile USTAR, canonical names/owners/mtime, 10240-byte records",
    }


def _lzma_version() -> str:
    xcli = shutil.which("xz")
    if xcli:
        out = subprocess.run([xcli, "--version"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if line.startswith("liblzma"):
                return line.split()[-1] + " (CLI-reported; Python lzma links system liblzma)"
    return "unknown"


def cross_check_cli(sample: bytes, scratch: Path) -> dict:
    """Confirm the library settings reproduce the CLI sizes on one input."""
    out = {}
    p = scratch / "xcheck.bin"
    p.write_bytes(sample)
    if shutil.which("xz"):
        cli = subprocess.run(["xz", "-9e", "-T1", "-c", str(p)], capture_output=True).stdout
        out["xz_cli_9e_T1_bytes"] = len(cli)
        out["xz_lib_bytes"] = len(xz_c(sample))
    if shutil.which("zstd"):
        cli = subprocess.run(["zstd", "-19", "--no-check", "-c", "-q", str(p)],
                             capture_output=True).stdout
        out["zstd_cli_19_nocheck_bytes"] = len(cli)
        out["zstd_lib_bytes"] = len(zstd_c(sample))
    p.unlink()
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--arixv", type=Path, required=True, help="unchanged arixv CLI binary")
    ap.add_argument("--smolv-dir", type=Path, default=None,
                    help="pinned smol-v checkout (verified against the lockfile)")
    ap.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "gate1a" / "results")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--skip-curves", action="store_true")
    ap.add_argument("--skip-cases", action="store_true")
    ap.add_argument("--pack-filter", default=None, help="substring filter on pack ids (debug)")
    ap.add_argument("--report-only", action="store_true",
                    help="regenerate report.md/paired_differences.csv from existing results")
    args = ap.parse_args(argv)
    if args.report_only:
        write_report(args.out, args.manifest)
        print(f"report {args.out / 'report.md'}")
        return 0
    if _zstd is None:
        raise SystemExit("compression.zstd (Python >= 3.14) is required for pinned zstd")

    t0 = time.time()
    man_bytes = args.manifest.read_bytes()
    man_sha = hashlib.sha256(man_bytes).hexdigest()
    man = json.loads(man_bytes)
    errs = mmm.validate_manifest(man, check_files=True)
    if errs:
        raise SystemExit("manifest invalid:\n  " + "\n  ".join(errs[:20]))
    mods = man["modules"]
    out = args.out
    cache = out / "cache"
    for k in ("spv", "arx2", "smolv"):
        (cache / k).mkdir(parents=True, exist_ok=True)
    unavailable_methods = []

    # Module content cache (byte-identical copies, hash verified).
    uniq: dict[str, str] = {}
    for m in mods.values():
        uniq.setdefault(m["sha256"], m["path"])
    for digest, path in uniq.items():
        dst = cache / "spv" / f"{digest}.spv"
        if not dst.exists() or mmm.sha256_file(dst) != digest:
            shutil.copyfile(path, dst)
        if mmm.sha256_file(dst) != digest:
            raise SystemExit(f"cache copy mismatch {path}")

    # ARX2 via unchanged CLI.
    arx_jobs = [(str(args.arixv), str(cache / "spv" / f"{d}.spv"),
                 str(cache / "arx2" / f"{d}.arx2"), d) for d in sorted(uniq)]
    with ProcessPoolExecutor(args.jobs) as ex:
        arx_res = list(ex.map(_arx2_one, arx_jobs, chunksize=16))
    arx_fail = [r for r in arx_res if not r["ok"]]
    for r in arx_fail:
        p = cache / "arx2" / f"{r['sha256']}.arx2"
        if p.exists():
            p.unlink()
    print(f"arx2: {len(arx_res) - len(arx_fail)}/{len(arx_res)} round-tripped", flush=True)

    # smol-v from the pinned checkout.
    smolv_meta = None
    smolv_res: dict[str, dict] = {}
    smolv_dir = args.smolv_dir.resolve() if args.smolv_dir else None
    if smolv_dir is None:
        unavailable_methods.append({"method": "smolv_*", "reason": "--smolv-dir not supplied"})
    else:
        lock = json.loads(args.lock.read_text())
        want = next(s["revision"] for s in lock["sources"] if s["name"] == "smol-v")
        have = mmm.git_head(smolv_dir)
        if have != want or mmm.git_dirty(smolv_dir):
            raise SystemExit(f"smol-v checkout {smolv_dir} at {have} (dirty="
                             f"{mmm.git_dirty(smolv_dir)}), lock expects {want}")
        exe, smolv_meta = build_smolv(smolv_dir, out / "tools")
        files = [str(cache / "spv" / f"{d}.spv") for d in sorted(uniq)]
        tmpdir = out / "tools" / "smolv_out"
        tmpdir.mkdir(exist_ok=True)
        for i in range(0, len(files), 128):
            p = subprocess.run([str(exe), "enc", str(tmpdir), *files[i:i + 128]],
                               capture_output=True, text=True, check=True)
            for line in p.stdout.splitlines():
                st, a, path = line.split("\t")
                d = Path(path).name[:-4]
                smolv_res[d] = {"ok": st == "OK", "bytes": int(a) if st == "OK" else None,
                                "error": None if st == "OK" else a}
                if st == "OK":
                    os.replace(tmpdir / f"{d}.spv.smolv", cache / "smolv" / f"{d}.smolv")
                else:
                    stale = cache / "smolv" / f"{d}.smolv"
                    if stale.exists():
                        stale.unlink()
        # Independent decode check through the dec entry point on every blob.
        dec_fail = 0
        for d, r in smolv_res.items():
            if r["ok"]:
                o = tmpdir / "dec.spv"
                ok = subprocess.run([str(exe), "dec", str(cache / "smolv" / f"{d}.smolv"),
                                     str(o)]).returncode == 0 and \
                    o.read_bytes() == (cache / "spv" / f"{d}.spv").read_bytes()
                if not ok:
                    r.update(ok=False, error="dec mismatch")
                    (cache / "smolv" / f"{d}.smolv").unlink()
                    dec_fail += 1
        print(f"smol-v: {sum(r['ok'] for r in smolv_res.values())}/{len(uniq)} verified", flush=True)

    # Per-module table.
    per_module = []
    arx_by = {r["sha256"]: r for r in arx_res}
    for digest in sorted(uniq):
        raw = (cache / "spv" / f"{digest}.spv").read_bytes()
        a = arx_by[digest]
        s = smolv_res.get(digest, {})
        per_module.append({
            "sha256": digest, "bytes": len(raw), "arx2_bytes": a.get("bytes"),
            "arx2_mode": a.get("mode"), "arx2_header_bytes": a.get("header_bytes"),
            "arx2_error": a.get("error"), "smolv_bytes": s.get("bytes"),
            "smolv_error": s.get("error") if s else "not run",
            "zstd19_bytes": len(zstd_c(raw)), "xz9e_bytes": len(xz_c(raw))})

    # Packs.
    packs = [p for p in man["packs"]
             if not args.pack_filter or args.pack_filter in p["id"]]
    jobs = [{"id": p["id"], "shas": [mods[m]["sha256"] for m in p["module_ids"]],
             "ids": p["module_ids"]} for p in packs]
    with ProcessPoolExecutor(args.jobs, initializer=_init_worker, initargs=(str(cache),)) as ex:
        pack_res = list(ex.map(eval_pack, jobs))
    pack_rows = [r for pr in pack_res for r in pr["rows"]]
    pinfo = {p["id"]: p for p in packs}
    for r in pack_rows:
        p = pinfo[r["pack_id"]]
        r.update(tier=p["tier"], representative=p["representative"],
                 collection_id=p["collection_id"], origin_id=p["origin_id"],
                 fold_id=p["fold_id"])
    print(f"packs: {len(packs)} evaluated", flush=True)

    # Shared-header wrapper: decode every reconstructed blob with the CLI.
    recon_checks = []
    for r in pack_rows:
        if r["method"] == "arx2_shared_header" and r["verified"]:
            p = pinfo[r["pack_id"]]
            for mid, bsha in zip(p["module_ids"], r["reconstructed_blob_sha256"]):
                recon_checks.append((mid, bsha))
    distinct = {}
    for mid, bsha in recon_checks:
        distinct.setdefault(bsha, mid)
    dec_jobs = []
    for bsha, mid in distinct.items():
        d = mods[mid]["sha256"]
        blob = (cache / "arx2" / f"{d}.arx2").read_bytes()
        if hashlib.sha256(blob).hexdigest() != bsha:
            raise SystemExit(f"reconstructed blob hash differs from standalone for {mid}")
        dec_jobs.append((str(args.arixv), blob, (cache / "spv" / f"{d}.spv").read_bytes()))
    with ProcessPoolExecutor(args.jobs) as ex:
        dec_ok = list(ex.map(_arx2_decode_check, dec_jobs, chunksize=16))
    wrapper_decode_ok = all(dec_ok)
    for r in pack_rows:
        r.pop("reconstructed_blob_sha256", None)
        if r["method"] == "arx2_shared_header":
            r["verified"] = bool(r["verified"] and wrapper_decode_ok)
    print(f"wrapper: {sum(dec_ok)}/{len(dec_ok)} distinct reconstructed blobs decoded exactly "
          f"({len(recon_checks)} member reconstructions)", flush=True)

    # Marginal curves.
    curve_rows = []
    if not args.skip_curves:
        cjobs = [j for j, p in zip(jobs, packs) if len(j["shas"]) >= 2]
        cjobs.sort(key=lambda j: -len(j["shas"]))
        with ProcessPoolExecutor(args.jobs, initializer=_init_worker,
                                 initargs=(str(cache),)) as ex:
            for cr in ex.map(eval_curve, cjobs):
                curve_rows.extend(cr["rows"])
        print(f"curves: {len(cjobs)} packs", flush=True)

    # Conditional costs.
    case_rows = []
    if not args.skip_cases:
        groups: dict[tuple, list] = defaultdict(list)
        for c in man["prefix_cases"]:
            base = {"case_id": c["id"], "target_id": c["target_id"], "tier": c["tier"],
                    "availability": c["availability"], "reason": c["reason"], "seed": c["seed"],
                    "target_collection": mods[c["target_id"]]["collection_id"],
                    "target_origin": mods[c["target_id"]]["origin_id"],
                    "prefix_stages": sorted({mods[m]["stage"] or "none" for m in c["prefix_ids"]}),
                    "prefix_collections": sorted({mods[m]["collection_id"]
                                                  for m in c["prefix_ids"]})}
            if c["availability"] != "available":
                case_rows.append(base)
                continue
            key = tuple(mods[m]["sha256"] for m in c["prefix_ids"])
            groups[key].append({"target_sha": mods[c["target_id"]]["sha256"], "row": base})
        cjobs = [{"prefix_shas": list(k), "cases": v} for k, v in groups.items()]
        with ProcessPoolExecutor(args.jobs, initializer=_init_worker,
                                 initargs=(str(cache),)) as ex:
            for res in ex.map(eval_cases, cjobs, chunksize=8):
                case_rows.extend(res)
        case_rows.sort(key=lambda r: r["case_id"])
        print(f"cases: {len(case_rows)} rows ({len(cjobs)} distinct prefixes)", flush=True)

    # Verification summary.
    by_method = defaultdict(Counter)
    for r in pack_rows:
        st = "verified" if r["verified"] else ("unavailable" if r["total_bytes"] is None
                                               else "FAILED")
        by_method[r["method"]][st] += 1
    failed = [r for r in pack_rows if r["total_bytes"] is not None and not r["verified"]]
    required_unavail = [r for r in pack_rows if r["total_bytes"] is None
                        and r["role"] == "required"]
    case_fail = [r for r in case_rows if r.get("availability") == "available"
                 and not r.get("verified")]
    identity = (not failed and not arx_fail and wrapper_decode_ok and not case_fail)
    cross = cross_check_cli((cache / "spv" / f"{sorted(uniq)[0]}.spv").read_bytes(), out)
    smolv_fail = sorted(d for d, r in smolv_res.items() if not r["ok"])
    notes = (f"Verified exact reconstruction for every evaluated pack x method "
             f"({sum(c['verified'] for c in by_method.values())} rows): MMF/AXS0/dedup/tar "
             f"parsing back to member bytes, zstd/xz decompression, ARX2 blobs decoded by "
             f"unchanged arixv CLI ({len(arx_res) - len(arx_fail)}/{len(arx_res)} modules; "
             f"{sum(dec_ok)} distinct wrapper-reconstructed blobs re-decoded), smol-v blobs "
             f"decode-verified ({len(uniq) - len(smolv_fail)}/{len(uniq)}). "
             f"Conditional cases decompression-verified. "
             f"Unavailable: {len(required_unavail)} required pack x method rows "
             f"(smol-v unsupported members: {len(smolv_fail)}); optional tools: "
             f"{'none' if not unavailable_methods else unavailable_methods}. "
             f"Extras (classification evidence, counts, unavailable comparisons) in "
             f"manifest_extras.json / group_counts.json next to the manifest.")
    verification = {
        "manifest_sha256": man_sha,
        "baseline_identity_verified": identity,
        "by_method": {k: dict(v) for k, v in by_method.items()},
        "arx2_failures": arx_fail,
        "smolv_failures": smolv_fail,
        "failed_rows": failed,
        "required_unavailable_rows": len(required_unavail),
        "conditional_case_failures": [r["case_id"] for r in case_fail],
        "wrapper_reconstructions": len(recon_checks),
        "wrapper_distinct_blob_decodes": len(dec_ok),
        "wrapper_decode_ok": wrapper_decode_ok,
        "unavailable_methods": unavailable_methods,
        "cli_cross_check": cross,
        "notes": notes,
    }
    config = {
        "manifest": str(args.manifest), "manifest_sha256": man_sha,
        "argv": sys.argv, "jobs": args.jobs, "elapsed_s": round(time.time() - t0, 1),
        "tools": tool_versions(args.arixv, smolv_meta, smolv_dir),
        "framing": {
            "MMF": "compact_u32(count) || compact_u32(len_i)* || payload_i*; positional "
                   "identity in manifest order; no key bytes charged to any method",
            "AXS0": "magic || compact(count) || (mode u8 || compact(size_field) || "
                    "compact(payload_len))* || payloads*",
            "dedup": "compact(count) || (0 || compact(len) || blob | k)*",
            "tar": "USTAR canonical (%06d names, mtime/uid/gid 0, mode 0644, empty owners)",
            "conditional_zstd": "target compressed alone with ZstdDict(raw concat of prefix "
                                "originals, is_raw=True), zstd19 settings; dictionary content "
                                "not charged (diagnostic)",
            "conditional_xz": "len(xz(MMF(prefix+[target]))) - len(xz(MMF(prefix))), both "
                              "complete finalized xz -9e streams",
        },
    }
    out.mkdir(parents=True, exist_ok=True)
    write_csv(out / "per_module.csv", per_module)
    write_csv(out / "pack_baselines.csv", pack_rows,
              ["pack_id", "tier", "representative", "collection_id", "origin_id", "fold_id",
               "method", "role", "members", "original_bytes", "total_bytes", "ratio",
               "payload_bytes", "metadata_bytes", "metadata_compressed", "arx2_header_bytes",
               "zstd_window", "stream_bytes", "tar_bytes", "verified", "note"])
    mmm.write_json_atomic(out / "pack_baselines.json", pack_rows)
    if curve_rows:
        write_csv(out / "marginal_curves.csv", curve_rows,
                  ["pack_id", "method", "k", "module_id", "module_bytes",
                   "cumulative_original_bytes", "cumulative_bytes", "marginal_bytes"])
    if case_rows:
        write_csv(out / "conditional_costs.csv", case_rows,
                  ["case_id", "target_id", "target_collection", "target_origin", "tier",
                   "availability", "reason", "seed", "prefix_members", "prefix_bytes",
                   "prefix_stages", "prefix_collections", "target_bytes",
                   "arx2_standalone_target_bytes", "zstd_rawdict_target_bytes",
                   "xz_prefix_bytes", "xz_prefix_plus_target_bytes",
                   "xz_conditional_target_bytes", "verified"])
    mmm.write_json_atomic(out / "run_config.json", config)
    mmm.write_json_atomic(out / "baseline_verification.json", verification)
    if not args.pack_filter:
        write_report(out, args.manifest)
    print(f"baseline_identity_verified={identity} elapsed={time.time() - t0:.0f}s")
    print(f"results in {out}")
    return 0 if identity else 1


# --------------------------------------------------------------------------
# Report (reads only the raw outputs above)
# --------------------------------------------------------------------------

def _read_csv(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def _num(v):
    if v in (None, "", "None"):
        return None
    try:
        return int(v)
    except ValueError:
        return float(v)


def _med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def _fmt(v, nd=3):
    if v is None:
        return "n/a"
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


def _table(header: list[str], rows: list[list]) -> str:
    out = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    out += ["| " + " | ".join(_fmt(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


REPORT_METHODS = ["arx2_bare_sum", "arx2_framed", "arx2_shared_header", "arx2_dedup",
                  "arx2_solid_zstd19", "orig_perfile_zstd19", "orig_solid_zstd19",
                  "smolv_solid_zstd19", "orig_tar_zstd19", "smolv_tar_zstd19",
                  "orig_solid_zstd22w27", "orig_solid_xz9e", "smolv_solid_zstd22w27",
                  "smolv_solid_xz9e"]


def paired_differences(case_rows: list[dict]) -> list[dict]:
    """Per target and anchor: related-tier cost minus none / unrelated / matched controls."""
    by_id = {r["case_id"]: r for r in case_rows}
    out = []
    for r in case_rows:
        cid = r["case_id"]
        parts = cid.split("|")
        # anchors: pc|<target>|sp0, |vf0, |sc0 (available only)
        if len(parts) != 3 or r["availability"] != "available" or r["tier"] == "none":
            continue
        target, anchor = parts[1], parts[2]
        none = by_id.get(f"pc|{target}|none")
        if not none:
            continue
        for metric in ("zstd_rawdict_target_bytes", "xz_conditional_target_bytes"):
            rel = _num(r[metric])
            base = _num(none[metric])
            row = {"target_id": target, "anchor": cid, "tier": r["tier"], "metric": metric,
                   "target_collection": r["target_collection"],
                   "target_origin": r["target_origin"], "target_bytes": _num(r["target_bytes"]),
                   "prefix_bytes": _num(r["prefix_bytes"]), "related_bytes": rel,
                   "none_bytes": base, "related_minus_none": rel - base}
            for label, tier in (("c", "same_collection_matched"), ("u", "unrelated"),
                                ("t", "unrelated_toolchain_matched")):
                vals = [_num(by_id[f"{cid}|{label}{k}"][metric])
                        for k in range(8) if f"{cid}|{label}{k}" in by_id
                        and by_id[f"{cid}|{label}{k}"]["availability"] == "available"]
                n_unavail = sum(1 for k in range(8) if f"{cid}|{label}{k}" in by_id
                                and by_id[f"{cid}|{label}{k}"]["availability"] != "available")
                row[f"{tier}_samples"] = len(vals)
                row[f"{tier}_unavailable"] = n_unavail
                row[f"{tier}_mean"] = statistics.mean(vals) if vals else None
                row[f"{tier}_min"] = min(vals) if vals else None
                row[f"{tier}_max"] = max(vals) if vals else None
                row[f"related_minus_{tier}_mean"] = (rel - statistics.mean(vals)) if vals else None
            out.append(row)
    return out


def write_report(out: Path, manifest_path: Path) -> None:
    man = json.loads(manifest_path.read_text())
    extras_path = manifest_path.parent / "manifest_extras.json"
    extras = json.loads(extras_path.read_text()) if extras_path.exists() else {}
    counts = extras.get("counts", {})
    ver = json.loads((out / "baseline_verification.json").read_text())
    cfg = json.loads((out / "run_config.json").read_text())
    packs = {p["id"]: p for p in man["packs"]}
    mods = man["modules"]
    rows = json.loads((out / "pack_baselines.json").read_text())
    by_pack: dict[str, dict] = defaultdict(dict)
    for r in rows:
        by_pack[r["pack_id"]][r["method"]] = r
    cases = _read_csv(out / "conditional_costs.csv")
    curves = _read_csv(out / "marginal_curves.csv")
    per_mod = {r["sha256"]: r for r in _read_csv(out / "per_module.csv")}
    pdiff = paired_differences(cases) if cases else []
    if pdiff:
        write_csv(out / "paired_differences.csv", pdiff)

    L = []
    L.append("# Gate 1a multi-module baselines: initial report\n")
    L.append("Generated by `scripts/bench/multi_module_baselines.py`. Size-only; no timing. "
             "Ratios are serialized bytes / original logical SPIR-V bytes (lower is better). "
             "All rows below were reconstructed exactly unless marked otherwise.\n")
    L.append(f"- manifest sha256 `{ver['manifest_sha256']}`")
    L.append(f"- baseline_identity_verified: **{ver['baseline_identity_verified']}**")
    t = cfg["tools"]
    L.append(f"- arixv commit `{t['arixv_source_commit'][:12]}` (codec sources modified: "
             f"{t['arixv_codec_sources_modified']}), libzstd {t['libzstd']}, liblzma "
             f"{t['liblzma']}, smol-v {str(t['smolv'].get('revision'))[:12]}")
    L.append(f"- zstd settings {json.dumps(t['zstd_settings'])}; xz {json.dumps(t['xz_settings'])}")
    L.append(f"- CLI cross-check (one module): {json.dumps(ver['cli_cross_check'])}")
    L.append(f"- smol-v unsupported modules: {len(ver['smolv_failures'])}; failed rows: "
             f"{len(ver['failed_rows'])}; required rows unavailable: "
             f"{ver['required_unavailable_rows']}\n")

    # 1. Counts published before interpretation.
    L.append("## 1. Corpus groups (frozen before probes)\n")
    if counts:
        tot = counts["total"]
        L.append(f"Total: {tot['logical_members']} logical entries, {tot['unique_members']} "
                 f"unique contents, {tot['duplicate_entries']} duplicate entries, "
                 f"{tot['logical_bytes']} logical / {tot['unique_bytes']} unique bytes.\n")
        L.append("### By rule (exhaustive, includes empty rules)\n")
        L.append(_table(["rule", "logical", "unique", "dups", "logical B", "unique B",
                         "stages", "debug"],
                        [[k, c["logical_members"], c["unique_members"], c["duplicate_entries"],
                          c["logical_bytes"], c["unique_bytes"],
                          ", ".join(f"{a}:{b}" for a, b in c["stages"].items()),
                          ", ".join(f"{a}:{b}" for a, b in c["debug_classes"].items())]
                         for k, c in counts["by_rule"].items()]))
        L.append("\n### By origin and by fold\n")
        L.append(_table(["origin", "logical", "unique", "dups", "unique B"],
                        [[k, c["logical_members"], c["unique_members"], c["duplicate_entries"],
                          c["unique_bytes"]] for k, c in counts["by_origin"].items()]))
        L.append("")
        L.append(_table(["fold", "logical", "unique", "unique B"],
                        [[k, c["logical_members"], c["unique_members"], c["unique_bytes"]]
                         for k, c in counts["by_fold"].items()]))
        L.append(f"\nPacks by tier: {json.dumps(counts['packs_by_tier'])}\n")
        L.append("### Prefix-case availability by tier\n")
        L.append(_table(["tier/availability", "cases"],
                        [[k, v] for k, v in extras.get("prefix_case_availability", {}).items()]))
        L.append("\n### Unavailable comparisons\n")
        for u in extras.get("known_unavailable", []):
            L.append(f"- {u['comparison']}: {u['reason']}")
        L.append("")

    # 2. Baselines.
    def agg(pids: list[str], methods: list[str]) -> list[list]:
        res = []
        common = [p for p in pids if all(by_pack[p].get(m, {}).get("total_bytes") is not None
                                         for m in methods)]
        for m in methods:
            ratios = [by_pack[p][m]["ratio"] for p in common]
            tot = sum(by_pack[p][m]["total_bytes"] for p in common)
            orig = sum(by_pack[p][m]["original_bytes"] for p in common)
            res.append([m, len(common), _med(ratios), tot / orig if orig else None, tot])
        return res, len(pids) - len(common)

    L.append("## 2. Full-set baselines\n")
    L.append("Medians are over packs; byte-weighted ratio = sum(total)/sum(original). "
             "Each table uses the common subset of packs where every listed method ran "
             "(smol-v cannot encode three malformed Vulkan-Samples HLSL entries).\n")
    groups = defaultdict(list)
    for pid, p in packs.items():
        if p["tier"] == "same_collection" and not pid.endswith("#unique"):
            cat = extras.get("collections", {}).get(p["collection_id"], {}).get("category", "?")
            groups[("same_collection", cat, p["origin_id"] or "(unknown)")].append(pid)
        else:
            groups[(p["tier"] + ("#unique" if pid.endswith("#unique") else ""), "-", "-")].append(pid)
    for key in sorted(groups):
        pids = groups[key]
        tbl, dropped = agg(pids, REPORT_METHODS)
        L.append(f"### {key[0]} / {key[1]} / origin {key[2]} ({len(pids)} packs, "
                 f"{dropped} outside common subset)\n")
        L.append(_table(["method", "packs", "median ratio", "byte-weighted", "total B"], tbl))
        L.append("")

    # Framing/header/dedup attribution on representative packs.
    L.append("### Framing, header and duplicate attribution (all logical collection packs)\n")
    attr = []
    for pid, p in sorted(packs.items()):
        if p["tier"] != "same_collection" or pid.endswith("#unique"):
            continue
        b = by_pack[pid]
        bare = b["arx2_bare_sum"]["total_bytes"]
        best_generic = min((b[m]["total_bytes"], m) for m in
                           ("orig_solid_zstd19", "smolv_solid_zstd19", "orig_solid_xz9e",
                            "smolv_solid_xz9e", "orig_solid_zstd22w27", "smolv_solid_zstd22w27")
                           if b[m]["total_bytes"] is not None)
        attr.append([pid, p["representative"], b["arx2_bare_sum"]["members"],
                     b["arx2_bare_sum"]["original_bytes"], bare,
                     b["arx2_framed"]["total_bytes"] - bare,
                     b["arx2_shared_header"]["total_bytes"] - bare,
                     b["arx2_dedup"]["total_bytes"] - bare,
                     best_generic[0], best_generic[1],
                     best_generic[0] / bare])
    L.append(_table(["pack", "rep", "n", "orig B", "ARX2 bare B", "framed-bare",
                     "wrapper-bare", "dedup-bare", "best solid generic B", "method",
                     "generic/bare"], attr))
    L.append("")

    # 3. Conditional costs.
    if cases:
        L.append("## 3. Generic conditional target costs (diagnostic, not deployable)\n")
        L.append("Prefix bytes are never charged here. zstd: target compressed with the raw "
                 "prefix as a raw-content dictionary. xz: finalized "
                 "size(xz(MMF(prefix+target))) - size(xz(MMF(prefix))). Ratios are "
                 "related-tier cost / no-prefix cost per target (median over targets).\n")
        av = Counter((r["tier"], r["availability"]) for r in cases)
        L.append(_table(["tier", "available", "unavailable"],
                        [[t, av[(t, "available")], av[(t, "unavailable")]]
                         for t in sorted({k[0] for k in av})]))
        L.append("")
        for metric in ("zstd_rawdict_target_bytes", "xz_conditional_target_bytes"):
            L.append(f"### {metric}\n")
            trs = []
            for tier_anchor in ("sp", "vf", "sc"):
                sub = [d for d in pdiff if d["metric"] == metric
                       and d["anchor"].split("|")[2].startswith(tier_anchor)
                       and (tier_anchor != "sc" or d["anchor"].endswith("sc0"))]
                if not sub:
                    continue
                for grp in sorted({d["target_origin"] or "(unknown)" for d in sub}) + ["ALL"]:
                    s2 = [d for d in sub if grp == "ALL" or (d["target_origin"] or "(unknown)") == grp]
                    r_none = [d["related_bytes"] / d["none_bytes"] for d in s2 if d["none_bytes"]]
                    def rel(tag):
                        v = [d["related_bytes"] / d[f"{tag}_mean"] for d in s2
                             if d.get(f"{tag}_mean")]
                        return _med(v), len(v)
                    u, nu = rel("unrelated")
                    tm, nt = rel("unrelated_toolchain_matched")
                    sc, ns = rel("same_collection_matched")
                    trs.append([tier_anchor, grp, len(s2), _med(r_none), u, nu, tm, nt, sc, ns,
                                _med([d["related_minus_none"] for d in s2])])
            L.append(_table(["tier", "target origin", "targets", "rel/none", "rel/unrel",
                             "n", "rel/tc-matched", "n", "rel/sc-matched", "n",
                             "median rel-none B"], trs))
            L.append("")
        # Sample spread for same-collection tier.
        spread = []
        by_target = defaultdict(list)
        for r in cases:
            p = r["case_id"].split("|")
            if len(p) == 3 and p[2].startswith("sc") and r["availability"] == "available":
                by_target[p[1]].append(_num(r["xz_conditional_target_bytes"]))
        rngs = [max(v) - min(v) for v in by_target.values() if len(v) >= 2]
        L.append(f"Same-collection 32 KiB tier: {len(by_target)} targets with samples; "
                 f"{len(rngs)} with >=2 samples; median xz spread across samples "
                 f"{_fmt(_med(rngs))} B.\n")

    # 4. Marginal curves.
    if curves:
        L.append("## 4. Incremental cost curves (fully finalized prefix encodes)\n")
        L.append("Ratio of solid cumulative bytes at k members to the sum of independent "
                 "per-file zstd-19 frames for the same k members (median over logical "
                 "collection packs with at least k members; frozen manifest order).\n")
        cur = defaultdict(dict)
        for r in curves:
            cur[(r["pack_id"], r["method"])][int(r["k"])] = r
        ks = [1, 2, 4, 8, 16, 32, 64, 128]
        trs = []
        for method in ("zstd19", "xz9e"):
            row = [method]
            for k in ks:
                vals = []
                for (pid, m), pts in cur.items():
                    p = packs[pid]
                    if m != method or p["tier"] != "same_collection" or pid.endswith("#unique"):
                        continue
                    if k not in pts:
                        continue
                    shas = [mods[x]["sha256"] for x in p["module_ids"][:k]]
                    indep = sum(int(per_mod[s]["zstd19_bytes"]) for s in shas)
                    vals.append(int(pts[k]["cumulative_bytes"]) / indep)
                row.append(f"{_fmt(_med(vals))} ({len(vals)})")
            trs.append(row)
        L.append(_table(["method"] + [f"k={k}" for k in ks], trs))
        L.append("")
    L.append("## Raw outputs\n")
    for f in ("pack_baselines.csv", "pack_baselines.json", "per_module.csv",
              "marginal_curves.csv", "conditional_costs.csv", "paired_differences.csv",
              "run_config.json", "baseline_verification.json"):
        if (out / f).exists():
            L.append(f"- `{out / f}`")
    (out / "report.md").write_text("\n".join(L) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
