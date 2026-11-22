// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "arixv/arixv.h"
#include "container.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

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
    const auto n = static_cast<size_t>(in.gcount());
    const size_t old_size = data.size();
    data.resize(old_size + n);
    for (size_t i = 0; i < n; ++i) {
      data[old_size + i] = static_cast<uint8_t>(chunk[i]);
    }
    if (!in) {
      break;
    }
  }
  return !in.bad();
}

bool ReadInput(const std::string& path, std::vector<uint8_t>& data) {
  if (path == "-") {
    SetBinaryStdin();
    return ReadStream(std::cin, data);
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    // Non-seekable special file (e.g. FIFO / /dev/stdin).
    in.clear();
    return ReadStream(in, data);
  }
  in.seekg(0, std::ios::beg);
  data.resize(static_cast<size_t>(size));
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), size);
  }
  return static_cast<bool>(in);
}

bool WriteOutput(const std::string& path, const std::vector<uint8_t>& data) {
  if (path == "-") {
    SetBinaryStdout();
    if (!data.empty()) {
      std::cout.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
    }
    std::cout.flush();
    return static_cast<bool>(std::cout);
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }
  return static_cast<bool>(out);
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
    const unsigned char ch = static_cast<unsigned char>((magic >> (i * 8)) & 0xFFu);
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
                          std::vector<uint8_t>& out) {
  const uint8_t empty_in = 0;
  const void* src = input.empty() ? static_cast<const void*>(&empty_in)
                                  : static_cast<const void*>(input.data());
  size_t required = 0;
  arixv_status status =
      arixv_decode(src, input.size(), nullptr, &required, nullptr);
  if (status != ARIXV_STATUS_OK) {
    return status;
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
  os << "usage:\n"
     << "  " << argv0
     << " c [--force-coded] [-q|-v] <in> <out>\n"
     << "  " << argv0
     << " c [--force-coded] [-q|-v] --out-dir <dir> <files...>\n"
     << "  " << argv0 << " d [-q|-v] <in> <out>\n"
     << "  " << argv0 << " info <files...>\n"
     << "  " << argv0 << " roundtrip [--force-coded] <files...>\n"
     << "  " << argv0 << " --version | --help\n"
     << "\n"
     << "notes:\n"
     << "  use '-' for stdin/stdout in single-file 'c', 'd', and 'info'.\n"
     << "  --out-dir writes <dir>/<filename>.arixv for each input file.\n"
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
    namespace fs = std::filesystem;
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

    // Validate inputs and detect output filename collisions up front.
    std::vector<std::string> out_paths;
    out_paths.reserve(positional.size());
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
      const std::string out_str =
          (dir_path / (fname.string() + ".arixv")).string();
      if (!seen_outputs.insert(out_str).second) {
        std::cerr << "error: duplicate output path in --out-dir: " << out_str
                  << "\n";
        return kExitError;
      }
      out_paths.push_back(out_str);
    }

    uint64_t total_in = 0;
    uint64_t total_out = 0;
    bool all_ok = true;
    for (size_t i = 0; i < positional.size(); ++i) {
      std::vector<uint8_t> input;
      if (!ReadInput(positional[i], input)) {
        std::cerr << "error: failed to read " << positional[i] << "\n";
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
      if (!WriteOutput(out_paths[i], encoded)) {
        std::cerr << "error: failed to write " << out_paths[i] << "\n";
        all_ok = false;
        continue;
      }
      total_in += input.size();
      total_out += encoded.size();
      if (verbose) {
        arixv_header_info info{};
        info.struct_size = sizeof(info);
        arixv_inspect(encoded.data(), encoded.size(), &info);
        std::cerr << positional[i] << " -> " << out_paths[i] << ": "
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
  if (!ReadInput(positional[0], input)) {
    std::cerr << "error: failed to read " << positional[0] << "\n";
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
  if (!ReadInput(positional[0], input)) {
    std::cerr << "error: failed to read " << positional[0] << "\n";
    return kExitError;
  }
  std::vector<uint8_t> decoded;
  const arixv_status status = DecodeBuffer(input, decoded);
  if (status != ARIXV_STATUS_OK) {
    std::cerr << "error: decode failed: " << arixv_status_string(status)
              << "\n";
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

  bool all_ok = true;
  for (const std::string& path : positional) {
    uint8_t header_buf[16];
    size_t header_bytes = 0;
    uint64_t compressed_size = 0;
    std::vector<uint8_t> stream_buf;
    const void* inspect_ptr = header_buf;

    if (path == "-") {
      SetBinaryStdin();
      if (!ReadStream(std::cin, stream_buf)) {
        std::cerr << "error: failed to read -\n";
        all_ok = false;
        continue;
      }
      compressed_size = stream_buf.size();
      inspect_ptr = stream_buf.empty() ? static_cast<const void*>(header_buf)
                                       : static_cast<const void*>(stream_buf.data());
      header_bytes = stream_buf.size();
    } else {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        std::cerr << "error: failed to read " << path << "\n";
        all_ok = false;
        continue;
      }
      in.seekg(0, std::ios::end);
      const std::streamoff size = in.tellg();
      if (size < 0) {
        in.clear();
        if (!ReadStream(in, stream_buf)) {
          std::cerr << "error: failed to read " << path << "\n";
          all_ok = false;
          continue;
        }
        compressed_size = stream_buf.size();
        inspect_ptr = stream_buf.empty()
                          ? static_cast<const void*>(header_buf)
                          : static_cast<const void*>(stream_buf.data());
        header_bytes = stream_buf.size();
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
          if (!in) {
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
    const arixv_status status = arixv_inspect(inspect_ptr, header_bytes, &info);
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
    std::vector<uint8_t> decoded;
    bool ok = ReadInput(path, input);
    std::vector<uint8_t> encoded;
    if (ok) {
      ok = EncodeBuffer(input, force_coded, encoded) == ARIXV_STATUS_OK &&
           DecodeBuffer(encoded, decoded) == ARIXV_STATUS_OK &&
           decoded == input;
    }
    std::cout << path << "\t" << (ok ? "OK" : "FAIL") << "\t" << input.size()
              << "\t" << encoded.size() << "\n";
    all_ok = all_ok && ok;
  }
  return all_ok ? kExitOk : kExitError;
}

}  // namespace

int main(int argc, char** argv) {
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
