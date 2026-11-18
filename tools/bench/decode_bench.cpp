#include <chrono>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "arixv.h"
#include "model.h"

namespace {

struct Case {
  std::string path;
  std::vector<uint8_t> input;
  std::vector<uint8_t> encoded;
};

bool ReadFile(const std::string& path, std::vector<uint8_t>& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  data.resize(static_cast<size_t>(size));
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), size);
  }
  return static_cast<bool>(in);
}

uint64_t Sample(const std::vector<uint8_t>& data) {
  uint64_t h = data.size();
  if (!data.empty()) {
    h ^= static_cast<uint64_t>(data.front()) << 8;
    h ^= static_cast<uint64_t>(data[data.size() / 2]) << 16;
    h ^= static_cast<uint64_t>(data.back()) << 24;
  }
  return h;
}

bool ParseIters(const char* arg, int* iters) {
  const std::string s(arg);
  const std::string prefix = "--iters=";
  if (s.rfind(prefix, 0) != 0) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(s.c_str() + prefix.size(), &end, 10);
  if (end == s.c_str() + prefix.size() || *end != '\0' || value <= 0) {
    return false;
  }
  *iters = static_cast<int>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int iters = 20;
  bool per_file = false;
  bool raw_residual = false;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--per-file") {
      per_file = true;
      continue;
    }
    if (arg == "--raw-residual") {
      raw_residual = true;
      continue;
    }
    if (arg.rfind("--iters=", 0) == 0) {
      if (!ParseIters(argv[i], &iters)) {
        std::cerr << "bad --iters value\n";
        return 1;
      }
      continue;
    }
    paths.push_back(arg);
  }
  if (paths.empty()) {
    std::cerr
        << "usage: arixv_decode_bench [--iters=N] [--per-file] "
           "[--raw-residual] <files...>\n";
    return 1;
  }

  std::vector<Case> cases;
  cases.reserve(paths.size());
  size_t total_input = 0;
  size_t total_encoded = 0;
  std::array<size_t, 256> mode_counts{};
  for (const std::string& path : paths) {
    Case c;
    c.path = path;
    if (!ReadFile(path, c.input)) {
      std::cerr << "failed to read " << path << "\n";
      return 1;
    }
    if (raw_residual) {
      if (!arixv::IsSpirvModelEligible(c.input.data(), c.input.size())) {
        std::cerr << "raw residual requires word-aligned SPIR-V: " << path
                  << "\n";
        return 1;
      }
      arixv::EncodeModuleRawResidual(c.input.data(), c.input.size(),
                                     c.encoded);
    } else {
      c.encoded = arixv::Encode(c.input.data(), c.input.size());
    }
    std::vector<uint8_t> decoded;
    const arixv::Status status = raw_residual
        ? arixv::DecodeModuleRawResidual(c.encoded.data(), c.encoded.size(),
                                         c.input.size(), decoded)
        : arixv::Decode(c.encoded.data(), c.encoded.size(), decoded);
    if (status != arixv::Status::kOk || decoded != c.input) {
      std::cerr << "roundtrip failed for " << path << "\n";
      return 1;
    }
    total_input += c.input.size();
    total_encoded += c.encoded.size();
    if (!raw_residual &&
        c.encoded.size() > arixv::kContainerMagicBytes) {
      ++mode_counts[c.encoded[arixv::kContainerMagicBytes]];
    }
    cases.push_back(std::move(c));
  }

  uint64_t checksum = 0;
  int64_t total_ns = 0;
  if (per_file) {
    std::cout << "path\tinput_bytes\tencoded_bytes\tdecode_ns_per_iter\n";
  }
  for (const Case& c : cases) {
    std::vector<uint8_t> decoded;
    const auto start = std::chrono::steady_clock::now();
    for (int iter = 0; iter < iters; ++iter) {
      decoded.clear();
      const arixv::Status status =
          raw_residual
              ? arixv::DecodeModuleRawResidual(c.encoded.data(),
                                               c.encoded.size(),
                                               c.input.size(), decoded)
              : arixv::Decode(c.encoded.data(), c.encoded.size(), decoded);
      if (status != arixv::Status::kOk ||
          decoded.size() != c.input.size()) {
        std::cerr << "decode failed for " << c.path << "\n";
        return 1;
      }
      checksum += Sample(decoded) + static_cast<uint64_t>(iter);
    }
    const auto end = std::chrono::steady_clock::now();
    const int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();
    total_ns += ns;
    if (per_file) {
      std::cout << c.path << '\t' << c.input.size() << '\t'
                << c.encoded.size() << '\t' << (ns / iters) << '\n';
    }
  }

  const double seconds = static_cast<double>(total_ns) / 1e9;
  const double decoded_mib =
      (static_cast<double>(total_input) * iters) / (1024.0 * 1024.0);
  std::cerr << "files=" << cases.size() << " iterations=" << iters
            << " raw_residual=" << (raw_residual ? 1 : 0)
            << " input_bytes=" << total_input
            << " encoded_bytes=" << total_encoded
            << " modes_0_1_2_3=" << mode_counts[0] << ","
            << mode_counts[1] << "," << mode_counts[2] << ","
            << mode_counts[3]
            << " decode_ns=" << total_ns
            << " decode_mib_s=" << (decoded_mib / seconds)
            << " checksum=" << checksum << "\n";
  return 0;
}
