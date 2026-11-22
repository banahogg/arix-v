// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "arixv/arixv.h"
#include "container.h"
#include "platform.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

// Upper bound on any single CLI input file/stream (matches library span limit).
constexpr uint64_t kMaxCliBytes = static_cast<uint64_t>(arixv::kMaxPlatformSpan);
// Default decode output allocation cap (512 MiB); override with --max-size.
constexpr uint64_t kDefaultMaxDecodeBytes = 512ull * 1024ull * 1024ull;
// Chunk size for bounded file/stream I/O calls.
constexpr size_t kIoChunkBytes = 16u * 1024u * 1024u;

namespace fs = std::filesystem;

void SetBinaryStdin() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
}

void SetBinaryStdout() {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

bool ReadStream(std::istream& in, std::vector<uint8_t>& data) {
  data.clear();
  char chunk[65536];
  while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0) {
    const std::streamsize got = in.gcount();
    if (got < 0) {
      return false;
    }
    const uint64_t n = static_cast<uint64_t>(got);
    if (n > kMaxCliBytes || data.size() > kMaxCliBytes - n) {
      data.clear();
      return false;
    }
    const size_t old_size = data.size();
    const size_t add = static_cast<size_t>(n);
    data.resize(old_size + add);
    std::memcpy(data.data() + old_size, chunk, add);
    if (!in) {
      break;
    }
  }
  return !in.bad();
}

bool ReadStreamHeaderAndCount(std::istream& in, uint8_t* hdr, size_t hdr_cap,
                              size_t* hdr_len, uint64_t* total_bytes) {
  *hdr_len = 0;
  *total_bytes = 0;
  char chunk[65536];
  while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0) {
    const std::streamsize got = in.gcount();
    if (got < 0) {
      return false;
    }
    const size_t n = static_cast<size_t>(got);
    if (*hdr_len < hdr_cap) {
      const size_t copy = std::min(n, hdr_cap - *hdr_len);
      std::memcpy(hdr + *hdr_len, chunk, copy);
      *hdr_len += copy;
    }
    *total_bytes += static_cast<uint64_t>(n);
    if (!in) {
      break;
    }
  }
  return !in.bad();
}

bool ReadInput(const std::string& path, std::vector<uint8_t>& data,
               std::string* err_detail = nullptr) {
  if (path == "-") {
    SetBinaryStdin();
    if (!ReadStream(std::cin, data)) {
      if (err_detail != nullptr) {
        *err_detail = "stream read failed or exceeded platform limit";
      }
      return false;
    }
    return true;
  }
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size_off = in.tellg();
  if (size_off < 0) {
    // Non-seekable special file (e.g. FIFO / /dev/stdin).
    in.clear();
    if (!ReadStream(in, data)) {
      if (err_detail != nullptr) {
        *err_detail = "stream read failed or exceeded platform limit";
      }
      return false;
    }
    return true;
  }
  const uint64_t size_u64 = static_cast<uint64_t>(size_off);
  if (size_u64 > kMaxCliBytes) {
    if (err_detail != nullptr) {
      *err_detail = "input exceeds platform maximum span";
    }
    return false;
  }
  const size_t target_size = static_cast<size_t>(size_u64);
  in.seekg(0, std::ios::beg);
  if (!in) {
    return false;
  }
  data.resize(target_size);
  size_t offset = 0;
  while (offset < target_size) {
    const size_t step = std::min(target_size - offset, kIoChunkBytes);
    in.read(reinterpret_cast<char*>(data.data() + offset),
            static_cast<std::streamsize>(step));
    const std::streamsize got = in.gcount();
    if (got <= 0 || static_cast<size_t>(got) != step) {
      data.clear();
      return false;
    }
    offset += step;
  }
  return !in.bad();
}

bool WriteAll(std::ostream& out, const std::vector<uint8_t>& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const size_t step = std::min(data.size() - offset, kIoChunkBytes);
    out.write(reinterpret_cast<const char*>(data.data() + offset),
              static_cast<std::streamsize>(step));
    if (!out) {
      return false;
    }
    offset += step;
  }
  out.flush();
  return static_cast<bool>(out);
}

bool WriteOutput(const std::string& path, const std::vector<uint8_t>& data) {
  if (path == "-") {
    SetBinaryStdout();
    return WriteAll(std::cout, data);
  }
  std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  if (!WriteAll(out, data)) {
    return false;
  }
  out.close();
  return static_cast<bool>(out);
}

bool WriteOutputPath(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  if (!WriteAll(out, data)) {
    return false;
  }
  out.close();
  return static_cast<bool>(out);
}

std::string NormalizeCollisionKey(const fs::path& p) {
  std::error_code ec;
  fs::path norm = fs::weakly_canonical(p, ec);
  if (ec) {
    norm = p.lexically_normal();
  }
  std::string key = norm.generic_string();
#if defined(_WIN32) || defined(__APPLE__)
  for (char& ch : key) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
#endif
  return key;
}

bool ParseSizeArg(const std::string& s, uint64_t* value) {
  if (s.empty() || s[0] == '-' || s[0] == '+') {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') {
    return false;
  }
  *value = static_cast<uint64_t>(v);
  return true;
}

const char* ModeName(uint32_t mode) {
  switch (mode) {
    case ARIXV_MODE_STORED:
      return "stored";
    case ARIXV_MODE_CODED:
      return "coded";
    case ARIXV_MODE_SPIRV:
      return "spirv";
    default:
      return "unknown";
  }
}

std::string FormatMagic(uint32_t magic) {
  char out[5];
  for (int i = 0; i < 4; ++i) {
    const unsigned char ch =
        static_cast<unsigned char>((magic >> (i * 8)) & 0xFFu);
    out[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : '?';
  }
  out[4] = '\0';
  return std::string(out);
}

std::string FormatRatio(uint64_t compressed, uint64_t uncompressed) {
  if (uncompressed == 0) {
    return "-";
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f",
                static_cast<double>(compressed) /
                    static_cast<double>(uncompressed));
  return std::string(buf);
}

std::string FormatPercent(uint64_t compressed, uint64_t uncompressed) {
  if (uncompressed == 0) {
    return "n/a";
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f%%",
                100.0 * static_cast<double>(compressed) /
                    static_cast<double>(uncompressed));
  return std::string(buf);
}

arixv_status EncodeBuffer(const std::vector<uint8_t>& input, bool force_coded,
                          std::vector<uint8_t>& out) {
  if (force_coded) {
    const arixv_status status =
        arixv::EncodeInternal(input.data(), input.size(), true, out);
    if (status != ARIXV_STATUS_OK) {
      return status;
    }
    if (out.size() > arixv::kMaxPlatformSpan) {
      out.clear();
      return ARIXV_STATUS_TOO_LARGE;
    }
    arixv_header_info info{};
    info.struct_size = sizeof(info);
    const arixv_status inspect_status =
        arixv_inspect(out.data(), out.size(), &info);
    if (inspect_status != ARIXV_STATUS_OK) {
      out.clear();
      return inspect_status;
    }
    if (info.uncompressed_size > arixv::kPlatformMaxUncompressedSize) {
      out.clear();
      return ARIXV_STATUS_TOO_LARGE;
    }
    return ARIXV_STATUS_OK;
  }

  const size_t bound = arixv_encode_bound(input.size(), nullptr);
  if (bound == 0) {
    return ARIXV_STATUS_TOO_LARGE;
  }
  out.resize(bound);
  size_t out_size = out.size();
  const arixv_status status =
      arixv_encode(input.data(), input.size(), out.data(), &out_size, nullptr);
  if (status == ARIXV_STATUS_OK) {
    out.resize(out_size);
  }
  return status;
}

arixv_status DecodeBuffer(const std::vector<uint8_t>& input,
                          uint64_t max_output_bytes,
                          std::vector<uint8_t>& out,
                          size_t* declared_size_out = nullptr) {
  const uint8_t empty_in = 0;
  const void* src = input.empty() ? static_cast<const void*>(&empty_in)
                                  : static_cast<const void*>(input.data());
  size_t required = 0;
  arixv_status status =
      arixv_decode(src, input.size(), nullptr, &required, nullptr);
  if (status != ARIXV_STATUS_OK) {
    return status;
  }
  if (declared_size_out != nullptr) {
    *declared_size_out = required;
  }
  if (max_output_bytes != 0 && static_cast<uint64_t>(required) > max_output_bytes) {
    return ARIXV_STATUS_TOO_LARGE;
  }
  out.resize(required);
  uint8_t empty_sink = 0;
  void* dst = required == 0 ? static_cast<void*>(&empty_sink)
                            : static_cast<void*>(out.data());
  size_t out_size = required;
  status = arixv_decode(src, input.size(), dst, &out_size, nullptr);
  if (status == ARIXV_STATUS_OK) {
    out.resize(out_size);
  }
  return status;
}

void PrintVersion(std::ostream& os) {
  const uint32_t v = arixv_version();
  const uint32_t major = (v >> 16) & 0xFFu;
  const uint32_t minor = (v >> 8) & 0xFFu;
  const uint32_t patch = v & 0xFFu;
  os << "arixv " << major << "." << minor << "." << patch << " (format "
     << ARIXV_MAGIC_0 << ARIXV_MAGIC_1 << ARIXV_MAGIC_2 << ARIXV_MAGIC_3
     << ")\n";
}

void PrintUsage(std::ostream& os, const char* argv0) {
  PrintVersion(os);
  os << "usage:\n"
     << "  " << argv0
     << " c [--force-coded] [-q|-v] <in> <out>\n"
     << "  " << argv0
     << " c [--force-coded] [-q|-v] --out-dir <dir> <files...>\n"
     << "  " << argv0 << " d [-q|-v] [--max-size <bytes>] <in> <out>\n"
     << "  " << argv0 << " info <files...>\n"
     << "  " << argv0 << " roundtrip [--force-coded] <files...>\n"
     << "  " << argv0 << " --version | --help\n"
     << "\n"
     << "notes:\n"
     << "  use '-' for stdin/stdout in single-file 'c', 'd', and 'info'.\n"
     << "  --out-dir writes <dir>/<filename>.arixv for each input file.\n"
     << "  --max-size caps decoded output size in bytes (default 512 MiB; 0 = platform max).\n"
     << "  -v prints compression/decompression summaries to stderr; -q is quiet.\n";
}

int RunCompress(const char* argv0, int argc, char** argv) {
  bool force_coded = false;
  bool verbose = false;
  bool has_out_dir = false;
  std::string out_dir;
  std::vector<std::string> positional;
  bool end_of_opts = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!end_of_opts && arg == "--") {
      end_of_opts = true;
      continue;
    }
    if (!end_of_opts && arg.size() > 1 && arg[0] == '-') {
      if (arg == "-h" || arg == "--help") {
        PrintUsage(std::cout, argv0);
        return kExitOk;
      } else if (arg == "--force-coded") {
        force_coded = true;
      } else if (arg == "-v" || arg == "--verbose") {
        verbose = true;
      } else if (arg == "-q" || arg == "--quiet") {
        verbose = false;
      } else if (arg == "--out-dir") {
        if (i + 1 >= argc) {
          std::cerr << "error: --out-dir requires a directory argument\n";
          return kExitUsage;
        }
        has_out_dir = true;
        out_dir = argv[++i];
        if (out_dir.empty()) {
          std::cerr << "error: --out-dir requires a non-empty directory\n";
          return kExitUsage;
        }
      } else if (arg.rfind("--out-dir=", 0) == 0) {
        has_out_dir = true;
        out_dir = arg.substr(10);
        if (out_dir.empty()) {
          std::cerr << "error: --out-dir requires a non-empty directory\n";
          return kExitUsage;
        }
      } else {
        std::cerr << "error: unknown option '" << arg << "' for 'c'\n";
        PrintUsage(std::cerr, argv0);
        return kExitUsage;
      }
    } else {
      positional.push_back(arg);
    }
  }

  if (has_out_dir) {
    if (positional.empty()) {
      std::cerr << "error: 'c --out-dir' requires at least one input file\n";
      PrintUsage(std::cerr, argv0);
      return kExitUsage;
    }
    const fs::path dir_path(out_dir);
    std::error_code ec;
    if (fs::exists(dir_path, ec)) {
      if (ec || !fs::is_directory(dir_path, ec)) {
        std::cerr << "error: --out-dir path is not a directory: " << out_dir
                  << "\n";
        return kExitError;
      }
    } else {
      fs::create_directories(dir_path, ec);
      if (ec) {
        std::cerr << "error: failed to create output directory '" << out_dir
                  << "': " << ec.message() << "\n";
        return kExitError;
      }
    }

    // Resolve all input and output paths up front so duplicate outputs (with
    // platform case-folding on Windows/macOS) or output-to-input overwrites
    // are rejected before any file is written.
    std::vector<fs::path> in_paths;
    std::vector<fs::path> out_paths;
    in_paths.reserve(positional.size());
    out_paths.reserve(positional.size());
    std::set<std::string> seen_inputs;
    std::set<std::string> seen_outputs;

    for (const std::string& in_str : positional) {
      if (in_str == "-") {
        std::cerr << "error: cannot use '-' (stdin) with --out-dir\n";
        return kExitUsage;
      }
      const fs::path in_path(in_str);
      const fs::path fname = in_path.filename();
      if (fname.empty() || fname == "." || fname == "..") {
        std::cerr << "error: cannot derive output filename from '" << in_str
                  << "'\n";
        return kExitUsage;
      }
      fs::path out_fname = fname;
      out_fname += ".arixv";
      const fs::path out_path = dir_path / out_fname;
      const std::string out_key = NormalizeCollisionKey(out_path);
      if (!seen_outputs.insert(out_key).second) {
        std::cerr << "error: duplicate output path in --out-dir: "
                  << out_path.string() << "\n";
        return kExitError;
      }
      seen_inputs.insert(NormalizeCollisionKey(in_path));
      in_paths.push_back(in_path);
      out_paths.push_back(out_path);
    }

    for (size_t i = 0; i < out_paths.size(); ++i) {
      const std::string out_key = NormalizeCollisionKey(out_paths[i]);
      if (seen_inputs.count(out_key) != 0) {
        std::cerr << "error: batch output would overwrite input file: "
                  << out_paths[i].string() << "\n";
        return kExitError;
      }
      for (const fs::path& in_path : in_paths) {
        std::error_code eq_ec;
        if (fs::exists(out_paths[i], eq_ec) && !eq_ec &&
            fs::exists(in_path, eq_ec) && !eq_ec &&
            fs::equivalent(out_paths[i], in_path, eq_ec) && !eq_ec) {
          std::cerr << "error: batch output would overwrite input file: "
                    << out_paths[i].string() << "\n";
          return kExitError;
        }
      }
    }

    uint64_t total_in = 0;
    uint64_t total_out = 0;
    bool all_ok = true;
    for (size_t i = 0; i < positional.size(); ++i) {
      std::vector<uint8_t> input;
      std::string read_err;
      if (!ReadInput(positional[i], input, &read_err)) {
        std::cerr << "error: failed to read " << positional[i];
        if (!read_err.empty()) {
          std::cerr << ": " << read_err;
        }
        std::cerr << "\n";
        all_ok = false;
        continue;
      }
      std::vector<uint8_t> encoded;
      const arixv_status status = EncodeBuffer(input, force_coded, encoded);
      if (status != ARIXV_STATUS_OK) {
        std::cerr << "error: encode failed for " << positional[i] << ": "
                  << arixv_status_string(status) << "\n";
        all_ok = false;
        continue;
      }
      if (!WriteOutputPath(out_paths[i], encoded)) {
        std::cerr << "error: failed to write " << out_paths[i].string() << "\n";
        all_ok = false;
        continue;
      }
      total_in += input.size();
      total_out += encoded.size();
      if (verbose) {
        arixv_header_info info{};
        info.struct_size = sizeof(info);
        arixv_inspect(encoded.data(), encoded.size(), &info);
        std::cerr << positional[i] << " -> " << out_paths[i].string() << ": "
                  << input.size() << " -> " << encoded.size() << " bytes ("
                  << FormatPercent(encoded.size(), input.size())
                  << ", mode=" << ModeName(info.mode) << ")\n";
      }
    }
    if (verbose && all_ok) {
      std::cerr << positional.size() << " files: " << total_in << " -> "
                << total_out << " bytes ("
                << FormatPercent(total_out, total_in) << ")\n";
    }
    return all_ok ? kExitOk : kExitError;
  }

  if (positional.size() != 2) {
    PrintUsage(std::cerr, argv0);
    return kExitUsage;
  }

  std::vector<uint8_t> input;
  std::string read_err;
  if (!ReadInput(positional[0], input, &read_err)) {
    std::cerr << "error: failed to read " << positional[0];
    if (!read_err.empty()) {
      std::cerr << ": " << read_err;
    }
    std::cerr << "\n";
    return kExitError;
  }
  std::vector<uint8_t> encoded;
  const arixv_status status = EncodeBuffer(input, force_coded, encoded);
  if (status != ARIXV_STATUS_OK) {
    std::cerr << "error: encode failed: " << arixv_status_string(status)
              << "\n";
    return kExitError;
  }
  if (!WriteOutput(positional[1], encoded)) {
    std::cerr << "error: failed to write " << positional[1] << "\n";
    return kExitError;
  }
  if (verbose) {
    arixv_header_info info{};
    info.struct_size = sizeof(info);
    arixv_inspect(encoded.data(), encoded.size(), &info);
    std::cerr << positional[0] << " -> " << positional[1] << ": "
              << input.size() << " -> " << encoded.size() << " bytes ("
              << FormatPercent(encoded.size(), input.size())
              << ", mode=" << ModeName(info.mode) << ")\n";
  }
  return kExitOk;
}

int RunDecompress(const char* argv0, int argc, char** argv) {
  bool verbose = false;
  uint64_t max_output_bytes = kDefaultMaxDecodeBytes;
  std::vector<std::string> positional;
  bool end_of_opts = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!end_of_opts && arg == "--") {
      end_of_opts = true;
      continue;
    }
    if (!end_of_opts && arg.size() > 1 && arg[0] == '-') {
      if (arg == "-h" || arg == "--help") {
        PrintUsage(std::cout, argv0);
        return kExitOk;
      } else if (arg == "-v" || arg == "--verbose") {
        verbose = true;
      } else if (arg == "-q" || arg == "--quiet") {
        verbose = false;
      } else if (arg == "--max-size") {
        if (i + 1 >= argc || !ParseSizeArg(argv[++i], &max_output_bytes)) {
          std::cerr << "error: --max-size requires a non-negative byte count\n";
          return kExitUsage;
        }
      } else if (arg.rfind("--max-size=", 0) == 0) {
        if (!ParseSizeArg(arg.substr(11), &max_output_bytes)) {
          std::cerr << "error: --max-size requires a non-negative byte count\n";
          return kExitUsage;
        }
      } else {
        std::cerr << "error: unknown option '" << arg << "' for 'd'\n";
        PrintUsage(std::cerr, argv0);
        return kExitUsage;
      }
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.size() != 2) {
    PrintUsage(std::cerr, argv0);
    return kExitUsage;
  }

  std::vector<uint8_t> input;
  std::string read_err;
  if (!ReadInput(positional[0], input, &read_err)) {
    std::cerr << "error: failed to read " << positional[0];
    if (!read_err.empty()) {
      std::cerr << ": " << read_err;
    }
    std::cerr << "\n";
    return kExitError;
  }
  std::vector<uint8_t> decoded;
  size_t declared = 0;
  const arixv_status status =
      DecodeBuffer(input, max_output_bytes, decoded, &declared);
  if (status != ARIXV_STATUS_OK) {
    if (status == ARIXV_STATUS_TOO_LARGE && max_output_bytes != 0 &&
        static_cast<uint64_t>(declared) > max_output_bytes) {
      std::cerr << "error: declared output size (" << declared
                << " bytes) exceeds --max-size limit (" << max_output_bytes
                << " bytes); pass --max-size <bytes> to override\n";
    } else {
      std::cerr << "error: decode failed: " << arixv_status_string(status)
                << "\n";
    }
    return kExitError;
  }
  if (!WriteOutput(positional[1], decoded)) {
    std::cerr << "error: failed to write " << positional[1] << "\n";
    return kExitError;
  }
  if (verbose) {
    std::cerr << positional[0] << " -> " << positional[1] << ": "
              << input.size() << " -> " << decoded.size() << " bytes\n";
  }
  return kExitOk;
}

int RunInfo(const char* argv0, int argc, char** argv) {
  std::vector<std::string> positional;
  bool end_of_opts = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!end_of_opts && arg == "--") {
      end_of_opts = true;
      continue;
    }
    if (!end_of_opts && arg.size() > 1 && arg[0] == '-') {
      if (arg == "-h" || arg == "--help") {
        PrintUsage(std::cout, argv0);
        return kExitOk;
      }
      std::cerr << "error: unknown option '" << arg << "' for 'info'\n";
      PrintUsage(std::cerr, argv0);
      return kExitUsage;
    }
    positional.push_back(arg);
  }

  if (positional.empty()) {
    PrintUsage(std::cerr, argv0);
    return kExitUsage;
  }

  size_t stdin_count = 0;
  for (const std::string& path : positional) {
    if (path == "-") {
      ++stdin_count;
    }
  }
  if (stdin_count > 1) {
    std::cerr << "error: cannot inspect stdin ('-') more than once\n";
    return kExitUsage;
  }

  bool all_ok = true;
  for (const std::string& path : positional) {
    uint8_t header_buf[16];
    size_t header_bytes = 0;
    uint64_t compressed_size = 0;

    if (path == "-") {
      SetBinaryStdin();
      if (!ReadStreamHeaderAndCount(std::cin, header_buf, sizeof(header_buf),
                                    &header_bytes, &compressed_size)) {
        std::cerr << "error: failed to read -\n";
        all_ok = false;
        continue;
      }
    } else {
      std::ifstream in(fs::path(path), std::ios::binary);
      if (!in) {
        std::cerr << "error: failed to read " << path << "\n";
        all_ok = false;
        continue;
      }
      in.seekg(0, std::ios::end);
      const std::streamoff size = in.tellg();
      if (size < 0) {
        in.clear();
        if (!ReadStreamHeaderAndCount(in, header_buf, sizeof(header_buf),
                                      &header_bytes, &compressed_size)) {
          std::cerr << "error: failed to read " << path << "\n";
          all_ok = false;
          continue;
        }
      } else {
        compressed_size = static_cast<uint64_t>(size);
        in.seekg(0, std::ios::beg);
        const size_t to_read =
            compressed_size < sizeof(header_buf)
                ? static_cast<size_t>(compressed_size)
                : sizeof(header_buf);
        if (to_read > 0) {
          in.read(reinterpret_cast<char*>(header_buf),
                  static_cast<std::streamsize>(to_read));
          if (!in || static_cast<size_t>(in.gcount()) != to_read) {
            std::cerr << "error: failed to read " << path << "\n";
            all_ok = false;
            continue;
          }
        }
        header_bytes = to_read;
      }
    }

    arixv_header_info info{};
    info.struct_size = sizeof(info);
    const arixv_status status = arixv_inspect(header_buf, header_bytes, &info);
    if (status != ARIXV_STATUS_OK) {
      std::cerr << "error: " << path << ": " << arixv_status_string(status)
                << "\n";
      all_ok = false;
      continue;
    }

    std::cout << path << "\tmagic=" << FormatMagic(info.magic)
              << "\tmode=" << ModeName(info.mode)
              << "\tcompressed=" << compressed_size
              << "\tuncompressed=" << info.uncompressed_size
              << "\tratio=" << FormatRatio(compressed_size, info.uncompressed_size)
              << "\n";
  }

  std::cout.flush();
  if (!std::cout) {
    std::cerr << "error: failed to write stdout\n";
    return kExitError;
  }
  return all_ok ? kExitOk : kExitError;
}

int RunRoundtrip(const char* argv0, int argc, char** argv) {
  bool force_coded = false;
  std::vector<std::string> positional;
  bool end_of_opts = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!end_of_opts && arg == "--") {
      end_of_opts = true;
      continue;
    }
    if (!end_of_opts && arg.size() > 1 && arg[0] == '-') {
      if (arg == "-h" || arg == "--help") {
        PrintUsage(std::cout, argv0);
        return kExitOk;
      } else if (arg == "--force-coded") {
        force_coded = true;
      } else {
        std::cerr << "error: unknown option '" << arg << "' for 'roundtrip'\n";
        PrintUsage(std::cerr, argv0);
        return kExitUsage;
      }
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.empty()) {
    PrintUsage(std::cerr, argv0);
    return kExitUsage;
  }

  bool all_ok = true;
  for (const std::string& path : positional) {
    std::vector<uint8_t> input;
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> decoded;
    std::string read_err;
    bool ok = ReadInput(path, input, &read_err);
    if (!ok) {
      std::cerr << "error: failed to read " << path;
      if (!read_err.empty()) {
        std::cerr << ": " << read_err;
      }
      std::cerr << "\n";
    } else {
      const arixv_status enc_st = EncodeBuffer(input, force_coded, encoded);
      if (enc_st != ARIXV_STATUS_OK) {
        std::cerr << "error: encode failed for " << path << ": "
                  << arixv_status_string(enc_st) << "\n";
        ok = false;
      } else {
        const arixv_status dec_st = DecodeBuffer(encoded, 0, decoded);
        if (dec_st != ARIXV_STATUS_OK) {
          std::cerr << "error: decode failed for " << path << ": "
                    << arixv_status_string(dec_st) << "\n";
          ok = false;
        } else if (decoded != input) {
          std::cerr << "error: roundtrip byte mismatch for " << path << "\n";
          ok = false;
        }
      }
    }
    std::cout << path << "\t" << (ok ? "OK" : "FAIL") << "\t" << input.size()
              << "\t" << encoded.size() << "\n";
    all_ok = all_ok && ok;
  }
  std::cout.flush();
  if (!std::cout) {
    std::cerr << "error: failed to write stdout\n";
    return kExitError;
  }
  return all_ok ? kExitOk : kExitError;
}

int RunMain(int argc, char** argv) {
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  if (argc < 2) {
    PrintUsage(std::cerr, argv[0]);
    return kExitUsage;
  }

  const std::string cmd = argv[1];
  if (cmd == "-h" || cmd == "--help") {
    PrintUsage(std::cout, argv[0]);
    return kExitOk;
  }
  if (cmd == "-V" || cmd == "--version") {
    PrintVersion(std::cout);
    return kExitOk;
  }
  if (cmd == "c") {
    return RunCompress(argv[0], argc, argv);
  }
  if (cmd == "d") {
    return RunDecompress(argv[0], argc, argv);
  }
  if (cmd == "info") {
    return RunInfo(argv[0], argc, argv);
  }
  if (cmd == "roundtrip") {
    return RunRoundtrip(argv[0], argc, argv);
  }

  std::cerr << "error: unknown command '" << cmd << "'\n";
  PrintUsage(std::cerr, argv[0]);
  return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
#if ARIXV_HAS_EXCEPTIONS
  try {
    return RunMain(argc, argv);
  } catch (const std::bad_alloc&) {
    std::cerr << "error: out of memory\n";
    return kExitError;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return kExitError;
  } catch (...) {
    std::cerr << "error: unexpected internal exception\n";
    return kExitError;
  }
#else
  return RunMain(argc, argv);
#endif
}
