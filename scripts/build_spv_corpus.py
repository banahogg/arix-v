#!/usr/bin/env python3
"""Build the ignored SPIR-V benchmark corpus from pinned local checkouts."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
LOCKFILE = ROOT / "deps" / "benchmark-sources.lock.json"


@dataclass(frozen=True)
class Source:
    name: str
    root: Path
    url: str
    revision: str
    selection_path_prefix: str
    cap: int | None = None
    exclude_substrings: tuple[str, ...] = ()


def load_sources(deps_dir: Path) -> tuple[Source, ...]:
    lock = json.loads(LOCKFILE.read_text(encoding="utf-8"))
    if lock.get("schema_version") != 1:
        raise SystemExit(f"unsupported lockfile schema: {LOCKFILE}")
    excluded = {"smol-v": ("tests/spirv-dumps/synthetic/invalid-",)}
    caps = {"spirv-database": 400}
    return tuple(
        Source(
            name=item["name"],
            root=deps_dir / item["directory"],
            url=item["url"],
            revision=item["revision"],
            selection_path_prefix=item.get("selection_path_prefix", item["directory"]),
            cap=caps.get(item["name"]),
            exclude_substrings=excluded.get(item["name"], ()),
        )
        for item in lock["sources"]
    )


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def candidate_paths(source: Source) -> list[Path]:
    paths = sorted(source.root.rglob("*.spv"))
    if source.exclude_substrings:
        paths = [
            path
            for path in paths
            if not any(marker in path.relative_to(source.root).as_posix()
                       for marker in source.exclude_substrings)
        ]
    if source.cap is None or len(paths) <= source.cap:
        return paths
    return sorted(
        paths,
        key=lambda path: (
            sha256_text(
                f"{source.selection_path_prefix}/"
                f"{path.relative_to(source.root).as_posix()}"
            ),
            f"{source.selection_path_prefix}/{path.relative_to(source.root).as_posix()}",
        ),
    )[: source.cap]


def verify_revision(source: Source) -> None:
    try:
        actual = subprocess.check_output(
            ["git", "-C", str(source.root), "rev-parse", "HEAD"], text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(
            f"cannot identify benchmark dependency revision: {source.root} ({error})"
        ) from error
    if actual != source.revision:
        raise SystemExit(
            f"benchmark dependency is not at its locked revision: {source.root}\n"
            f"  expected {source.revision}\n  actual   {actual}\n"
            "rerun: python3 scripts/fetch_pinned_repos.py --force"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a SPIR-V reference corpus.")
    parser.add_argument(
        "--deps",
        type=Path,
        default=ROOT / "benchmark-deps",
        help="directory created by fetch_pinned_repos.py (default: %(default)s)",
    )
    parser.add_argument("--out", default="corpus/spv", help="output directory for canonical SPV files")
    parser.add_argument(
        "--manifest", default="corpus/manifest.tsv", help="TSV manifest path"
    )
    parser.add_argument(
        "--sources", default="corpus/sources.tsv", help="TSV source inventory path"
    )
    parser.add_argument(
        "--clean", action="store_true", help="remove an existing output directory before building"
    )
    args = parser.parse_args()

    sources = load_sources(args.deps)
    missing = [source.root for source in sources if not source.root.is_dir()]
    if missing:
        locations = "\n  ".join(map(str, missing))
        raise SystemExit(
            "missing benchmark dependencies:\n  " + locations
            + "\nrun: python3 scripts/fetch_pinned_repos.py"
        )
    for source in sources:
        verify_revision(source)

    out_dir = Path(args.out)
    manifest_path = Path(args.manifest)
    sources_path = Path(args.sources)
    duplicates_path = manifest_path.parent / "duplicates.tsv"
    if out_dir.exists() and any(out_dir.iterdir()):
        if not args.clean:
            raise SystemExit(f"output directory is not empty: {out_dir} (rerun with --clean)")
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)

    seen_hashes: dict[str, Path] = {}
    manifest_rows: list[tuple[str, int, str, str, str]] = []
    duplicate_rows: list[tuple[str, int, str, str, str]] = []
    source_rows: list[tuple[str, str, str, int, int]] = []

    for source in sources:
        paths = candidate_paths(source)
        selected = 0
        for path in paths:
            data = path.read_bytes()
            digest = sha256_bytes(data)
            relative_path = path.relative_to(source.root).as_posix()
            if digest in seen_hashes:
                duplicate_rows.append(
                    (digest, len(data), source.name, relative_path, seen_hashes[digest].name)
                )
                continue

            canonical = out_dir / f"{digest}.spv"
            canonical.write_bytes(data)
            seen_hashes[digest] = canonical
            manifest_rows.append((digest, len(data), source.name, relative_path, canonical.name))
            selected += 1

        source_rows.append((source.name, source.url, source.revision, len(paths), selected))

    with manifest_path.open("w", encoding="utf-8") as f:
        f.write("sha256\tbytes\tsource\tinput_path\tcanonical_name\n")
        for row in manifest_rows:
            f.write("\t".join(map(str, row)) + "\n")

    with sources_path.open("w", encoding="utf-8") as f:
        f.write("source\tgit_url\trevision\tcandidates\tunique_selected\n")
        for row in source_rows:
            f.write("\t".join(map(str, row)) + "\n")

    with duplicates_path.open("w", encoding="utf-8") as f:
        f.write("sha256\tbytes\tsource\tinput_path\tcanonical_name\n")
        for row in duplicate_rows:
            f.write("\t".join(map(str, row)) + "\n")

    print(f"selected {len(manifest_rows)} unique modules into {out_dir}")
    for source_name, url, revision, candidates, selected in source_rows:
        print(f"  {source_name}: {selected}/{candidates} from {url} @ {revision}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
