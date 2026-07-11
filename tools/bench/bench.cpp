#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "arixv.h"

namespace {

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

bool ParseDecodeIters(const char* arg, int& iters) {
  const std::string s(arg);
  const std::string prefix = "--decode-iters=";
  if (s.rfind(prefix, 0) != 0) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(s.c_str() + prefix.size(), &end, 10);
  if (end == s.c_str() + prefix.size() || *end != '\0' || value <= 0) {
    return false;
  }
  iters = static_cast<int>(value);
  return true;
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

}  // namespace

int main(int argc, char** argv) {
  int decode_iters = 20;
  uint64_t checksum = 0;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--decode-iters=", 0) == 0) {
      if (!ParseDecodeIters(argv[i], decode_iters)) {
        std::cerr << "bad --decode-iters value\n";
        return 1;
      }
      continue;
    }
    paths.push_back(arg);
  }
  if (paths.empty()) {
    std::cerr << "usage: arixv_bench [--decode-iters=N] <files...>\n";
    return 1;
  }

  for (const std::string& path : paths) {
    std::vector<uint8_t> input;
    if (!ReadFile(path, input)) {
      std::cerr << "failed to read " << path << "\n";
      return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    const std::vector<uint8_t> encoded = arixv::Encode(input.data(), input.size());
    const auto end = std::chrono::steady_clock::now();
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();

    std::vector<uint8_t> decoded;
    const arixv::Status status =
        arixv::Decode(encoded.data(), encoded.size(), decoded);
    if (status != arixv::Status::kOk || decoded != input) {
      std::cerr << "decode verify failed for " << path << "\n";
      return 1;
    }

    int64_t decode_total_ns = 0;
    for (int iter = 0; iter < decode_iters; ++iter) {
      decoded.clear();
      const auto decode_start = std::chrono::steady_clock::now();
      const arixv::Status iter_status =
          arixv::Decode(encoded.data(), encoded.size(), decoded);
      const auto decode_end = std::chrono::steady_clock::now();
      if (iter_status != arixv::Status::kOk ||
          decoded.size() != input.size()) {
        std::cerr << "decode failed for " << path << "\n";
        return 1;
      }
      decode_total_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end -
                                                               decode_start)
              .count();
      checksum += Sample(decoded) + static_cast<uint64_t>(iter);
    }

    std::cout << path << '\t' << input.size() << '\t' << encoded.size() << '\t'
              << ns << '\t' << (decode_total_ns / decode_iters) << '\n';
  }
  std::cerr << "checksum=" << checksum << "\n";
  return 0;
}
