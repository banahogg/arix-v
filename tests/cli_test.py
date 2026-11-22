#!/usr/bin/env python3
"""Cross-platform integration tests for the arixv CLI."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(cli: str, args: list[str], stdin_bytes: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [cli, *args],
        input=stdin_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def require(cond: bool, msg: str) -> None:
    if not cond:
        print(f"FAIL: {msg}", file=sys.stderr)
        sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--golden", required=True)
    ns = parser.parse_args()

    cli = ns.cli
    golden_dir = Path(ns.golden)
    arx_files = sorted(golden_dir.glob("*.arx2"))
    require(len(arx_files) >= 4, f"expected >=4 golden .arx2 files in {golden_dir}")

    # 1. --version and --help / -h
    r = run(cli, ["--version"])
    require(r.returncode == 0, f"--version rc={r.returncode}")
    out = r.stdout.decode("utf-8")
    require("arixv 1.0.0" in out and "ARX2" in out, f"unexpected --version: {out!r}")

    for flag in ("-h", "--help"):
        r = run(cli, [flag])
        require(r.returncode == 0, f"{flag} rc={r.returncode}")
        require(b"usage:" in r.stdout, f"{flag} missing usage on stdout")

    # 2. Usage errors exit with code 2
    for bad_args in ([], ["unknown-cmd"], ["c"], ["c", "only-one"], ["d", "--force-coded", "a", "b"], ["info"]):
        r = run(cli, bad_args)
        require(r.returncode == 2, f"expected exit 2 for {bad_args}, got {r.returncode}")
        require(len(r.stderr) > 0, f"expected stderr for {bad_args}")

    with tempfile.TemporaryDirectory(prefix="arixv_cli_test_") as tmp:
        tmp_path = Path(tmp)

        # Decode golden SPIR-V vectors to get raw test inputs
        spirv_goldens = [p for p in arx_files if "spirv" in p.name]
        require(len(spirv_goldens) >= 2, "expected >=2 spirv golden .arx2 files")
        spv_files: list[Path] = []
        for arx in spirv_goldens:
            raw_path = tmp_path / f"{arx.stem}.spv"
            rd = run(cli, ["d", str(arx), str(raw_path)])
            require(rd.returncode == 0, f"failed to decode golden {arx}: {rd.stderr!r}")
            spv_files.append(raw_path)

        # 3. Single-file c / d / -v / -q / --force-coded + byte-identity vs golden .arx2
        src0 = spv_files[0]
        src0_bytes = src0.read_bytes()
        out0 = tmp_path / "out0.arixv"
        dec0 = tmp_path / "dec0.spv"

        r = run(cli, ["c", "-v", str(src0), str(out0)])
        require(r.returncode == 0, f"c -v failed: {r.stderr!r}")
        require(len(r.stdout) == 0, "c -v should not write to stdout")
        require(b"mode=spirv" in r.stderr, f"expected mode=spirv in stderr: {r.stderr!r}")
        require(out0.read_bytes() == spirv_goldens[0].read_bytes(), "c output != golden .arx2")

        r = run(cli, ["d", "-v", str(out0), str(dec0)])
        require(r.returncode == 0, f"d -v failed: {r.stderr!r}")
        require(dec0.read_bytes() == src0_bytes, "d output mismatch")

        # -q overrides -v
        r = run(cli, ["c", "-v", "-q", str(src0), str(out0)])
        require(r.returncode == 0 and len(r.stderr) == 0, "c -v -q should be quiet")

        # --force-coded on tiny non-SPIR-V input
        tiny = tmp_path / "tiny.bin"
        tiny.write_bytes(b"hello")
        tiny_fc = tmp_path / "tiny_fc.arixv"
        tiny_dec = tmp_path / "tiny_dec.bin"
        r = run(cli, ["c", "--force-coded", "-v", str(tiny), str(tiny_fc)])
        require(r.returncode == 0, f"c --force-coded failed: {r.stderr!r}")
        require(b"mode=coded" in r.stderr, f"expected mode=coded: {r.stderr!r}")
        r = run(cli, ["d", str(tiny_fc), str(tiny_dec)])
        require(r.returncode == 0 and tiny_dec.read_bytes() == b"hello", "force-coded roundtrip failed")

        # 4. Stdin/stdout '-' pipelines (including 0-byte payload!)
        for payload in (src0_bytes, b"", b"\x00\xff\r\n\x1a\x00binary-test"):
            rc = run(cli, ["c", "-v", "-", "-"], stdin_bytes=payload)
            require(rc.returncode == 0, f"stdin/stdout c failed: {rc.stderr!r}")
            require(len(rc.stderr) > 0, "c -v - - should log summary to stderr")
            rd = run(cli, ["d", "-", "-"], stdin_bytes=rc.stdout)
            require(rd.returncode == 0, f"stdin/stdout d failed: {rd.stderr!r}")
            require(rd.stdout == payload, "stdin/stdout roundtrip mismatch")

            ri = run(cli, ["info", "-"], stdin_bytes=rc.stdout)
            require(ri.returncode == 0, f"info - failed: {ri.stderr!r}")
            require(b"magic=ARX2" in ri.stdout, f"info - missing ARX2: {ri.stdout!r}")

        # 5. Batch compression with --out-dir (creates nested dir automatically)
        batch_dir = tmp_path / "nested" / "batch_out"
        r = run(cli, ["c", "-v", "--out-dir", str(batch_dir), *[str(p) for p in spv_files]])
        require(r.returncode == 0, f"batch --out-dir failed: {r.stderr!r}")
        require(f"{len(spv_files)} files:".encode() in r.stderr, f"missing batch total: {r.stderr!r}")
        for p in spv_files:
            expected_out = batch_dir / f"{p.name}.arixv"
            require(expected_out.exists(), f"missing batch output {expected_out}")
            dec_path = tmp_path / f"{p.name}.dec"
            rd = run(cli, ["d", str(expected_out), str(dec_path)])
            require(rd.returncode == 0 and dec_path.read_bytes() == p.read_bytes(), f"batch decode mismatch for {p}")

        # Duplicate basename rejection in --out-dir
        d1 = tmp_path / "dup1"
        d2 = tmp_path / "dup2"
        d1.mkdir()
        d2.mkdir()
        (d1 / "same.spv").write_bytes(src0_bytes)
        (d2 / "same.spv").write_bytes(src0_bytes)
        r = run(cli, ["c", "--out-dir", str(batch_dir), str(d1 / "same.spv"), str(d2 / "same.spv")])
        require(r.returncode == 1, f"expected exit 1 on duplicate basename, got {r.returncode}")

        # 6. info on files (valid + invalid)
        batch_outputs = [str(batch_dir / f"{p.name}.arixv") for p in spv_files]
        r = run(cli, ["info", *batch_outputs])
        require(r.returncode == 0, f"info failed: {r.stderr!r}")
        lines = [ln for ln in r.stdout.decode("utf-8").splitlines() if ln.strip()]
        require(len(lines) == len(spv_files), f"expected {len(spv_files)} info lines, got {len(lines)}")
        for ln in lines:
            require("magic=ARX2" in ln and "mode=" in ln and "compressed=" in ln and "uncompressed=" in ln and "ratio=" in ln, f"malformed info line: {ln!r}")

        bad_file = tmp_path / "bad.bin"
        bad_file.write_bytes(b"NOT_ARIXV")
        r = run(cli, ["info", str(bad_file)])
        require(r.returncode == 1 and b"bad container" in r.stderr, f"expected bad container on bad_file: {r.stderr!r}")

        empty_file = tmp_path / "empty.bin"
        empty_file.write_bytes(b"")
        r = run(cli, ["d", str(empty_file), str(tmp_path / "empty.out")])
        require(r.returncode == 1 and b"bad container" in r.stderr, f"expected bad container on empty file: {r.stderr!r}")

        # 7. roundtrip subcommand
        r = run(cli, ["roundtrip", str(src0), str(spv_files[1])])
        require(r.returncode == 0, f"roundtrip failed: {r.stderr!r}")
        rt_lines = r.stdout.decode("utf-8").splitlines()
        require(len(rt_lines) == 2 and all("\tOK\t" in ln for ln in rt_lines), f"unexpected roundtrip output: {rt_lines!r}")

    print("cli_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
