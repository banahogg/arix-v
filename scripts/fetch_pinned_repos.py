#!/usr/bin/env python3
"""Fetch repositories at the exact revisions recorded in a source lockfile."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOCKFILE = ROOT / "deps" / "benchmark-sources.lock.json"


def run(*args: str) -> None:
    print("+", " ".join(args))
    subprocess.run(args, check=True)


def checked_out_revision(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fetch repositories pinned by a source lockfile."
    )
    parser.add_argument(
        "--lock",
        type=Path,
        default=DEFAULT_LOCKFILE,
        help="source lockfile (default: %(default)s)",
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=ROOT / "benchmark-deps",
        help="directory for fetched repositories (default: %(default)s)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="check out the locked revision when a destination checkout differs",
    )
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    if lock.get("schema_version") != 1:
        raise SystemExit(f"unsupported lockfile schema: {args.lock}")

    args.dest.mkdir(parents=True, exist_ok=True)
    for source in lock["sources"]:
        dest = args.dest / source["directory"]
        revision = source["revision"]
        if dest.exists():
            if not (dest / ".git").exists():
                raise SystemExit(f"destination exists but is not a Git checkout: {dest}")
            current = checked_out_revision(dest)
            if current == revision:
                print(f"{source['name']}: already at {revision}")
                continue
            if not args.force:
                raise SystemExit(
                    f"{dest} is at {current}, not locked revision {revision}; "
                    "rerun with --force to replace it"
                )
            run("git", "-C", str(dest), "fetch", "--tags", "origin", revision)
        else:
            run("git", "clone", "--no-checkout", source["url"], str(dest))
        run("git", "-C", str(dest), "checkout", "--detach", revision)
        if checked_out_revision(dest) != revision:
            raise SystemExit(f"failed to check out locked revision for {source['name']}")
        print(f"{source['name']}: {revision}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
