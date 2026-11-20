#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "arixv.h"
#include "model.h"
#include "arixv_decoder_compat.h"
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
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  assert(size >= 0);
  in.seekg(0, std::ios::beg);
  data.resize(static_cast<size_t>(size));
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), size);
  }
  return static_cast<bool>(in);
}

void RoundTripBytes(const std::vector<uint8_t>& input, bool force_coded) {
  arixv::EncodeOptions opts;
  opts.force_coded = force_coded;
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size(), opts);
  (void)ContainerMode(encoded);
  std::vector<uint8_t> decoded;
  assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == input);
}

void BasicRoundTrips() {
  RoundTripBytes({}, false);
  RoundTripBytes({}, true);
  RoundTripBytes({0xA5}, false);
  RoundTripBytes({0xA5}, true);

  const std::string text =
      "SPIR-V byte fallback model test. Repeated text compresses enough to "
      "exercise coded mode without relying on SPIR-V structure.";
  RoundTripBytes(std::vector<uint8_t>(text.begin(), text.end()), false);
  RoundTripBytes(std::vector<uint8_t>(text.begin(), text.end()), true);
}

void GoldenSpirvEncoding() {
  // Minimal little-endian SPIR-V module: header only, version 1.3, bound 1.
  // This is an exact ARX2 format vector. Any byte-format change must use a
  // new magic and update this vector for that format. The tail is the rANS
  // stream (version major/minor and the tool ID are symbol-coded).
  const std::vector<uint8_t> input = {
      0x03, 0x02, 0x23, 0x07, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const std::vector<uint8_t> expected = {
      0x41, 0x52, 0x58, 0x32, 0x02, 0x14, 0x0f, 0x44, 0x98,
      0x06, 0xac, 0x38, 0x05, 0x00,
  };
  arixv::EncodeOptions options;
  options.force_coded = true;
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size(), options);
  assert(encoded == expected);
  assert(ContainerMode(encoded) == arixv::kModeSpirv);

  std::vector<uint8_t> decoded;
  assert(arixv::Decode(expected.data(), expected.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == input);
}

void GoldenSpirvModuleEncoding() {
  // A minimal but complete fragment shader (19 instructions): capability,
  // memory model, entry point with a name string, execution mode, OpName,
  // a decoration, void/function/float/vector/pointer types, a constant and
  // a composite, an output variable, and a function storing to it. Unlike
  // the header-only vector above it exercises the opcode trees, word counts,
  // result IDs, ranked ID/type references, enums, literals, strings, and
  // both entropy streams. This is an exact ARX2 format vector: any
  // byte-format change must use a new magic and update this vector.
  const std::vector<uint32_t> words = {
      0x07230203, 0x00010000, 0, 11, 0,         // header, bound 11
      (2u << 16) | 17, 1,                       // OpCapability Shader
      (3u << 16) | 14, 0, 1,                    // OpMemoryModel Logical GLSL450
      (6u << 16) | 15, 4, 1, 0x6e69616d, 0, 2,  // OpEntryPoint Fragment %1 "main" %2
      (3u << 16) | 16, 1, 7,                    // OpExecutionMode %1 OriginUpperLeft
      (4u << 16) | 5, 1, 0x6e69616d, 0,         // OpName %1 "main"
      (4u << 16) | 71, 2, 30, 0,                // OpDecorate %2 Location 0
      (2u << 16) | 19, 3,                       // %3 = OpTypeVoid
      (3u << 16) | 33, 4, 3,                    // %4 = OpTypeFunction %3
      (3u << 16) | 22, 5, 32,                   // %5 = OpTypeFloat 32
      (4u << 16) | 23, 6, 5, 4,                 // %6 = OpTypeVector %5 4
      (4u << 16) | 32, 7, 3, 6,                 // %7 = OpTypePointer Output %6
      (4u << 16) | 59, 7, 2, 3,                 // %2 = OpVariable %7 Output
      (4u << 16) | 43, 5, 8, 0x3f800000,        // %8 = OpConstant %5 1.0
      (7u << 16) | 44, 6, 9, 8, 8, 8, 8,        // %9 = OpConstantComposite %6 %8 x4
      (5u << 16) | 54, 3, 1, 0, 4,              // %1 = OpFunction %3 None %4
      (2u << 16) | 248, 10,                     // %10 = OpLabel
      (3u << 16) | 62, 2, 9,                    // OpStore %2 %9
      (1u << 16) | 253,                         // OpReturn
      (1u << 16) | 56,                          // OpFunctionEnd
  };
  std::vector<uint8_t> input;
  for (uint32_t w : words) {
    for (int i = 0; i < 4; ++i) {
      input.push_back(static_cast<uint8_t>(w >> (8 * i)));
    }
  }
  const std::vector<uint8_t> expected = {
      0x41, 0x52, 0x58, 0x32, 0x02, 0x98, 0x02, 0x13, 0x4d, 0x57, 0x2f, 0x46,
      0x62, 0x70, 0xe1, 0x26, 0x83, 0x39, 0x2e, 0x5e, 0xc0, 0x62, 0xf0, 0x7f,
      0x8a, 0x21, 0x70, 0xc2, 0x23, 0xc5, 0x82, 0x7f, 0xb6, 0xd0, 0x03, 0xc6,
      0x84, 0x3e, 0x62, 0xc0, 0x23, 0x3d, 0x88, 0x94, 0x06, 0xcb, 0x8e, 0xd3,
      0x08, 0x93, 0x01, 0x00, 0x29, 0xf1, 0x3a, 0x39, 0x1a, 0x8c, 0x2e, 0x30,
      0x6d, 0x13, 0x11, 0x43, 0x67, 0x35, 0x45, 0xbc, 0x02, 0x1d, 0x09, 0x01,
      0x00,
  };
  arixv::EncodeOptions options;
  options.force_coded = true;
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size(), options);
  assert(encoded == expected);
  assert(ContainerMode(encoded) == arixv::kModeSpirv);

  std::vector<uint8_t> decoded;
  assert(arixv::Decode(expected.data(), expected.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == input);
}

void RandomRoundTrips() {
  std::mt19937 rng(0xC001D00Du);
  for (size_t size : {size_t{1024}, size_t{1024 * 1024}}) {
    std::vector<uint8_t> input(size);
    for (uint8_t& b : input) {
      b = static_cast<uint8_t>(rng());
    }
    // Hard expansion contract: default encoding falls back to
    // stored mode, output <= input + 10 bytes.
    const std::vector<uint8_t> stored =
        arixv::Encode(input.data(), input.size());
    assert(stored.size() <= input.size() + 10);

    // Forced-coded path: the accelerated early-adaptation schedule costs a
    // few percent on incompressible data (oscillation before saturation);
    // that trade buys ~4% absolute corpus ratio and only force_coded (a test
    // knob) ever exposes it. Bound it loosely so regressions still surface.
    arixv::EncodeOptions opts;
    opts.force_coded = true;
    const std::vector<uint8_t> encoded =
        arixv::Encode(input.data(), input.size(), opts);
    assert(encoded.size() <= input.size() + input.size() / 20 + 1024);
    std::vector<uint8_t> decoded;
    assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
           arixv::Status::kOk);
    assert(decoded == input);
    std::vector<uint8_t> stored_decoded;
    assert(arixv::Decode(stored.data(), stored.size(), stored_decoded) ==
           arixv::Status::kOk);
    assert(stored_decoded == input);
  }
}

void CorpusRoundTrips() {
  const std::filesystem::path dir = "corpus/spv";
  if (!std::filesystem::exists(dir)) {
    return;
  }
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".spv") {
      continue;
    }
    std::vector<uint8_t> input;
    assert(ReadFile(entry.path(), input));
    RoundTripBytes(input, false);
    RoundTripBytes(input, true);
    ++count;
  }
  assert(count > 0);
}

void StoredModeForIncompressible() {
  std::mt19937 rng(0x51525354u);
  std::vector<uint8_t> input(16384);
  for (uint8_t& b : input) {
    b = static_cast<uint8_t>(rng());
  }
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size());
  assert(!encoded.empty());
  assert(ContainerMode(encoded) == arixv::kModeStored);
  assert(encoded.size() <= input.size() + 10);
}

void HostileInputs() {
  const std::vector<uint8_t> input = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  arixv::EncodeOptions opts;
  opts.force_coded = true;
  const std::vector<uint8_t> valid =
      arixv::Encode(input.data(), input.size(), opts);
  for (size_t prefix = 0; prefix < valid.size(); ++prefix) {
    std::vector<uint8_t> out;
    const arixv::Status status = arixv::Decode(valid.data(), prefix, out);
    assert(status != arixv::Status::kOk);
  }
  std::vector<uint8_t> padded = valid;
  padded.push_back(0);
  std::vector<uint8_t> out;
  assert(arixv::Decode(padded.data(), padded.size(), out) ==
         arixv::Status::kBadContainer);

  std::vector<uint8_t> wrong_magic = valid;
  wrong_magic[0] ^= 1;
  assert(arixv::Decode(wrong_magic.data(), wrong_magic.size(), out) ==
         arixv::Status::kBadContainer);
  assert(out.empty());

  std::vector<uint8_t> stored =
      arixv::Encode(input.data(), input.size(), arixv::EncodeOptions{});
  stored[arixv::kContainerMagicBytes] = arixv::kModeStored;
  for (size_t prefix = 0; prefix < stored.size(); ++prefix) {
    std::vector<uint8_t> out;
    const arixv::Status status = arixv::Decode(stored.data(), prefix, out);
    assert(status != arixv::Status::kOk);
  }

  std::vector<uint8_t> huge(arixv::kContainerMagic,
                            arixv::kContainerMagic + arixv::kContainerMagicBytes);
  huge.insert(huge.end(), {arixv::kModeStored, 0x80, 0x80, 0x80, 0x80,
                            0x80, 0x20});
  const arixv::Status status = arixv::Decode(huge.data(), huge.size(), out);
  assert(status == arixv::Status::kBadContainer ||
         status == arixv::Status::kTooLarge);

  std::vector<uint8_t> unknown(arixv::kContainerMagic,
                               arixv::kContainerMagic + arixv::kContainerMagicBytes);
  unknown.insert(unknown.end(), {99, 0});
  assert(arixv::Decode(unknown.data(), unknown.size(), out) ==
         arixv::Status::kBadContainer);
}

// The compatibility shim (arixv_decoder_compat.h) must behave exactly like
// stateless Decode, however many calls go through one instance.
void ExpectShimDecode(arixv::Decoder& decoder,
                      const std::vector<uint8_t>& encoded,
                      const std::vector<uint8_t>& expected) {
  std::vector<uint8_t> via_shim;
  assert(decoder.Decode(encoded.data(), encoded.size(), via_shim) ==
         arixv::Status::kOk);
  assert(via_shim == expected);

  std::vector<uint8_t> stateless;
  assert(arixv::Decode(encoded.data(), encoded.size(), stateless) ==
         arixv::Status::kOk);
  assert(via_shim == stateless);
}

void DecoderCompatShim() {
  arixv::Decoder decoder;
  arixv::EncodeOptions opts;
  opts.force_coded = true;

  const std::string text1 =
      "first shim decoder byte payload, deliberately repeated repeated";
  const std::vector<uint8_t> bytes1(text1.begin(), text1.end());
  const std::vector<uint8_t> encoded1 =
      arixv::Encode(bytes1.data(), bytes1.size(), opts);
  assert(ContainerMode(encoded1) == arixv::kModeCoded);
  ExpectShimDecode(decoder, encoded1, bytes1);

  std::vector<uint8_t> bad = encoded1;
  bad.push_back(0);
  std::vector<uint8_t> out;
  assert(decoder.Decode(bad.data(), bad.size(), out) ==
         arixv::Status::kBadContainer);
  assert(out.empty());
  // The shim forwards max_output too.
  assert(decoder.Decode(encoded1.data(), encoded1.size(), out, 4) ==
         arixv::Status::kTooLarge);
  assert(out.empty());

  // Movable like the old reusable decoder.
  arixv::Decoder moved = std::move(decoder);
  decoder = std::move(moved);

  const std::string text2 =
      "second payload uses the same decoder after an error path";
  const std::vector<uint8_t> bytes2(text2.begin(), text2.end());
  const std::vector<uint8_t> encoded2 =
      arixv::Encode(bytes2.data(), bytes2.size(), opts);
  assert(ContainerMode(encoded2) == arixv::kModeCoded);
  ExpectShimDecode(decoder, encoded2, bytes2);

  const std::filesystem::path dir = "corpus/spv";
  if (!std::filesystem::exists(dir)) {
    return;
  }
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".spv") {
      continue;
    }
    std::vector<uint8_t> input;
    assert(ReadFile(entry.path(), input));
    const std::vector<uint8_t> encoded =
        arixv::Encode(input.data(), input.size(), opts);
    assert(ContainerMode(encoded) == arixv::kModeSpirv);
    ExpectShimDecode(decoder, encoded, input);
    if (++count == 2) {
      break;
    }
  }
  assert(count >= 2);
}

}  // namespace

int main() {
  assert(arixv::PrimedStringMatchTableIsConsistent());
  BasicRoundTrips();
  GoldenSpirvEncoding();
  GoldenSpirvModuleEncoding();
  RandomRoundTrips();
  CorpusRoundTrips();
  StoredModeForIncompressible();
  HostileInputs();
  DecoderCompatShim();
  return 0;
}
