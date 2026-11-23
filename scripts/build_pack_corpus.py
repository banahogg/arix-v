#!/usr/bin/env python3
"""Build the ignored shader-pack corpus (corpus/packs/) from pinned sources.

Each set is one real application's shader set, kept whole (not deduplicated
across sets), for multi-module (ARXP) benchmarks. Sources and revisions are
pinned in deps/pack-corpus-sources.lock.json; see docs/benchmark-corpus.md.

  python3 scripts/build_pack_corpus.py [--only wicked,xenia] [--clean]

Writes corpus/packs/<set dir>/..., corpus/packs/<set>.list (module paths in
pack order, relative to the repository root), and corpus/packs/manifest.tsv.
Fetched inputs go to benchmark-deps/packs/. Needs git, zstd (Wicked) and
spirv-as (Skia) on PATH.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCKFILE = ROOT / "deps" / "pack-corpus-sources.lock.json"
SPIRV_MAGIC = b"\x03\x02\x23\x07"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
CHUNK = 1 << 20


def run(*args: str, cwd: Path | None = None) -> None:
    print("+", " ".join(args), flush=True)
    subprocess.run(args, check=True, cwd=cwd)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


# ---------------------------------------------------------------- fetching --


def fetch_git(src: dict, dest: Path) -> Path:
    """Shallow, blob-filtered, sparse checkout of the locked revision."""
    rev = src["revision"]
    if (dest / ".git").exists():
        head = subprocess.run(["git", "-C", str(dest), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        if head == rev:
            print(f"{src['name']}: already at {rev}")
            return dest
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    run("git", "init", "-q", str(dest))
    run("git", "-C", str(dest), "remote", "add", "origin", src["url"])
    run("git", "-C", str(dest), "sparse-checkout", "set", *src["paths"])
    run("git", "-C", str(dest), "fetch", "-q", "--depth", "1",
        "--filter=blob:none", "origin", rev)
    run("git", "-C", str(dest), "checkout", "-q", "--detach", "FETCH_HEAD")
    head = subprocess.check_output(["git", "-C", str(dest), "rev-parse", "HEAD"],
                                   text=True).strip()
    if head != rev:
        raise SystemExit(f"{src['name']}: fetched {head}, expected {rev}")
    return dest


def download(url: str, path: Path, size: int, sha256: str | None) -> None:
    if path.exists() and path.stat().st_size == size and (
            sha256 is None or sha256_file(path) == sha256):
        print(f"{path.name}: already downloaded")
        return
    print(f"+ download {url} ({size:,} bytes)", flush=True)
    tmp = path.with_suffix(path.suffix + ".part")
    h = hashlib.sha256()
    with urllib.request.urlopen(url) as r, tmp.open("wb") as f:
        for block in iter(lambda: r.read(CHUNK), b""):
            h.update(block)
            f.write(block)
    if tmp.stat().st_size != size:
        raise SystemExit(f"{url}: got {tmp.stat().st_size} bytes, expected {size}")
    if sha256 is not None and h.hexdigest() != sha256:
        raise SystemExit(f"{url}: sha256 {h.hexdigest()}, expected {sha256}")
    tmp.replace(path)


def fetch_tar_member(src: dict, path: Path) -> None:
    """Streams a .tar.xz and keeps one member, without storing the archive."""
    if path.exists() and sha256_file(path) == src["member_sha256"]:
        print(f"{path.name}: already extracted")
        return
    print(f"+ stream {src['url']} ({src['size']:,} bytes) for {src['member']}",
          flush=True)
    with urllib.request.urlopen(src["url"]) as r:
        with tarfile.open(fileobj=r, mode="r|xz") as tar:
            for m in tar:
                if m.isfile() and m.name.endswith("/" + src["member"]):
                    data = tar.extractfile(m).read()
                    break
            else:
                raise SystemExit(f"{src['name']}: {src['member']} not in archive")
    if hashlib.sha256(data).hexdigest() != src["member_sha256"]:
        raise SystemExit(f"{src['name']}: {src['member']} checksum mismatch")
    path.write_bytes(data)


# -------------------------------------------------------------- extraction --


def hex_values(body: str) -> list[int]:
    return [int(tok, 0) for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body)]


def extract_wicked(src: dict, deps: Path, out: Path) -> None:
    zpath = deps / "Wicked.Engine.zip"
    download(src["url"], zpath, src["size"], src["sha256"])
    with zipfile.ZipFile(zpath) as z:
        names = [n for n in z.namelist() if n.split("/")[-1] == src["member"]]
        if len(names) != 1:
            raise SystemExit(f"wicked: expected one {src['member']}, found {names}")
        blob = z.read(names[0])
    starts = [m.start() for m in re.finditer(re.escape(ZSTD_MAGIC), blob)]
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(blob)
        # zstd decodes the frame and rejects the trailing bytes up to the
        # next magic; the decoded frame is on stdout either way.
        r = subprocess.run(["zstd", "-dcq"], input=blob[start:end],
                           capture_output=True)
        data = r.stdout
        if not data.startswith(SPIRV_MAGIC):
            continue
        name = f"{i:05d}_{hashlib.sha1(data).hexdigest()[:8]}.spv"
        (out / name).write_bytes(data)


def extract_0ad(src: dict, deps: Path, out: Path) -> None:
    mpath = deps / "mod.zip"
    fetch_tar_member(src, mpath)
    with zipfile.ZipFile(mpath) as z:
        for n in z.namelist():
            if n.startswith("shaders/spirv/") and not n.endswith("/"):
                (out / n.split("/")[-1]).write_bytes(z.read(n))


def extract_copy_spv(tree: Path, out: Path) -> None:
    for p in sorted(tree.rglob("*.spv")):
        dst = out / p.relative_to(tree)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(p.read_bytes())


def extract_c_uint32(tree: Path, out: Path) -> None:
    pat = re.compile(r"const\s+uint32_t\s+(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}")
    for p in sorted(tree.glob("*.h")):
        m = pat.search(p.read_text(encoding="utf-8"))
        if m:
            words = hex_values(m.group(2))
            (out / (p.stem + ".spv")).write_bytes(
                b"".join(w.to_bytes(4, "little") for w in words))


def extract_angle(tree: Path, out: Path) -> None:
    pat = re.compile(r"constexpr\s+uint8_t\s+\w+\s*\[\s*\]\s*=\s*\{([^}]*)\}")
    for p in sorted(tree.glob("*.inc")):
        m = pat.search(p.read_text(encoding="utf-8"))
        if m:
            data = gzip.decompress(bytes(hex_values(m.group(1))))
            (out / (p.name[: -len(".inc")] + ".spv")).write_bytes(data)


def extract_c_bytes(tree: Path, out: Path) -> None:
    pat = re.compile(
        r"const\s+unsigned\s+char\s+(\w+)\s*\[\s*\d*\s*\]\s*=\s*\{([^}]*)\}")
    for p in sorted(tree.glob("*.c")):
        for m in pat.finditer(p.read_text(encoding="utf-8")):
            (out / (m.group(1) + ".spv")).write_bytes(bytes(hex_values(m.group(2))))


def extract_spirv_as(tree: Path, out: Path, extra: list[str]) -> None:
    for p in sorted(tree.rglob("*.asm.*")):
        name = p.relative_to(tree).as_posix().replace("/", "_") + ".spv"
        r = subprocess.run(["spirv-as", *extra, str(p), "-o", str(out / name)],
                           capture_output=True)
        if r.returncode != 0:
            (out / name).unlink(missing_ok=True)


def build_source(src: dict, deps: Path, out: Path) -> None:
    sdir = deps / src["name"]
    sdir.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    kind = src["extract"]
    if kind == "wicked_zstd_carve":
        return extract_wicked(src, sdir, out)
    if kind == "0ad_mod_zip":
        return extract_0ad(src, sdir, out)
    checkout = fetch_git(src, sdir / "repo")
    tree = checkout / src["paths"][0]
    if kind == "spirv_as":
        return extract_spirv_as(tree, out, src.get("spirv_as_args", []))
    {"copy_spv": extract_copy_spv, "c_uint32_arrays": extract_c_uint32,
     "angle_gzip_inc": extract_angle, "c_byte_arrays": extract_c_bytes,
     }[kind](tree, out)


# -------------------------------------------------------------------- main --


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--deps", type=Path, default=ROOT / "benchmark-deps" / "packs")
    ap.add_argument("--out", type=Path, default=ROOT / "corpus" / "packs")
    ap.add_argument("--only", help="comma-separated source names")
    ap.add_argument("--clean", action="store_true",
                    help="replace existing output for the selected sources")
    args = ap.parse_args()

    lock = json.loads(LOCKFILE.read_text(encoding="utf-8"))
    if lock.get("schema_version") != 1:
        raise SystemExit(f"unsupported lockfile schema: {LOCKFILE}")
    sources = {s["name"]: s for s in lock["sources"]}
    only = set(args.only.split(",")) if args.only else set(sources)
    unknown = only - set(sources)
    if unknown:
        raise SystemExit(f"unknown sources: {', '.join(sorted(unknown))}")

    out_root = args.out.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    for name in sorted(only):
        dst = out_root / name
        if dst.exists() and any(dst.iterdir()):
            if not args.clean:
                raise SystemExit(f"output exists: {dst} (rerun with --clean)")
            shutil.rmtree(dst)
        with tempfile.TemporaryDirectory(dir=out_root.parent) as tmp:
            stage = Path(tmp) / name
            build_source(sources[name], args.deps.resolve(), stage)
            dst.parent.mkdir(parents=True, exist_ok=True)
            stage.rename(dst)

    # Lists and manifest cover every set whose source is present.
    rows = []
    for st in lock["sets"]:
        d = out_root / st["dir"]
        if not d.is_dir():
            continue
        files = sorted(p for p in d.rglob("*.spv"))
        rel = [p.relative_to(ROOT).as_posix() if p.is_relative_to(ROOT)
               else str(p) for p in files]
        (out_root / f"{st['name']}.list").write_text(
            "".join(r + "\n" for r in rel), encoding="utf-8")
        total = 0
        for p, r in zip(files, rel):
            data = p.read_bytes()
            total += len(data)
            rows.append((st["name"], r, len(data), hashlib.sha256(data).hexdigest()))
        print(f"  {st['name']}: {len(files)} modules, {total:,} bytes")
    with (out_root / "manifest.tsv").open("w", encoding="utf-8") as f:
        f.write("set\tpath\tbytes\tsha256\n")
        for row in rows:
            f.write("\t".join(map(str, row)) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
