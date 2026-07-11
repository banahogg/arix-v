#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "arixv.h"
#include "model.h"
#include "tuning.h"

namespace {

uint8_t ContainerMode(const std::vector<uint8_t>& encoded) {
  assert(encoded.size() > arixv::kContainerMagicBytes);
  assert(std::equal(arixv::kContainerMagic,
                    arixv::kContainerMagic + arixv::kContainerMagicBytes,
                    encoded.begin()));
  return encoded[arixv::kContainerMagicBytes];
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  data.assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return in.good() || in.eof();
}

void AppendWord(uint32_t word, std::vector<uint8_t>& out) {
  out.push_back(static_cast<uint8_t>(word & 0xffu));
  out.push_back(static_cast<uint8_t>((word >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>((word >> 16) & 0xffu));
  out.push_back(static_cast<uint8_t>((word >> 24) & 0xffu));
}

std::vector<uint8_t> Module(const std::vector<uint32_t>& body,
                            uint32_t version = 0x00010300u,
                            uint32_t generator = 0,
                            uint32_t bound = 1,
                            uint32_t schema = 0) {
  std::vector<uint8_t> out;
  AppendWord(0x07230203u, out);
  AppendWord(version, out);
  AppendWord(generator, out);
  AppendWord(bound, out);
  AppendWord(schema, out);
  for (uint32_t word : body) {
    AppendWord(word, out);
  }
  return out;
}

uint32_t Op(uint16_t opcode, uint16_t words) {
  return (static_cast<uint32_t>(words) << 16) | opcode;
}

void AppendStringWords(const std::string& text, std::vector<uint32_t>& out,
                       uint32_t final_padding = 0) {
  std::string bytes = text;
  bytes.push_back('\0');
  while ((bytes.size() & 3u) != 0) {
    bytes.push_back('\0');
  }
  if (final_padding != 0 && !bytes.empty()) {
    const size_t base = bytes.size() - 4;
    for (int i = 0; i < 4; ++i) {
      const uint8_t b = static_cast<uint8_t>((final_padding >> (i * 8)) & 0xffu);
      if (b != 0 && bytes[base + static_cast<size_t>(i)] == '\0') {
        bytes[base + static_cast<size_t>(i)] = static_cast<char>(b);
      }
    }
  }
  for (size_t i = 0; i < bytes.size(); i += 4) {
    uint32_t word = 0;
    for (int b = 0; b < 4; ++b) {
      word |= static_cast<uint32_t>(
                  static_cast<uint8_t>(bytes[i + static_cast<size_t>(b)]))
              << (b * 8);
    }
    out.push_back(word);
  }
}

void RoundTrip(const std::vector<uint8_t>& input, bool force_coded,
               int expected_mode = -1) {
  arixv::EncodeOptions opts;
  opts.force_coded = force_coded;
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size(), opts);
  assert(!encoded.empty());
  if (expected_mode >= 0) {
    assert(ContainerMode(encoded) == expected_mode);
  }
  std::vector<uint8_t> decoded;
  assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == input);

  if (arixv::IsSpirvModelEligible(input.data(), input.size())) {
    std::vector<uint8_t> raw;
    arixv::EncodeModuleRawResidual(input.data(), input.size(), raw);
    std::vector<uint8_t> raw_decoded;
    assert(arixv::DecodeModuleRawResidual(raw.data(), raw.size(), input.size(),
                                          raw_decoded) ==
           arixv::Status::kOk);
    assert(raw_decoded == input);
  }
}

std::vector<std::filesystem::path> CorpusFiles() {
  std::vector<std::filesystem::path> files;
  const std::filesystem::path dir = "corpus/spv";
  if (!std::filesystem::exists(dir)) {
    return files;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".spv") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void CorpusMode2RoundTrips() {
  std::vector<std::filesystem::path> files = CorpusFiles();
  if (files.empty()) {
    return;
  }
  assert(files.size() >= 1080);

  const std::filesystem::path swapped =
      "corpus/spv/119e12dcf8638cd31134f4a10f50be49da9c250b214f7c2dc3c47b994b557de1.spv";
  if (std::find(files.begin(), files.end(), swapped) == files.end()) {
    files.push_back(swapped);
    std::sort(files.begin(), files.end());
  }

  for (const auto& path : files) {
    std::vector<uint8_t> input;
    assert(ReadFile(path, input));
    RoundTrip(input, false);
  }

  std::vector<uint8_t> swapped_input;
  assert(ReadFile(swapped, swapped_input));
  assert((swapped_input.size() % 4) != 0);
  RoundTrip(swapped_input, true, arixv::kModeCoded);
}

void HostileModules() {
  const uint32_t op_capability = (2u << 16) | 17u;
  const uint32_t op_memory_model = (3u << 16) | 14u;
  const uint32_t op_type_void = (2u << 16) | 19u;
  const uint32_t op_type_int = (4u << 16) | 21u;
  const uint32_t op_unknown = (2u << 16) | 0x3FBDu;
  const uint32_t op_count_zero = 14u;
  const uint32_t op_bad_fixed_count = (4u << 16) | 14u;
  const uint32_t op_count_too_large = (20u << 16) | 17u;

  RoundTrip(Module({op_capability, 1, op_unknown, 0x12345678u,
                    op_memory_model, 0, 1},
                   0x00010300u, 0, 4),
            true, arixv::kModeSpirv);
  RoundTrip(Module({op_bad_fixed_count, 0, 1, 0xabcdef01u}, 0x00010300u, 0,
                   1),
            true, arixv::kModeSpirv);
  RoundTrip(Module({op_capability, 1, op_count_zero, op_memory_model, 0, 1},
                   0x00010300u, 0, 1),
            true, arixv::kModeSpirv);
  RoundTrip(Module({op_count_too_large, 1, 2, 3}, 0x00010300u, 0, 1), true,
            arixv::kModeSpirv);
  RoundTrip(Module({}), true, arixv::kModeSpirv);
  RoundTrip(Module({op_type_void, 7}, 0x00010300u, 0, 123), true,
            arixv::kModeSpirv);
  RoundTrip(Module({op_memory_model, 0, 1}, 0xDEADBEEFu, 0, 1), true,
            arixv::kModeSpirv);
  RoundTrip(Module({op_memory_model, 0, 1}, 0x00010300u, 0xFFFFFFFFu, 1),
            true, arixv::kModeSpirv);
  RoundTrip(Module({op_type_int, 2, 32, 1}, 0x00010300u, 0, 3), true,
            arixv::kModeSpirv);

  arixv::EncodeOptions opts;
  opts.force_coded = true;
  const std::vector<uint8_t> module =
      Module({op_memory_model, 0, 1}, 0x00010300u, 0, 1);
  std::vector<uint8_t> encoded =
      arixv::Encode(module.data(), module.size(), opts);
  assert(!encoded.empty());
  assert(ContainerMode(encoded) == arixv::kModeSpirv);
  encoded.push_back(0);
  std::vector<uint8_t> decoded;
  assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
         arixv::Status::kBadContainer);
}

void RawResidualValidation() {
  const std::vector<uint8_t> module =
      Module({Op(19, 2), 1, Op(21, 4), 2, 32, 0, Op(43, 4), 2, 3, 7},
             0x00010300u, 0, 4);
  std::vector<uint8_t> raw;
  arixv::EncodeModuleRawResidual(module.data(), module.size(), raw);
  assert(!raw.empty());

  for (size_t prefix = 0; prefix < raw.size(); ++prefix) {
    std::vector<uint8_t> decoded;
    assert(arixv::DecodeModuleRawResidual(raw.data(), prefix, module.size(),
                                          decoded) != arixv::Status::kOk);
    assert(decoded.empty());
  }

  std::vector<uint8_t> padded = raw;
  padded.push_back(0);
  std::vector<uint8_t> decoded;
  assert(arixv::DecodeModuleRawResidual(padded.data(), padded.size(),
                                        module.size(), decoded) ==
         arixv::Status::kBadContainer);
  assert(decoded.empty());

  arixv::RawResidualModuleDecoder reusable;
  assert(reusable.Decode(raw.data(), raw.size(), module.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == module);
  assert(reusable.Decode(padded.data(), padded.size(), module.size(), decoded) ==
         arixv::Status::kBadContainer);
  assert(decoded.empty());
  assert(reusable.Decode(raw.data(), raw.size(), module.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == module);
}

void M3FieldModules() {
  {
    std::vector<uint32_t> body = {
        Op(21, 4), 1, 64, 1,
        Op(43, 5), 1, 2, 0xffffffffu, 0xffffffffu,
        Op(43, 5), 1, 3, 0xffffffffu, 0u,
    };
    RoundTrip(Module(body, 0x00010300u, 0, 4), true, arixv::kModeSpirv);
  }
  {
    std::vector<uint32_t> body = {
        Op(21, 4), 1, 64, 0,
        Op(43, 5), 1, 2, 7, 0,
        Op(251, 9), 2, 3, 10, 4, 11, 5, 12, 6,
    };
    RoundTrip(Module(body, 0x00010300u, 0, 7), true, arixv::kModeSpirv);
  }
  {
    std::vector<uint32_t> body;
    body.push_back(Op(7, 3));
    body.push_back(1);
    AppendStringWords("ab", body, 0x58000000u);
    body.push_back(Op(7, 4));
    body.push_back(2);
    AppendStringWords("abcd", body);
    body.push_back(Op(7, 3));
    body.push_back(3);
    AppendStringWords("ab", body, 0x58000000u);
    RoundTrip(Module(body, 0x00010300u, 0, 4), true, arixv::kModeSpirv);
  }
  {
    std::vector<uint32_t> body;
    body.push_back(Op(11, 6));
    body.push_back(1);
    AppendStringWords("GLSL.std.450", body);
    body.push_back(Op(12, 6));
    body.push_back(2);
    body.push_back(3);
    body.push_back(1);
    body.push_back(13);
    body.push_back(4);
    body.push_back(Op(12, 6));
    body.push_back(2);
    body.push_back(5);
    body.push_back(99);
    body.push_back(12345);
    body.push_back(6);
    RoundTrip(Module(body, 0x00010300u, 0, 100), true, arixv::kModeSpirv);
  }
  {
    std::vector<uint32_t> body = {
        Op(71, 3), 200, 9999,
        Op(19, 2), 1,
        Op(33, 3), 2, 1,
        Op(54, 5), 1, 3, 0, 2,
        Op(248, 2), 10,
        Op(245, 7), 1, 11, 11, 10, 12, 10,
        Op(245, 7), 1, 12, 11, 10, 12, 10,
        Op(249, 2), 10,
        Op(56, 1),
    };
    RoundTrip(Module(body, 0x00010300u, 0, 201), true, arixv::kModeSpirv);
  }
  {
    std::vector<uint32_t> body = {
        Op(71, 3), 1, 0x12345678u,
        Op(21, 4), 1, 32, 0,
        Op(61, 6), 1, 2, 3, 0x80000002u, 16,
    };
    RoundTrip(Module(body, 0x00010300u, 0, 4), true, arixv::kModeSpirv);
  }
}

void M4aModules() {
  {
    std::vector<uint32_t> body = {
        Op(19, 2), 1,
        Op(21, 4), 2, 32, 0,
        Op(33, 3), 3, 1,
        Op(43, 4), 2, 4, 1,
        Op(43, 4), 2, 5, 2,
        Op(54, 5), 1, 6, 0, 3,
        Op(248, 2), 10,
        Op(249, 2), 13,
        Op(248, 2), 11,
        Op(249, 2), 13,
        Op(248, 2), 12,
        Op(249, 2), 13,
        Op(248, 2), 13,
        Op(245, 9), 2, 14, 4, 10, 5, 11, 14, 12,
        Op(245, 9), 2, 15, 14, 10, 5, 11, 4, 12,
        Op(249, 2), 16,
        Op(248, 2), 16,
        Op(253, 1),
        Op(56, 1),
    };
    RoundTrip(Module(body, 0x00010300u, 0, 17), true, arixv::kModeSpirv);
  }
  {
    std::vector<uint32_t> body = {
        Op(21, 4), 1, 32, 0,
        Op(43, 4), 1, 2, 3,
        Op(251, 15), 2, 100, 0, 101, 1, 102, 2, 103, 3, 104, 4, 105, 5,
        106,
        Op(248, 2), 100,
        Op(248, 2), 101,
        Op(248, 2), 102,
        Op(248, 2), 103,
        Op(248, 2), 104,
        Op(248, 2), 105,
        Op(248, 2), 106,
    };
    RoundTrip(Module(body, 0x00010300u, 0, 107), true, arixv::kModeSpirv);
  }
  {
    std::vector<uint32_t> body = {
        Op(21, 4), 1, 32, 0,
        Op(43, 4), 1, 2, 1,
        Op(43, 4), 1, 3, 2,
        Op(128, 5), 1, 4, 2, 3,
        Op(128, 5), 1, 5, 4, 3,
        Op(128, 5), 1, 6, 5, 4,
        Op(128, 5), 1, 7, 6, 5,
        Op(128, 5), 1, 8, 7, 6,
        Op(128, 5), 1, 9, 8, 7,
    };
    RoundTrip(Module(body, 0x00010300u, 0, 10), true, arixv::kModeSpirv);
  }
}

void MutatedCorpusRoundTrips() {
  std::vector<std::filesystem::path> files = CorpusFiles();
  if (files.empty()) {
    return;
  }
  assert(files.size() >= 10);
  files.resize(10);

  std::vector<std::vector<uint8_t>> pool;
  for (const auto& path : files) {
    std::vector<uint8_t> input;
    assert(ReadFile(path, input));
    pool.push_back(std::move(input));
  }

  std::mt19937_64 rng(0x4d324d5554415445ull);
  for (int iter = 0; iter < 300; ++iter) {
    std::vector<uint8_t> input = pool[static_cast<size_t>(rng() % pool.size())];
    const int op = static_cast<int>(rng() % 3);
    if (op == 0 && input.size() >= 4) {
      const uint32_t hostile[] = {0u, 0xffffffffu, 0x0000ffffu,
                                  0xffff0000u, 0x00010000u,
                                  0x000400ffu};
      const uint32_t value = hostile[rng() % 6];
      const size_t words = input.size() / 4;
      AppendWord(value, input);
      std::copy(input.end() - 4, input.end(),
                input.begin() + static_cast<size_t>(rng() % words) * 4);
      input.resize(input.size() - 4);
    } else if (op == 1) {
      input.resize(static_cast<size_t>(rng() % (input.size() + 1)));
    } else if (input.size() >= 8) {
      const size_t words = input.size() / 4;
      const size_t src = static_cast<size_t>(rng() % words);
      const size_t len = 1 + static_cast<size_t>(
                                 rng() % std::min<size_t>(words - src, 8));
      const size_t dst = static_cast<size_t>(rng() % words) * 4;
      std::vector<uint8_t> chunk(input.begin() + src * 4,
                                 input.begin() + (src + len) * 4);
      input.insert(input.begin() + dst, chunk.begin(), chunk.end());
    }
    RoundTrip(input, true);
  }
}

}  // namespace

int main() {
  CorpusMode2RoundTrips();
  HostileModules();
  RawResidualValidation();
  M3FieldModules();
  M4aModules();
  MutatedCorpusRoundTrips();
  return 0;
}
