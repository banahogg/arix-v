#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "arixv.h"
#include "model.h"
#include "prob.h"
#include "tracker.h"
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

  // A rejected decode leaves nothing behind for the next call.
  assert(arixv::DecodeModuleRawResidual(raw.data(), raw.size(), module.size(),
                                        decoded) == arixv::Status::kOk);
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

void LiteralModelingModules() {
  // Float32 constants covering all bit-pattern classes (+0, -0, normal powers,
  // fractional mantissas, subnormals, infinities, quiet/signaling NaNs,
  // all-ones), plus non-canonical float16 upper bits and 64-bit float64.
  {
    std::vector<uint32_t> body = {
        Op(22, 3), 1, 32,  // %1 = OpTypeFloat 32
        Op(22, 3), 2, 16,  // %2 = OpTypeFloat 16
        Op(22, 3), 3, 64,  // %3 = OpTypeFloat 64
    };
    const uint32_t f32_bits[] = {
        0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3f000000u,
        0x3ec00000u, 0x00000001u, 0x007fffffu, 0x80000001u, 0x7f800000u,
        0xff800000u, 0x7f800001u, 0x7fc00000u, 0xffffffffu, 0x3eaaaaabu,
    };
    uint32_t id = 4;
    for (uint32_t bits : f32_bits) {
      body.insert(body.end(), {Op(43, 4), 1, id++, bits});  // OpConstant
    }
    body.insert(body.end(), {Op(50, 4), 1, id++, 0x40490fdbu});  // OpSpecConstant
    // float16 with nonzero high 16 bits must survive bit-identically.
    body.insert(body.end(), {Op(43, 4), 2, id++, 0xdead3c00u});
    // float64 2-word constant.
    body.insert(body.end(), {Op(43, 5), 3, id++, 0x54442d18u, 0x400921fbu});
    RoundTrip(Module(body, 0x00010300u, 0, id), true, arixv::kModeSpirv);
  }
  // Multi-type integer constants exceeding IntConstCache::kSize (8) plus
  // wrapping deltas.
  {
    std::vector<uint32_t> body;
    uint32_t id = 1;
    for (uint32_t t = 0; t < 12; ++t) {
      body.insert(body.end(), {Op(21, 4), id++, 32, t & 1u});
    }
    for (uint32_t round = 0; round < 3; ++round) {
      for (uint32_t t = 1; t <= 12; ++t) {
        const uint32_t val =
            (round == 2 && (t & 1u)) ? (0xffffffffu - t) : (t * 100u + round);
        body.insert(body.end(), {Op(43, 4), t, id++, val});
      }
    }
    RoundTrip(Module(body, 0x00010300u, 0, id), true, arixv::kModeSpirv);
  }
  // Enum-conditioned literal strides across OpDecorate / OpMemberDecorate,
  // including >32 distinct decoration enums to exercise cache collisions.
  {
    std::vector<uint32_t> body;
    for (uint32_t i = 0; i < 6; ++i) {
      body.insert(body.end(), {Op(71, 4), 10 + i, 30, i});       // Location
      body.insert(body.end(), {Op(71, 4), 20 + i, 33, i * 2});   // Binding
      body.insert(body.end(), {Op(71, 4), 20 + i, 34, 0});       // DescriptorSet
      body.insert(body.end(), {Op(72, 5), 1, i, 35, i * 16});    // Member Offset
    }
    for (uint32_t e = 0; e < 40; ++e) {
      body.insert(body.end(), {Op(71, 4), 50, e, e * 3});
    }
    RoundTrip(Module(body, 0x00010300u, 0, 60), true, arixv::kModeSpirv);
  }
}

void Order1OpcodePromotionModules() {
  // Exercises order-1 opcode shadow training -> promotion (past
  // kOpcodeOrder1MinCount = 4), lazy leaf-group creation both before and after
  // promotion, and the same prev_key (OpVariable, OpNop) crossing module
  // sections so current-section leaf priors are tested on encode and decode.
  std::vector<uint32_t> body = {
      Op(17, 2), 1,         // OpCapability Shader
      Op(0, 1),             // OpNop
      Op(14, 3), 0, 1,      // OpMemoryModel Logical GLSL450
      Op(19, 2), 1,         // %1 = OpTypeVoid
      Op(33, 3), 2, 1,      // %2 = OpTypeFunction %1
      Op(21, 4), 3, 32, 0,  // %3 = OpTypeInt 32 0
      Op(32, 4), 4, 7, 3,   // %4 = OpTypePointer Function %3
      Op(32, 4), 5, 6, 3,   // %5 = OpTypePointer Private %3
  };
  uint32_t id = 6;
  // Promote OpVariable in kTypesConstantsVars (> 4 consecutive occurrences).
  const uint32_t first_var = id;
  for (int i = 0; i < 6; ++i) {
    body.insert(body.end(), {Op(59, 4), 5, id++, 6});  // OpVariable Private
  }
  // OpNop chain crossing into kFunctionBodies.
  for (int i = 0; i < 6; ++i) {
    body.push_back(Op(0, 1));  // OpNop
  }
  body.insert(body.end(), {
      Op(54, 5), 1, id++, 0, 2,  // OpFunction
      Op(248, 2), id++,          // OpLabel
  });
  // Same prev_key (OpVariable) now in kFunctionBodies, followed by body ops
  // that open new leaf groups under the promoted order-1 tree.
  const uint32_t local_var = id;
  for (int i = 0; i < 3; ++i) {
    body.insert(body.end(), {Op(59, 4), 4, id++, 7});  // OpVariable Function
  }
  // Repeated OpLoad -> OpIAdd -> OpStore sequence crossing promotion threshold.
  for (int i = 0; i < 10; ++i) {
    const uint32_t loaded = id++;
    const uint32_t added = id++;
    body.insert(body.end(), {Op(61, 4), 3, loaded, first_var});         // OpLoad
    body.insert(body.end(), {Op(128, 5), 3, added, loaded, loaded});    // OpIAdd
    body.insert(body.end(), {Op(62, 3), local_var, added});             // OpStore
  }
  body.push_back(Op(0, 1));    // OpNop in kFunctionBodies -> OpReturn
  body.push_back(Op(253, 1));  // OpReturn
  body.push_back(Op(56, 1));   // OpFunctionEnd
  RoundTrip(Module(body, 0x00010300u, 0, id), true, arixv::kModeSpirv);
}

void KindRecencyListTests() {
  // Direct Tracker invariants for kind lists, kRecentTypesList aliasing,
  // forward references, Reset(), and hostile redefinitions.
  {
    arixv::Tracker tracker;
    const auto feed = [&](const std::vector<uint32_t>& words,
                          uint32_t first_id_op = 0) {
      const uint16_t opcode = static_cast<uint16_t>(words[0] & 0xffffu);
      const arixv::InstructionDesc* desc = arixv::FindInstruction(opcode);
      const int idx = arixv::FindInstructionIndex(opcode);
      const uint16_t inst_index =
          idx >= 0 ? static_cast<uint16_t>(idx) : arixv::NumCoreInstructions();
      tracker.OnInstructionStart(opcode, desc,
                                 static_cast<uint32_t>(words.size()));
      tracker.OnInstructionEnd(words.data(),
                               static_cast<uint32_t>(words.size()), inst_index,
                               first_id_op);
    };

    // Forward reference to an undefined label (%50) and variable (%20) has
    // type_list == 0.
    assert(tracker.OnIdUse(50).type_list == 0);
    assert(tracker.OnIdUse(20).type_list == 0);

    // Types alias recent_types_ via kRecentTypesList.
    feed({Op(19, 2), 1});             // %1 = OpTypeVoid
    feed({Op(22, 3), 2, 32});         // %2 = OpTypeFloat 32
    feed({Op(21, 4), 3, 32, 0});      // %3 = OpTypeInt 32 0
    feed({Op(32, 4), 4, 7, 2}, 2);    // %4 = OpTypePointer Function %2
    feed({Op(32, 4), 5, 7, 3}, 3);    // %5 = OpTypePointer Function %3
    feed({Op(33, 3), 6, 1}, 1);       // %6 = OpTypeFunction %1
    feed({Op(33, 3), 7, 2}, 2);       // %7 = OpTypeFunction %2
    const auto* t2 = tracker.Find(2);
    assert(t2 != nullptr && t2->kind == arixv::Tracker::IdKind::kType);
    assert(t2->type_list == arixv::Tracker::kRecentTypesList);
    assert(tracker.TypedList(t2->type_list) == &tracker.recent_types());
    assert(tracker.TypedList(0) == nullptr);

    // Variables of distinct pointer types (%4 vs %5) share one unified
    // kVariable list, without creating per-pointer-type lists.
    feed({Op(59, 4), 4, 20, 7});      // %20 = OpVariable %4 Function
    feed({Op(59, 4), 5, 21, 7});      // %21 = OpVariable %5 Function
    const auto* v20 = tracker.Find(20);
    const auto* v21 = tracker.Find(21);
    assert(v20 != nullptr && v21 != nullptr);
    assert(v20->kind == arixv::Tracker::IdKind::kVariable);
    assert(v21->kind == arixv::Tracker::IdKind::kVariable);
    assert(v20->type_list != 0 && v20->type_list == v21->type_list);
    assert(tracker.Find(4)->values_list == 0);
    assert(tracker.Find(5)->values_list == 0);
    const arixv::RecencyList* var_list = tracker.TypedList(v20->type_list);
    assert(var_list != nullptr && var_list->size() == 2);
    assert(var_list->at(0) == 21 && var_list->at(1) == 20);
    assert(tracker.OnIdUse(20).type_list == v20->type_list);
    assert(var_list->at(0) == 20);

    // Functions of distinct return types (%1 void vs %2 float) share one
    // unified kFunction list.
    feed({Op(54, 5), 1, 30, 0, 6}, 6);  // %30 = OpFunction %1 None %6
    feed({Op(56, 1)});                  // OpFunctionEnd
    feed({Op(54, 5), 2, 31, 0, 7}, 7);  // %31 = OpFunction %2 None %7
    const auto* f30 = tracker.Find(30);
    const auto* f31 = tracker.Find(31);
    assert(f30 != nullptr && f31 != nullptr);
    assert(f30->type_list != 0 && f30->type_list == f31->type_list);
    assert(f30->type_list != v20->type_list);

    // Ext-inst sets share a dedicated kExtInstSet list.
    std::vector<uint32_t> ext1 = {Op(11, 6), 40};
    AppendStringWords("GLSL.std.450", ext1);
    feed(ext1);
    std::vector<uint32_t> ext2 = {Op(11, 5), 41};
    AppendStringWords("OpenCL.std", ext2);
    feed(ext2);
    const auto* e40 = tracker.Find(40);
    const auto* e41 = tracker.Find(41);
    assert(e40 != nullptr && e41 != nullptr);
    assert(e40->type_list != 0 && e40->type_list == e41->type_list);
    assert(e40->type_list != v20->type_list &&
           e40->type_list != f30->type_list);

    // Labels keep type_list == 0 (forward-ref and after definition).
    feed({Op(248, 2), 50});  // %50 = OpLabel
    const auto* l50 = tracker.Find(50);
    assert(l50 != nullptr && l50->kind == arixv::Tracker::IdKind::kLabel);
    assert(l50->type_list == 0);

    // Hostile redefinition: redefining a kType ID (%2) as untyped OpString
    // must clear type_list to 0 (no stale kRecentTypesList handle).
    std::vector<uint32_t> str_redef = {Op(7, 3), 2};
    AppendStringWords("x", str_redef);
    feed(str_redef);
    const auto* redef2 = tracker.Find(2);
    assert(redef2 != nullptr && redef2->kind == arixv::Tracker::IdKind::kValue);
    assert(redef2->type_list == 0);
    assert(tracker.OnIdUse(2).type_list == 0);

    // Reset() clears kind_lists_ and typed_lists_ cleanly.
    tracker.Reset();
    assert(tracker.TypedList(1) == nullptr);
    assert(tracker.TypedList(arixv::Tracker::kRecentTypesList) ==
           &tracker.recent_types());
    assert(tracker.recent_types().size() == 0);
    feed({Op(22, 3), 1, 32});
    feed({Op(32, 4), 2, 7, 1}, 1);
    feed({Op(59, 4), 2, 3, 7});
    assert(tracker.Find(3)->type_list == 1);
  }

  // Round-trip module exercising repeated type operands, multiple ext-inst
  // sets, variables of multiple pointer types, functions of multiple return
  // types, and forward- and backward-referenced labels.
  {
    std::vector<uint32_t> body;
    body.insert(body.end(), {Op(11, 6), 1});
    AppendStringWords("GLSL.std.450", body);
    body.insert(body.end(), {Op(11, 5), 2});
    AppendStringWords("OpenCL.std", body);
    body.insert(body.end(), {
        Op(19, 2), 3,                     // %3 = OpTypeVoid
        Op(22, 3), 4, 32,                 // %4 = OpTypeFloat 32
        Op(21, 4), 5, 32, 0,              // %5 = OpTypeInt 32 0
        Op(20, 2), 6,                     // %6 = OpTypeBool
        Op(23, 4), 7, 4, 4,               // %7 = OpTypeVector %4 4
        Op(23, 4), 8, 5, 4,               // %8 = OpTypeVector %5 4
        Op(30, 4), 9, 4, 5,               // %9 = OpTypeStruct %4 %5
        Op(30, 4), 10, 7, 8,              // %10 = OpTypeStruct %7 %8
        Op(32, 4), 11, 7, 4,              // %11 = OpTypePointer Function %4
        Op(32, 4), 12, 7, 5,              // %12 = OpTypePointer Function %5
        Op(32, 4), 13, 7, 7,              // %13 = OpTypePointer Function %7
        Op(32, 4), 14, 7, 9,              // %14 = OpTypePointer Function %9
        Op(33, 3), 15, 3,                 // %15 = OpTypeFunction %3
        Op(33, 3), 16, 4,                 // %16 = OpTypeFunction %4
        Op(33, 3), 17, 5,                 // %17 = OpTypeFunction %5
        Op(43, 4), 4, 18, 0x3f800000u,    // %18 = OpConstant %4 1.0
        Op(43, 4), 5, 19, 1,              // %19 = OpConstant %5 1
        Op(41, 3), 6, 20,                 // %20 = OpConstantTrue %6
        // Helper functions with distinct return types (%4 float, %5 uint).
        Op(54, 5), 4, 21, 0, 16,
        Op(248, 2), 22,
        Op(254, 2), 18,
        Op(56, 1),
        Op(54, 5), 5, 23, 0, 17,
        Op(248, 2), 24,
        Op(254, 2), 19,
        Op(56, 1),
        // Main function with multiple pointer-type variables, interleaved
        // OpLoad/OpStore/OpFunctionCall/OpExtInst, and forward/back labels.
        Op(54, 5), 3, 25, 0, 15,
        Op(248, 2), 26,
        Op(59, 4), 11, 27, 7,             // var float*
        Op(59, 4), 12, 28, 7,             // var uint*
        Op(59, 4), 13, 29, 7,             // var vec4*
        Op(59, 4), 14, 30, 7,             // var struct*
        Op(62, 3), 27, 18,                // OpStore %27 %18
        Op(62, 3), 28, 19,                // OpStore %28 %19
        Op(249, 2), 31,                   // OpBranch %31 (forward ref)
        Op(248, 2), 31,                   // %31 = OpLabel (loop header)
        Op(245, 7), 4, 32, 18, 26, 35, 31,  // OpPhi with fwd %35 & back %31
        Op(61, 4), 4, 33, 27,             // OpLoad %27
        Op(61, 4), 5, 34, 28,             // OpLoad %28
        Op(12, 6), 4, 35, 1, 4, 33,       // OpExtInst %1 (GLSL) FAbs
        Op(12, 6), 4, 36, 1, 4, 35,       // OpExtInst %1 (GLSL) FAbs
        Op(12, 6), 4, 37, 2, 16, 36,      // OpExtInst %2 (OpenCL)
        Op(57, 4), 4, 38, 21,             // OpFunctionCall %21
        Op(57, 4), 5, 39, 23,             // OpFunctionCall %23
        Op(57, 4), 4, 40, 21,             // OpFunctionCall %21
        Op(62, 3), 27, 38,                // OpStore %27 %38
        Op(62, 3), 28, 39,                // OpStore %28 %39
        Op(250, 4), 20, 31, 41,           // OpBranchConditional %20 %31(back) %41(fwd)
        Op(248, 2), 41,                   // %41 = OpLabel
        Op(253, 1),                       // OpReturn
        Op(56, 1),                        // OpFunctionEnd
    });
    RoundTrip(Module(body, 0x00010300u, 0, 42), true, arixv::kModeSpirv);
  }

  // Hostile round-trip: redefines IDs across kType, kVariable, kFunction,
  // kExtInstSet, kLabel, and untyped OpString, referencing them throughout.
  {
    std::vector<uint32_t> body = {
        Op(22, 3), 5, 32,              // %5 = OpTypeFloat 32 (kType)
        Op(32, 4), 6, 7, 5,            // %6 = OpTypePointer Function %5
        Op(32, 4), 7, 7, 5,            // uses %5 (kRecentTypesList)
        Op(59, 4), 6, 5, 7,            // redefine %5 as OpVariable
        Op(61, 4), 5, 8, 5,            // uses %5 (kVariable)
        Op(248, 2), 5,                 // redefine %5 as OpLabel (type_list=0)
        Op(249, 2), 5,                 // uses %5 (type_list=0)
        Op(54, 5), 6, 5, 0, 7,         // redefine %5 as OpFunction
        Op(57, 4), 6, 9, 5,            // uses %5 (kFunction)
        Op(22, 3), 5, 32,              // redefine %5 as OpTypeFloat again
        Op(7, 3), 5, 0x00000061u,      // redefine %5 as OpString (type_list=0)
        Op(62, 3), 5, 8,               // uses %5 (type_list=0)
    };
    RoundTrip(Module(body, 0x00010300u, 0, 10), true, arixv::kModeSpirv);
  }
}

void ColdStartPriorAndBoundaryTests() {
  // Constexpr SymbolCdf / UintModel length priors must match SymbolCdfSetPrior
  // byte-for-byte.
  {
    const uint32_t top_w[4] = {16, 16, 1, 0};
    arixv::SymbolCdf expected_root;
    arixv::SymbolCdfSetPrior<4>(expected_root, top_w);
    assert(std::memcmp(arixv::kUint32LengthRootPrior.d, expected_root.d,
                       sizeof(expected_root.d)) == 0);

    const uint32_t leaf32_w[16] = {1, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0};
    arixv::SymbolCdf expected_leaf32;
    arixv::SymbolCdfSetPrior<16>(expected_leaf32, leaf32_w);
    assert(std::memcmp(arixv::kUint32LengthLeaf32Prior.d, expected_leaf32.d,
                       sizeof(expected_leaf32.d)) == 0);

    const arixv::BitTree<6> tree32 = arixv::MakeUintLengthTreePrior(0, 32);
    const arixv::UintModel fresh;
    if constexpr (arixv::kUintLengthBits == 6) {
      assert(std::memcmp(fresh.length.tables.data(), tree32.tables.data(),
                         sizeof(tree32.tables)) == 0);
    }

    const arixv::BitTree<6> exp_tree = arixv::MakeUintLengthTreePrior(0, 9);
    const uint32_t exp_top[4] = {10, 0, 0, 0};
    const uint32_t exp_leaf0[16] = {1, 1, 1, 1, 1, 1, 1, 1,
                                    1, 1, 0, 0, 0, 0, 0, 0};
    arixv::SymbolCdf exp_r, exp_l0, zero{};
    arixv::SymbolCdfSetPrior<4>(exp_r, exp_top);
    arixv::SymbolCdfSetPrior<16>(exp_l0, exp_leaf0);
    assert(std::memcmp(exp_tree.tables[0].d, exp_r.d, sizeof(exp_r.d)) == 0);
    assert(std::memcmp(exp_tree.tables[1].d, exp_l0.d, sizeof(exp_l0.d)) == 0);
    assert(std::memcmp(exp_tree.tables[2].d, zero.d, sizeof(zero.d)) == 0);
    assert(std::memcmp(exp_tree.tables[3].d, zero.d, sizeof(zero.d)) == 0);
    assert(std::memcmp(exp_tree.tables[4].d, zero.d, sizeof(zero.d)) == 0);

    const arixv::BitTree<6> mant_tree = arixv::MakeUintLengthTreePrior(1, 23);
    const uint32_t mant_top[4] = {15, 8, 0, 0};
    const uint32_t mant_leaf0[16] = {0, 1, 1, 1, 1, 1, 1, 1,
                                     1, 1, 1, 1, 1, 1, 1, 1};
    const uint32_t mant_leaf1[16] = {1, 1, 1, 1, 1, 1, 1, 1,
                                     0, 0, 0, 0, 0, 0, 0, 0};
    arixv::SymbolCdf mant_r, mant_l0, mant_l1;
    arixv::SymbolCdfSetPrior<4>(mant_r, mant_top);
    arixv::SymbolCdfSetPrior<16>(mant_l0, mant_leaf0);
    arixv::SymbolCdfSetPrior<16>(mant_l1, mant_leaf1);
    assert(std::memcmp(mant_tree.tables[0].d, mant_r.d, sizeof(mant_r.d)) == 0);
    assert(std::memcmp(mant_tree.tables[1].d, mant_l0.d, sizeof(mant_l0.d)) == 0);
    assert(std::memcmp(mant_tree.tables[2].d, mant_l1.d, sizeof(mant_l1.d)) == 0);
    assert(std::memcmp(mant_tree.tables[3].d, zero.d, sizeof(zero.d)) == 0);
    assert(std::memcmp(mant_tree.tables[4].d, zero.d, sizeof(zero.d)) == 0);
  }

  // 32-bit and 0 uint boundary values across bit widths 0, 1, 15, 16, 31, 32
  // (plus float32 extreme exponents 0x00/0xff and mantissas 1/23 bits).
  {
    std::vector<uint32_t> body = {
        Op(21, 4), 1, 32, 0,  // %1 = OpTypeInt 32 0
        Op(22, 3), 2, 32,     // %2 = OpTypeFloat 32
    };
    const uint32_t boundary_uints[] = {
        0u,          1u,          2u,          0x7fffu,
        0x8000u,     0xffffu,     0x7fffffffu, 0x80000000u,
        0xffffffffu, 0u,          0xffffffffu, 0x80000000u,
    };
    uint32_t id = 3;
    for (uint32_t v : boundary_uints) {
      body.insert(body.end(), {Op(43, 4), 1, id++, v});       // OpConstant int
      body.insert(body.end(), {Op(71, 4), 1, 35, v});         // OpDecorate Offset
    }
    const uint32_t boundary_floats[] = {
        0x00000000u,  // +0.0 (exp=0, mant=0)
        0x80000000u,  // -0.0 (exp=0 -> delta=-127 -> width 8)
        0x00000001u,  // min subnormal (exp=0, rmant bitwidth 23)
        0x00400000u,  // subnormal (exp=0, rmant bitwidth 1)
        0x7f800000u,  // +inf (exp=255 -> delta=+128 -> width 9)
        0xff800000u,  // -inf (exp=255)
        0x7fffffffu,  // NaN all-ones mantissa (exp=255, rmant bitwidth 23)
        0xffc00000u,  // quiet -NaN (exp=255, rmant bitwidth 1)
    };
    for (uint32_t f : boundary_floats) {
      body.insert(body.end(), {Op(43, 4), 2, id++, f});
    }
    // Also exercise unknown enum escape and large result-ID jump + unregistered
    // generator ID and mismatched bound (0xffffffffu).
    body.insert(body.end(), {Op(71, 3), 1, 0xffffffffu});
    body.insert(body.end(), {Op(71, 3), 1, 0x80000000u});
    body.insert(body.end(), {Op(43, 4), 1, 0x80000000u, 0xffffffffu});
    body.insert(body.end(), {Op(43, 4), 1, 0xffffffffu, 0u});
    RoundTrip(Module(body, 0x00010300u, 0xffff0000u, 0xffffffffu), true,
              arixv::kModeSpirv);
  }

  // Non-ASCII, binary, empty, and hostile string round-trips exercising every
  // byte 0x00..0xff (all 16 order0 high-nibble leaves, both primer-present and
  // primer-absent), byte-0 cross-string boundaries, non-canonical padding, and
  // unterminated strings.
  {
    std::vector<uint32_t> body;
    uint32_t id = 1;
    const auto add_op_string = [&](const std::string& text,
                                   uint32_t final_padding = 0) {
      std::vector<uint32_t> str_words;
      AppendStringWords(text, str_words, final_padding);
      body.push_back(Op(7, static_cast<uint16_t>(2 + str_words.size())));
      body.push_back(id++);
      body.insert(body.end(), str_words.begin(), str_words.end());
    };

    // Empty string (NUL at byte 0).
    add_op_string("");
    // Single-byte non-ASCII and control strings.
    add_op_string(std::string(1, '\x01'));
    add_op_string(std::string(1, '\x7f'));
    add_op_string(std::string(1, '\x80'));
    add_op_string(std::string(1, '\xff'));
    // Every non-zero byte 0x01..0xff in one canonical string (touches all 16
    // high-nibble leaves of StringModel::order0).
    std::string all_bytes;
    all_bytes.reserve(255);
    for (int b = 1; b <= 255; ++b) {
      all_bytes.push_back(static_cast<char>(static_cast<uint8_t>(b)));
    }
    add_op_string(all_bytes);
    // Repeated UTF-8 and binary strings exercising order-2 matches + byte-0
    // boundary between adjacent strings.
    add_op_string("\xc3\xa9\xe2\x9c\x93\xf0\x9f\x9a\x80/path/to/shader.glsl");
    add_op_string("\xc3\xa9\xe2\x9c\x93\xf0\x9f\x9a\x80/path/to/other.glsl");
    add_op_string("\xff\xfe\xfd\xfc\x01\x02\x03\x04\x80\x90\xa0\xb0");
    // Non-canonical trailing bytes after NUL.
    add_op_string("\x80\xff", 0xaa000000u);
    // Hostile unterminated OpString (no NUL byte anywhere in its words).
    body.push_back(Op(7, 4));
    body.push_back(id++);
    body.push_back(0xdeadbeefu);
    body.push_back(0x80818283u);

    RoundTrip(Module(body, 0x00010300u, 0, id), true, arixv::kModeSpirv);
  }
}

void AdaptiveIdListSourceTests() {
  // 1. Polymorphic in-block chains exercising all adaptive list-source modes:
  //    - id_idx == 0 hysteresis (own_last vs prev_res_list) across float/vec4
  //      OpFMul/OpFAdd chains, then resetting across non-result OpStore/OpNop.
  //    - id_idx > 0 modes: op0_list (OpPhi, OpFAdd), prev_op_list (OpSelect,
  //      OpCompositeConstruct), elem(prev_op)_list (OpStore alternating
  //      float*/vec4*/uint* pointers), prev_res_list (OpSelect op1, OpExtInst
  //      with op0_type == 0), and own_last fallback when chosen_list == 0.
  //    - Variadic shared-slot instructions (OpCompositeConstruct) with >16 ID
  //      operands (exercising Slot16 clamping and num_id_operands cap at 8).
  {
    std::vector<uint32_t> body;
    body.insert(body.end(), {Op(11, 6), 1});  // %1 = OpExtInstImport "GLSL.std.450"
    AppendStringWords("GLSL.std.450", body);
    body.insert(body.end(), {
        Op(19, 2), 2,                      // %2 = OpTypeVoid
        Op(20, 2), 3,                      // %3 = OpTypeBool
        Op(22, 3), 4, 32,                  // %4 = OpTypeFloat 32
        Op(21, 4), 5, 32, 0,               // %5 = OpTypeInt 32 0
        Op(23, 4), 6, 4, 4,                // %6 = OpTypeVector %4 4
        Op(23, 4), 7, 5, 4,                // %7 = OpTypeVector %5 4
        Op(32, 4), 8, 7, 4,                // %8 = OpTypePointer Function %4
        Op(32, 4), 9, 7, 6,                // %9 = OpTypePointer Function %6
        Op(32, 4), 10, 7, 5,               // %10 = OpTypePointer Function %5
        Op(33, 3), 11, 2,                  // %11 = OpTypeFunction %2
        Op(41, 3), 3, 12,                  // %12 = OpConstantTrue %3
        Op(43, 4), 4, 13, 0x3f800000u,     // %13 = OpConstant %4 1.0
        Op(43, 4), 4, 14, 0x40000000u,     // %14 = OpConstant %4 2.0
        Op(43, 4), 5, 15, 20u,             // %15 = OpConstant %5 20
        Op(43, 4), 5, 16, 7u,              // %16 = OpConstant %5 7
        Op(28, 4), 17, 4, 15,              // %17 = OpTypeArray %4 %15 (len 20)
        Op(44, 7), 6, 18, 13, 14, 13, 14,  // %18 = OpConstantComposite %6
        Op(44, 7), 7, 19, 15, 16, 15, 16,  // %19 = OpConstantComposite %7
        // Function definition.
        Op(54, 5), 2, 20, 0, 11,           // %20 = OpFunction %2 None %11
        Op(248, 2), 21,                    // %21 = OpLabel
        Op(59, 4), 8, 22, 7,               // %22 = OpVariable %8 (float*)
        Op(59, 4), 9, 23, 7,               // %23 = OpVariable %9 (vec4*)
        Op(59, 4), 10, 24, 7,              // %24 = OpVariable %10 (uint*)
    });

    uint32_t next_id = 25;
    // Polymorphic OpStore alternating float*, vec4*, uint* (elem(prev_op)).
    for (int i = 0; i < 6; ++i) {
      body.insert(body.end(), {
          Op(62, 3), 22, 13,  // OpStore float* float
          Op(62, 3), 23, 18,  // OpStore vec4*  vec4
          Op(62, 3), 24, 15,  // OpStore uint*  uint
      });
    }

    // Polymorphic SSA chains alternating float and vec4 so id_idx == 0 adapts
    // to prev_res_list (cnt: 1 -> 2) and OpFAdd/OpExtInst/OpSelect exercise
    // op0_list, prev_op_list, and prev_res_list.
    uint32_t cur_f = 13;
    uint32_t cur_v = 18;
    for (int i = 0; i < 8; ++i) {
      const uint32_t m_f = next_id++;
      const uint32_t a_f = next_id++;
      const uint32_t e_f = next_id++;
      const uint32_t s_f = next_id++;
      body.insert(body.end(), {
          Op(133, 5), 4, m_f, cur_f, 14,       // OpFMul float
          Op(129, 5), 4, a_f, m_f, cur_f,      // OpFAdd float (op0 = prev_res)
          Op(12, 6), 4, e_f, 1, 4, a_f,        // OpExtInst FAbs (op0_type == 0)
          Op(169, 6), 4, s_f, 12, e_f, cur_f,  // OpSelect bool float float
      });
      cur_f = s_f;

      const uint32_t m_v = next_id++;
      const uint32_t a_v = next_id++;
      const uint32_t e_v = next_id++;
      const uint32_t s_v = next_id++;
      body.insert(body.end(), {
          Op(133, 5), 6, m_v, cur_v, 18,       // OpFMul vec4
          Op(129, 5), 6, a_v, m_v, cur_v,      // OpFAdd vec4 (op0 = prev_res)
          Op(12, 6), 6, e_v, 1, 4, a_v,        // OpExtInst FAbs vec4
          Op(169, 6), 6, s_v, 12, e_v, cur_v,  // OpSelect bool vec4 vec4
      });
      cur_v = s_v;

      // Non-result instruction clears prev_inst_result_id; then a steady-type
      // OpFAdd sequence lets id_idx == 0 also adapt back toward own_last.
      body.insert(body.end(), {Op(62, 3), 22, cur_f, Op(0, 1)});
    }

    // Variadic OpCompositeConstruct sharing field.slot across id_idx == 0 and
    // id_idx > 0, alternating float/uint vectors, plus a 20-operand array
    // construct (>16 operands, testing Slot16 clamp and num_id_operands cap).
    for (int i = 0; i < 4; ++i) {
      const uint32_t cv = next_id++;
      const uint32_t cu = next_id++;
      body.insert(body.end(), {
          Op(80, 7), 6, cv, cur_f, 13, 14, cur_f,  // vec4 from floats
          Op(80, 7), 7, cu, 15, 16, 15, 16,        // uvec4 from uints
      });
    }
    const uint32_t arr_id = next_id++;
    body.insert(body.end(), {Op(80, 23), 17, arr_id});
    for (int k = 0; k < 20; ++k) {
      body.push_back((k & 1) ? 14u : cur_f);
    }

    // Branch into loop header with OpPhi (op0_list across zero-list labels).
    const uint32_t loop_lbl = next_id++;
    const uint32_t phi_f = next_id++;
    const uint32_t phi_v = next_id++;
    const uint32_t next_f = next_id++;
    const uint32_t next_v = next_id++;
    body.insert(body.end(), {
        Op(249, 2), loop_lbl,
        Op(248, 2), loop_lbl,
        Op(245, 7), 4, phi_f, cur_f, 21, next_f, loop_lbl,
        Op(245, 7), 6, phi_v, cur_v, 21, next_v, loop_lbl,
        Op(129, 5), 4, next_f, phi_f, 13,
        Op(129, 5), 6, next_v, phi_v, 18,
        Op(253, 1),
        Op(56, 1),
    });

    const std::vector<uint8_t> mod = Module(body, 0x00010300u, 0, next_id);
    RoundTrip(mod, true, arixv::kModeSpirv);

    // Sanity check: adaptive typed-list selection should compress this
    // repetitive polymorphic shader well below its uncompressed size.
    arixv::EncodeOptions opts;
    opts.force_coded = true;
    const std::vector<uint8_t> encoded =
        arixv::Encode(mod.data(), mod.size(), opts);
    assert(encoded.size() * 2 < mod.size());
  }

  // 2. Hostile / non-canonical edge cases for adaptive list-source tracking:
  //    - Result ID == 0 and IdRef == 0 at id_idx == 0 and id_idx > 0
  //    - Truncated/grammar-inconsistent instruction with kIdResult inside a
  //      block (prev_inst_result_id not inserted into tracker.ids_, so
  //      PrevResListSlow sees Find() == nullptr)
  //    - Forward-referenced IDs (type_list == 0) in both id_idx == 0 and > 0
  //    - OpExtInst referencing an undefined set ID (op0_type == 0)
  //    - Mid-block redefinition of prev_inst_result_id to untyped OpString
  {
    std::vector<uint32_t> body = {
        Op(19, 2), 1,                       // %1 = OpTypeVoid
        Op(22, 3), 2, 32,                   // %2 = OpTypeFloat 32
        Op(33, 3), 3, 1,                    // %3 = OpTypeFunction %1
        Op(54, 5), 1, 4, 0, 3,              // %4 = OpFunction
        Op(248, 2), 5,                      // %5 = OpLabel (enters block)
        // Result ID 0 inside block, followed by OpFAdd using ID 0.
        Op(129, 5), 2, 0, 99, 98,           // fwd refs %99, %98; res_id == 0
        Op(129, 5), 2, 6, 0, 0,             // uses ID 0 at id_idx 0 and 1
        // Grammar-inconsistent OpFAdd (word count 4 instead of fixed 5):
        // sets prev_inst_result_id = 777 without defining 777 in Tracker.
        Op(129, 4), 2, 777, 6,
        // Next instruction queries PrevResList(777) -> Find returns nullptr.
        Op(129, 5), 2, 7, 6, 6,
        // OpExtInst with undefined set ID %888 (op0_type == 0, op0_list == 0).
        Op(12, 6), 2, 8, 888, 4, 7,
        // Untyped OpString inside a block sets prev_inst_result_id = 9 with
        // type_list == 0, followed by OpStore and OpSelect.
        Op(7, 3), 9, 0x00000061u,
        Op(62, 3), 9, 7,
        Op(169, 6), 2, 10, 9, 7, 8,
        // Very wide variadic OpCompositeConstruct with 24 undefined/mixed IDs.
        Op(80, 27), 2, 11,
        0, 7, 8, 99, 98, 777, 888, 6, 7, 8, 10, 0,
        7, 8, 6, 9, 10, 0, 7, 8, 99, 6, 7, 8,
        Op(253, 1),
        Op(56, 1),
        // Post-function instruction must not inherit in-block prev_res_id.
        Op(62, 3), 7, 8,
    };
    RoundTrip(Module(body, 0x00010300u, 0, 900), true, arixv::kModeSpirv);
  }
}

}  // namespace

int main() {
  CorpusMode2RoundTrips();
  HostileModules();
  RawResidualValidation();
  M3FieldModules();
  M4aModules();
  LiteralModelingModules();
  Order1OpcodePromotionModules();
  KindRecencyListTests();
  ColdStartPriorAndBoundaryTests();
  AdaptiveIdListSourceTests();
  MutatedCorpusRoundTrips();
  return 0;
}
