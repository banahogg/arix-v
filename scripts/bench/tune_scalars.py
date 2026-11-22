#!/usr/bin/env python3
"""Scalar tuning under the train-split-only tuning rule (see README design notes).

Only scalar constants from src/tuning.h may be tuned. Selection uses the
TRAIN split only (corpus files whose hex name starts with 0-7); the HOLDOUT
split (8-f) stays blind until the final validation report. Candidates are
compiled with -DARIXV_* overrides and scored by total ratio over the split.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "corpus" / "spv"
SOURCES = [
    "tools/bench/bench.cpp", "src/arixv.cpp", "src/arixv_encode.cpp",
    "src/model.cpp", "src/tracker.cpp", "src/operand_cursor.cpp",
    "src/grammar_tables.cpp",
]

# knob -> candidate values (first entry = current default). Rank-list caps
# must stay <= 2^kRankTreeBits = 16 (static_assert in tuning.h).
KNOBS: dict[str, list[int]] = {
    "ARIXV_KPROB_SHIFT": [3, 2, 4, 5],
    "ARIXV_KSYMBOL_RATE_CAP": [5, 4, 6],
    "ARIXV_KBYTE_SYMBOL_RATE_CAP": [7, 6, 8],
    "ARIXV_KRANK_LIST_CAP_IDS": [16, 8, 12],
    "ARIXV_KRANK_LIST_CAP_TYPES": [16, 8, 12],
    "ARIXV_KUINT_HIGH_BITS": [8, 6, 12],
}


def split_files(which: str) -> list[Path]:
    files = sorted(CORPUS.glob("*.spv"))
    train = [f for f in files if f.name[0] in "01234567"]
    holdout = [f for f in files if f.name[0] not in "01234567"]
    return {"train": train, "holdout": holdout, "all": files}[which]


def build(defines: dict[str, int], out: Path) -> None:
    cmd = [
        "c++", "-O2", "-std=c++17", "-DNDEBUG",
        f"-I{ROOT}/include", f"-I{ROOT}/src",
        "-DARIXV_ENCODER=1", "-DARIXV_RESEARCH_TUNING",
    ]
    cmd += [f"-D{k}={v}" for k, v in defines.items()]
    cmd += [str(ROOT / s) for s in SOURCES]
    cmd += ["-o", str(out)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)


def total_ratio(exe: Path, files: list[Path]) -> float:
    total_in = total_out = 0
    for start in range(0, len(files), 200):
        batch = files[start : start + 200]
        proc = subprocess.run([str(exe), *map(str, batch)],
                              capture_output=True, text=True, check=True)
        for line in proc.stdout.splitlines():
            # path, input_bytes, encoded_bytes, encode_ns, decode_ns
            fields = line.split("\t")
            total_in += int(fields[1])
            total_out += int(fields[2])
    return total_out / total_in


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--final", action="store_true",
                        help="validate chosen defaults on the holdout split")
    args = parser.parse_args()

    train = split_files("train")
    print(f"train={len(train)} holdout={len(split_files('holdout'))}")

    chosen: dict[str, int] = {k: v[0] for k, v in KNOBS.items()}
    with tempfile.TemporaryDirectory(prefix="arixv-tune-") as td:
        exe = Path(td) / "bench"
        build(chosen, exe)
        best = total_ratio(exe, train)
        print(f"baseline (defaults): train total={best:.5f}")

        # Coordinate descent, one pass.
        for knob, values in KNOBS.items():
            for v in values[1:]:
                trial = dict(chosen)
                trial[knob] = v
                build(trial, exe)
                r = total_ratio(exe, train)
                marker = ""
                if r < best:
                    best, chosen = r, trial
                    marker = "  <-- new best"
                print(f"{knob}={v}: train total={r:.5f}{marker}")

        print(f"\nchosen: {chosen}\ntrain total={best:.5f}")
        if args.final:
            holdout = split_files("holdout")
            build(chosen, exe)
            print(f"holdout total={total_ratio(exe, holdout):.5f}")
            build({k: v[0] for k, v in KNOBS.items()}, exe)
            print(f"holdout total (defaults)="
                  f"{total_ratio(exe, split_files('holdout')):.5f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
