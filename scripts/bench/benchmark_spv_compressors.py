#!/usr/bin/env python3

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from shutil import which

import zstandard as zstd


ROOT = Path(__file__).resolve().parents[2]
CORPUS_DEFAULT = ROOT / "corpus" / "spv"
BENCHMARK_DEPS = ROOT / "benchmark-deps"
BATCH_SIZE = 64
RUNNER_TIMEOUT_SECONDS = 10


@dataclass(frozen=True)
class ToolRunner:
    name: str
    source: str
    include_dirs: tuple[Path, ...]
    source_files: tuple[Path, ...]
    cxx_flags: tuple[str, ...]


@dataclass(frozen=True)
class BenchmarkRow:
    path: Path
    in_bytes: int
    out_bytes: int
    encode_ns: int
    decode_ns: int


@dataclass(frozen=True)
class ToolResult:
    name: str
    rows: list[BenchmarkRow]
    skipped: list[Path]


SMOLV_RUNNER = ToolRunner(
    name="smol-v",
    source=r"""
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "smolv.h"

static std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("failed to open " + path);
  }
  f.seekg(0, std::ios::end);
  const std::streamsize size = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (!f.read(reinterpret_cast<char*>(data.data()), size)) {
    throw std::runtime_error("failed to read " + path);
  }
  return data;
}

static std::string Basename(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

static void WriteOutput(const std::string& outdir, const std::string& input_path, const smolv::ByteArray& data) {
  if (outdir.empty()) {
    return;
  }
  std::string output_path = outdir;
  if (!output_path.empty() && output_path.back() != '/' && output_path.back() != '\\') {
    output_path += '/';
  }
  output_path += Basename(input_path);
  output_path += ".smolv";
  std::ofstream f(output_path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("failed to open output " + output_path);
  }
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!f) {
    throw std::runtime_error("failed to write output " + output_path);
  }
}

static bool ParseDecodeIters(const char* arg, int& iters) {
  const std::string s(arg);
  const std::string prefix = "--decode-iters=";
  if (s.rfind(prefix, 0) != 0) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(s.c_str() + prefix.size(), &end, 10);
  if (end == s.c_str() + prefix.size() || *end != '\0' || value <= 0) {
    throw std::runtime_error("bad --decode-iters value");
  }
  iters = static_cast<int>(value);
  return true;
}

static uint64_t Sample(const std::vector<uint8_t>& data) {
  uint64_t h = data.size();
  if (!data.empty()) {
    h ^= static_cast<uint64_t>(data.front()) << 8;
    h ^= static_cast<uint64_t>(data[data.size() / 2]) << 16;
    h ^= static_cast<uint64_t>(data.back()) << 24;
  }
  return h;
}

int main(int argc, char** argv) {
  std::string outdir;
  int decode_iters = 20;
  int first_file = 1;
  const std::string outdir_prefix = "--outdir=";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind(outdir_prefix, 0) == 0) {
      outdir = arg.substr(outdir_prefix.size());
      first_file = i + 1;
      continue;
    }
    if (ParseDecodeIters(argv[i], decode_iters)) {
      first_file = i + 1;
      continue;
    }
    break;
  }
  if (first_file >= argc) {
    std::cerr << "usage: smolv_bench [--outdir=DIR] [--decode-iters=N] <files...>\n";
    return 2;
  }
  uint64_t checksum = 0;
  for (int i = first_file; i < argc; ++i) {
    const std::string path = argv[i];
    const auto input = ReadFile(path);
    smolv::ByteArray output;
    const auto start = std::chrono::steady_clock::now();
    if (!smolv::Encode(input.data(), input.size(), output)) {
      std::cerr << "encode failed: " << path << "\n";
      return 1;
    }
    const auto end = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    WriteOutput(outdir, path, output);
    std::vector<uint8_t> decoded(input.size());
    if (!smolv::Decode(output.data(), output.size(), decoded.data(), decoded.size()) || decoded != input) {
      std::cerr << "decode verify failed: " << path << "\n";
      return 1;
    }
    int64_t decode_total_ns = 0;
    for (int iter = 0; iter < decode_iters; ++iter) {
      const auto decode_start = std::chrono::steady_clock::now();
      if (!smolv::Decode(output.data(), output.size(), decoded.data(), decoded.size())) {
        std::cerr << "decode failed: " << path << "\n";
        return 1;
      }
      const auto decode_end = std::chrono::steady_clock::now();
      decode_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end - decode_start).count();
      checksum += Sample(decoded) + static_cast<uint64_t>(iter);
    }
    std::cout << path << '\t' << input.size() << '\t' << output.size() << '\t' << ns
              << '\t' << (decode_total_ns / decode_iters) << '\n';
  }
  std::cerr << "checksum=" << checksum << "\n";
  return 0;
}
""",
    include_dirs=(BENCHMARK_DEPS / "smol-v" / "source",),
    source_files=(BENCHMARK_DEPS / "smol-v" / "source" / "smolv.cpp",),
    cxx_flags=("-O2", "-std=c++17", "-DNDEBUG"),
)


YARIV_RUNNER = ToolRunner(
    name="yari-v",
    source=r"""
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "yariv.h"

static std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("failed to open " + path);
  }
  f.seekg(0, std::ios::end);
  const std::streamsize size = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (!f.read(reinterpret_cast<char*>(data.data()), size)) {
    throw std::runtime_error("failed to read " + path);
  }
  return data;
}

static bool ParseDecodeIters(const char* arg, int& iters) {
  const std::string s(arg);
  const std::string prefix = "--decode-iters=";
  if (s.rfind(prefix, 0) != 0) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(s.c_str() + prefix.size(), &end, 10);
  if (end == s.c_str() + prefix.size() || *end != '\0' || value <= 0) {
    throw std::runtime_error("bad --decode-iters value");
  }
  iters = static_cast<int>(value);
  return true;
}

static uint64_t Sample(const std::vector<uint8_t>& data) {
  uint64_t h = data.size();
  if (!data.empty()) {
    h ^= static_cast<uint64_t>(data.front()) << 8;
    h ^= static_cast<uint64_t>(data[data.size() / 2]) << 16;
    h ^= static_cast<uint64_t>(data.back()) << 24;
  }
  return h;
}

int main(int argc, char** argv) {
  int decode_iters = 20;
  int first_file = 1;
  for (int i = 1; i < argc; ++i) {
    if (ParseDecodeIters(argv[i], decode_iters)) {
      first_file = i + 1;
      continue;
    }
    break;
  }
  if (first_file >= argc) {
    std::cerr << "usage: yariv_bench [--decode-iters=N] <files...>\n";
    return 2;
  }
  uint64_t checksum = 0;
  for (int i = first_file; i < argc; ++i) {
    const std::string path = argv[i];
    const auto input = ReadFile(path);
    const size_t encoded_size = yariv_encode_size(yariv_encode_flags_default, input.data(), input.size());
    std::vector<uint8_t> output(encoded_size);
    const auto start = std::chrono::steady_clock::now();
    if (yariv_encode(yariv_encode_flags_default, output.data(), output.size(), input.data(), input.size()) != 0) {
      std::cerr << "encode failed: " << path << "\n";
      return 1;
    }
    const auto end = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const size_t decoded_size = yariv_decode_size(output.data(), output.size());
    std::vector<uint8_t> decoded(decoded_size);
    if (decoded_size != input.size() ||
        yariv_decode(decoded.data(), decoded.size(), output.data(), output.size()) != 0 ||
        decoded != input) {
      std::cerr << "decode verify failed: " << path << "\n";
      return 1;
    }
    int64_t decode_total_ns = 0;
    for (int iter = 0; iter < decode_iters; ++iter) {
      const auto decode_start = std::chrono::steady_clock::now();
      if (yariv_decode(decoded.data(), decoded.size(), output.data(), output.size()) != 0) {
        std::cerr << "decode failed: " << path << "\n";
        return 1;
      }
      const auto decode_end = std::chrono::steady_clock::now();
      decode_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end - decode_start).count();
      checksum += Sample(decoded) + static_cast<uint64_t>(iter);
    }
    std::cout << path << '\t' << input.size() << '\t' << output.size() << '\t' << ns
              << '\t' << (decode_total_ns / decode_iters) << '\n';
  }
  std::cerr << "checksum=" << checksum << "\n";
  return 0;
}
""",
    include_dirs=(BENCHMARK_DEPS / "yari-v",),
    source_files=(),
    cxx_flags=("-O2", "-std=c++17", "-DNDEBUG"),
)


def compiler() -> str:
    for candidate in ("c++", "g++", "clang++"):
        if which(candidate):
            return candidate
    raise SystemExit("no C++ compiler found (expected c++, g++, or clang++)")


def build_runner(tool: ToolRunner, build_dir: Path) -> Path:
    exe = build_dir / tool.name.replace("-", "_")
    src = build_dir / f"{exe.name}.cc"
    src.write_text(tool.source, encoding="utf-8")
    cmd = [compiler(), *tool.cxx_flags]
    for inc in tool.include_dirs:
        cmd.append(f"-I{inc}")
    cmd.extend(map(str, tool.source_files))
    cmd.extend([str(src), "-o", str(exe)])
    subprocess.run(cmd, check=True)
    return exe


def load_corpus(root: Path, limit: int | None) -> list[Path]:
    files = sorted(root.rglob("*.spv"))
    if limit is not None:
        files = files[:limit]
    if not files:
        raise SystemExit(f"no .spv files found under {root}")
    return files


def run_tool(name: str, exe: Path, files: list[Path], extra_args: tuple[str, ...] = ()) -> ToolResult:
    rows: list[BenchmarkRow] = []
    skipped: list[Path] = []

    def run_batch(batch: list[Path]) -> None:
        try:
            proc = subprocess.run(
                [str(exe), *extra_args, *map(str, batch)],
                capture_output=True,
                text=True,
                timeout=RUNNER_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired:
            if len(batch) == 1:
                skipped.append(batch[0])
                print(f"{name}: skipped {batch[0]} (timeout)", file=sys.stderr)
                return
            mid = len(batch) // 2
            run_batch(batch[:mid])
            run_batch(batch[mid:])
            return
        if proc.returncode != 0:
            if len(batch) == 1:
                skipped.append(batch[0])
                message = proc.stderr.strip() or f"tool returned {proc.returncode}"
                print(f"{name}: skipped {batch[0]} ({message})", file=sys.stderr)
                return
            mid = len(batch) // 2
            run_batch(batch[:mid])
            run_batch(batch[mid:])
            return
        for line in proc.stdout.splitlines():
            if not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) == 4:
                path_s, in_s, out_s, ns_s = fields
                decode_ns_s = "0"
            elif len(fields) == 5:
                path_s, in_s, out_s, ns_s, decode_ns_s = fields
            else:
                raise SystemExit(f"{name} produced malformed line: {line!r}")
            rows.append(BenchmarkRow(Path(path_s), int(in_s), int(out_s), int(ns_s), int(decode_ns_s)))

    for start in range(0, len(files), BATCH_SIZE):
        run_batch(files[start : start + BATCH_SIZE])
    if len(rows) + len(skipped) != len(files):
        raise SystemExit(f"{name} produced {len(rows)} rows and {len(skipped)} skips for {len(files)} inputs")
    return ToolResult(name, rows, skipped)


def run_zstd19(files: list[Path], decode_iters: int) -> ToolResult:
    rows: list[BenchmarkRow] = []
    skipped: list[Path] = []
    for path in files:
        try:
            data = path.read_bytes()
            compressor = zstd.ZstdCompressor(level=19)
            start = time.perf_counter_ns()
            compressed = compressor.compress(data)
            end = time.perf_counter_ns()
            decompressor = zstd.ZstdDecompressor()
            decoded = decompressor.decompress(compressed)
            if decoded != data:
                raise zstd.ZstdError("decode verify failed")
            decode_total_ns = 0
            checksum = 0
            for iter_idx in range(decode_iters):
                decode_start = time.perf_counter_ns()
                decoded = decompressor.decompress(compressed)
                decode_end = time.perf_counter_ns()
                decode_total_ns += decode_end - decode_start
                checksum += len(decoded) + iter_idx
        except (OSError, zstd.ZstdError) as exc:
            skipped.append(path)
            print(f"zstd19: skipped {path} ({exc})", file=sys.stderr)
            continue
        rows.append(BenchmarkRow(path, len(data), len(compressed), end - start, decode_total_ns // decode_iters))
    return ToolResult("zstd19", rows, skipped)


def run_smolv_zstd19(files: list[Path], smolv_rows: list[BenchmarkRow], smolv_outdir: Path, decode_iters: int) -> ToolResult:
    rows: list[BenchmarkRow] = []
    skipped: list[Path] = []
    smolv_by_path = {row.path: row for row in smolv_rows}
    for path in files:
        smolv_row = smolv_by_path.get(path)
        if smolv_row is None:
            skipped.append(path)
            continue
        encoded_path = smolv_outdir / f"{path.name}.smolv"
        try:
            data = encoded_path.read_bytes()
            compressor = zstd.ZstdCompressor(level=19)
            start = time.perf_counter_ns()
            compressed = compressor.compress(data)
            end = time.perf_counter_ns()
            decompressor = zstd.ZstdDecompressor()
            decoded = decompressor.decompress(compressed)
            if decoded != data:
                raise zstd.ZstdError("decode verify failed")
            zstd_decode_total_ns = 0
            checksum = 0
            for iter_idx in range(decode_iters):
                decode_start = time.perf_counter_ns()
                decoded = decompressor.decompress(compressed)
                decode_end = time.perf_counter_ns()
                zstd_decode_total_ns += decode_end - decode_start
                checksum += len(decoded) + iter_idx
        except (OSError, zstd.ZstdError) as exc:
            skipped.append(path)
            print(f"smolv+zstd19: skipped {path} ({exc})", file=sys.stderr)
            continue
        decode_ns = smolv_row.decode_ns + (zstd_decode_total_ns // decode_iters)
        rows.append(BenchmarkRow(path, smolv_row.in_bytes, len(compressed), smolv_row.encode_ns + (end - start), decode_ns))
    return ToolResult("smolv+zstd19", rows, skipped)


def summarize(rows: list[BenchmarkRow]) -> dict[str, float]:
    ratios = [row.out_bytes / row.in_bytes for row in rows if row.in_bytes > 0]
    total_in = sum(row.in_bytes for row in rows)
    total_out = sum(row.out_bytes for row in rows)
    total_ns = sum(row.encode_ns for row in rows)
    total_decode_ns = sum(row.decode_ns for row in rows)
    return {
        "mean_ratio": statistics.fmean(ratios),
        "median_ratio": statistics.median(ratios),
        "total_ratio": total_out / total_in,
        "encode_mib_s": (total_in / (1024 * 1024)) / (total_ns / 1e9) if total_ns else 0.0,
        "decode_mib_s": (total_in / (1024 * 1024)) / (total_decode_ns / 1e9) if total_decode_ns else 0.0,
        "files": float(len(rows)),
        "input_mib": total_in / (1024 * 1024),
        "output_mib": total_out / (1024 * 1024),
        "seconds": total_ns / 1e9,
    }


def print_summary(name: str, stats: dict[str, float]) -> None:
    print(
        f"{name:10s} "
        f"mean={stats['mean_ratio']:.4f} "
        f"median={stats['median_ratio']:.4f} "
        f"total={stats['total_ratio']:.4f} "
        f"encode={stats['encode_mib_s']:.2f} MiB/s "
        f"decode={stats['decode_mib_s']:.2f} MiB/s "
        f"files={int(stats['files'])}"
    )


def print_skips(results: list[ToolResult]) -> None:
    print("skips:")
    for result in results:
        print(f"{result.name:12s} skipped={len(result.skipped)}")
        for path in result.skipped:
            print(f"  {path}")


def rows_for_intersection(result: ToolResult, intersection: set[Path], files: list[Path]) -> list[BenchmarkRow]:
    rows_by_path = {row.path: row for row in result.rows}
    return [rows_by_path[path] for path in files if path in intersection]


def build_arixv() -> Path | None:
    """Build the main project's TSV bench runner via CMake."""
    build_dir = ROOT / "build" / "release"
    exe = build_dir / "arixv_bench"
    cfg = subprocess.run(
        ["cmake", "--preset", "release"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
    )
    if cfg.returncode != 0:
        print(f"arixv: cmake configure failed, skipping runner\n{cfg.stderr}", file=sys.stderr)
        return None
    proc = subprocess.run(
        ["cmake", "--build", "--preset", "release", "--target", "arixv_bench", "-j"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0 or not exe.exists():
        print(f"arixv: build failed, skipping runner\n{proc.stderr}", file=sys.stderr)
        return None
    return exe


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark SPIR-V compression tools over the corpus.")
    parser.add_argument("--corpus", type=Path, default=CORPUS_DEFAULT, help="Directory containing .spv files.")
    parser.add_argument("--limit", type=int, default=None, help="Optional file limit for quick smoke tests.")
    parser.add_argument("--decode-iters", type=int, default=20, help="Decode iterations per file for timing.")
    args = parser.parse_args()
    if args.decode_iters <= 0:
        raise SystemExit("--decode-iters must be positive")

    files = load_corpus(args.corpus, args.limit)
    with tempfile.TemporaryDirectory(prefix="spv-bench-") as td:
        build_dir = Path(td)
        smolv_outdir = build_dir / "smolv-encoded"
        smolv_outdir.mkdir()
        runners = [SMOLV_RUNNER, YARIV_RUNNER]
        compiled = [build_runner(tool, build_dir) for tool in runners]
        decode_arg = f"--decode-iters={args.decode_iters}"
        smolv_result = run_tool(SMOLV_RUNNER.name, compiled[0], files, (f"--outdir={smolv_outdir}", decode_arg))
        yariv_result = run_tool(YARIV_RUNNER.name, compiled[1], files, (decode_arg,))
        zstd_result = run_zstd19(files, args.decode_iters)
        smolv_zstd_result = run_smolv_zstd19(files, smolv_result.rows, smolv_outdir, args.decode_iters)

        results = [smolv_result, yariv_result, zstd_result, smolv_zstd_result]
        arixv_exe = build_arixv()
        if arixv_exe is not None:
            results.append(run_tool("arixv", arixv_exe, files, (decode_arg,)))
        successful_sets = [set(row.path for row in result.rows) for result in results]
        intersection = set(files)
        for successful in successful_sets:
            intersection &= successful
        summaries = [(result.name, summarize(rows_for_intersection(result, intersection, files))) for result in results]

    print(f"corpus={args.corpus} files={len(files)} intersection={len(intersection)}")
    for name, stats in summaries:
        print_summary(name, stats)
    print_skips(results)
    print("main project: add a runner that prints TSV lines in the same <path>\\t<input_bytes>\\t<output_bytes>\\t<encode_ns>\\t<decode_ns> format, then add it to the runner list in scripts/bench/benchmark_spv_compressors.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
