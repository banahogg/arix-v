#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "arixv/arixv.h"
#include "container.h"

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

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
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

arixv_status EncodeBuffer(const std::vector<uint8_t>& input, bool force_coded,
                          std::vector<uint8_t>& out) {
  if (force_coded) {
    return arixv::EncodeInternal(input.data(), input.size(), true, out);
  }
  const size_t bound = arixv_encode_bound(input.size(), nullptr);
  if (bound == 0) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
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
  size_t required = 0;
  arixv_status status =
      arixv_decode(input.data(), input.size(), nullptr, &required, nullptr);
  if (status != ARIXV_STATUS_OK) {
    return status;
  }
  out.resize(required);
  uint8_t empty_sink = 0;
  void* dst = required == 0 ? static_cast<void*>(&empty_sink)
                            : static_cast<void*>(out.data());
  size_t out_size = required;
  status = arixv_decode(input.data(), input.size(), dst, &out_size, nullptr);
  if (status == ARIXV_STATUS_OK) {
    out.resize(out_size);
  }
  return status;
}

void Usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " c [--force-coded] <in> <out>\n"
            << "       " << argv0 << " d <in> <out>\n"
            << "       " << argv0
            << " roundtrip [--force-coded] <files...>\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 2;
  }

  const std::string cmd = argv[1];
  bool force_coded = false;
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--force-coded") {
      force_coded = true;
    } else {
      args.push_back(arg);
    }
  }

  if (cmd == "c") {
    if (args.size() != 2) {
      Usage(argv[0]);
      return 2;
    }
    std::vector<uint8_t> input;
    if (!ReadFile(args[0], input)) {
      std::cerr << "failed to read " << args[0] << "\n";
      return 1;
    }
    std::vector<uint8_t> encoded;
    const arixv_status status = EncodeBuffer(input, force_coded, encoded);
    if (status != ARIXV_STATUS_OK) {
      std::cerr << "encode failed: " << arixv_status_string(status) << "\n";
      return 1;
    }
    if (!WriteFile(args[1], encoded)) {
      std::cerr << "failed to write " << args[1] << "\n";
      return 1;
    }
    return 0;
  }

  if (cmd == "d") {
    if (args.size() != 2) {
      Usage(argv[0]);
      return 2;
    }
    std::vector<uint8_t> input;
    std::vector<uint8_t> decoded;
    if (!ReadFile(args[0], input)) {
      std::cerr << "failed to read " << args[0] << "\n";
      return 1;
    }
    const arixv_status status = DecodeBuffer(input, decoded);
    if (status != ARIXV_STATUS_OK) {
      std::cerr << "decode failed: " << arixv_status_string(status) << "\n";
      return 1;
    }
    if (!WriteFile(args[1], decoded)) {
      std::cerr << "failed to write " << args[1] << "\n";
      return 1;
    }
    return 0;
  }

  if (cmd == "roundtrip") {
    if (args.empty()) {
      Usage(argv[0]);
      return 2;
    }
    bool all_ok = true;
    for (const std::string& path : args) {
      std::vector<uint8_t> input;
      std::vector<uint8_t> decoded;
      bool ok = ReadFile(path, input);
      std::vector<uint8_t> encoded;
      if (ok) {
        ok = EncodeBuffer(input, force_coded, encoded) == ARIXV_STATUS_OK &&
             DecodeBuffer(encoded, decoded) == ARIXV_STATUS_OK &&
             decoded == input;
      }
      std::cout << path << "\t" << (ok ? "OK" : "FAIL") << "\t"
                << input.size() << "\t" << encoded.size() << "\n";
      all_ok = all_ok && ok;
    }
    return all_ok ? 0 : 1;
  }

  Usage(argv[0]);
  return 2;
}
