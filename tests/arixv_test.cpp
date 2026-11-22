#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
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
      0x41, 0x52, 0x58, 0x32, 0x02, 0x00, 0x10, 0x6c, 0xe3, 0x3f, 0xa3,
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
      0x41, 0x52, 0x58, 0x32, 0x02, 0x41, 0x13, 0x4d, 0x8c, 0x88, 0x20, 0x29,
      0xe9, 0x42, 0x6f, 0x9b, 0x80, 0xd7, 0x84, 0x1e, 0xa1, 0x73, 0x03, 0x90,
      0x7d, 0xf7, 0x84, 0xe7, 0x82, 0x3c, 0xb9, 0x98, 0xb6, 0x49, 0xf3, 0x7a,
      0x19, 0x91, 0x03, 0x5e, 0xbd, 0xd8, 0xdc, 0xb9, 0x3b, 0x16, 0x8b, 0x9a,
      0x43, 0x0a, 0x92, 0x11, 0x88, 0x2a, 0x1e, 0x6f,
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

void CompactSizeEdgeCases() {
  // Raw byte-size boundaries across 1-, 2-, and 3-byte compact encodings.
  const std::vector<size_t> byte_sizes = {
      0, 1, 127, 128, 191, 192, 255, 256, 15551, 15552, 16384, 81087, 81088,
  };
  for (size_t sz : byte_sizes) {
    // Use high-entropy pattern so default Encode picks stored mode and we can
    // also verify the exact compact size field length.
    std::vector<uint8_t> input(sz);
    std::mt19937 rng(static_cast<uint32_t>(0x13579BDFu + sz));
    for (uint8_t& b : input) {
      b = static_cast<uint8_t>(rng());
    }
    const std::vector<uint8_t> stored =
        arixv::Encode(input.data(), input.size());
    assert(ContainerMode(stored) == arixv::kModeStored);
    const size_t expected_size_bytes =
        sz < 192u ? 1 : (sz < 15552u ? 2 : (sz < 81088u ? 3 : 4));
    assert(stored.size() ==
           arixv::kContainerMagicBytes + 1 + expected_size_bytes + sz);
    assert(stored.size() <= sz + 10);
    std::vector<uint8_t> decoded;
    assert(arixv::Decode(stored.data(), stored.size(), decoded) ==
           arixv::Status::kOk);
    assert(decoded == input);

    // Also round-trip smaller/boundary sizes via force_coded (kModeCoded).
    if (sz <= 16384) {
      RoundTripBytes(input, true);
    }
  }

  // SPIR-V body-word boundaries (0, 1, 191, 192, 15551, 15552 body words).
  const std::vector<size_t> body_word_counts = {0, 1, 191, 192, 15551, 15552};
  for (size_t body_words : body_word_counts) {
    std::vector<uint8_t> spirv(20 + body_words * 4, 0);
    // SPIR-V magic (LE), version 1.3, generator 0, bound 1, schema 0.
    spirv[0] = 0x03;
    spirv[1] = 0x02;
    spirv[2] = 0x23;
    spirv[3] = 0x07;
    spirv[5] = 0x03;
    spirv[6] = 0x01;
    spirv[12] = 0x01;
    // Fill body with OpNop (word_count=1, opcode=0 -> 0x00010000).
    for (size_t w = 0; w < body_words; ++w) {
      spirv[20 + w * 4 + 2] = 0x01;
    }
    arixv::EncodeOptions opts;
    opts.force_coded = true;
    const std::vector<uint8_t> encoded =
        arixv::Encode(spirv.data(), spirv.size(), opts);
    assert(ContainerMode(encoded) == arixv::kModeSpirv);
    std::vector<uint8_t> decoded;
    assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
           arixv::Status::kOk);
    assert(decoded == spirv);
  }
}

void HostileInputs() {
  auto make_blob = [](std::initializer_list<uint8_t> tail) {
    std::vector<uint8_t> v(arixv::kContainerMagic,
                           arixv::kContainerMagic + arixv::kContainerMagicBytes);
    v.insert(v.end(), tail.begin(), tail.end());
    return v;
  };

  auto check_all_prefixes_fail = [](const std::vector<uint8_t>& valid) {
    for (size_t prefix = 0; prefix < valid.size(); ++prefix) {
      // Fresh copy of exact prefix length so ASan catches any OOB read.
      std::vector<uint8_t> trunc(valid.begin(), valid.begin() + prefix);
      std::vector<uint8_t> out;
      const arixv::Status status =
          arixv::Decode(trunc.data(), trunc.size(), out);
      assert(status != arixv::Status::kOk);
      assert(out.empty());
    }
  };

  const std::vector<uint8_t> input = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  arixv::EncodeOptions opts;
  opts.force_coded = true;
  const std::vector<uint8_t> valid =
      arixv::Encode(input.data(), input.size(), opts);
  check_all_prefixes_fail(valid);

  // Exact 4-byte magic without mode byte must return kTruncated safely.
  {
    std::vector<uint8_t> just_magic = make_blob({});
    std::vector<uint8_t> out;
    assert(arixv::Decode(just_magic.data(), just_magic.size(), out) ==
           arixv::Status::kTruncated);
    assert(out.empty());
  }

  // Every prefix of a SPIR-V blob (both header-only and full module).
  {
    const std::vector<uint8_t> hdr_only = {
        0x03, 0x02, 0x23, 0x07, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const std::vector<uint8_t> enc_hdr =
        arixv::Encode(hdr_only.data(), hdr_only.size(), opts);
    check_all_prefixes_fail(enc_hdr);
    std::vector<uint8_t> padded_hdr = enc_hdr;
    padded_hdr.push_back(0);
    std::vector<uint8_t> out;
    assert(arixv::Decode(padded_hdr.data(), padded_hdr.size(), out) ==
           arixv::Status::kBadContainer);
    assert(out.empty());
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
  assert(ContainerMode(stored) == arixv::kModeStored);
  check_all_prefixes_fail(stored);

  // Truncated multi-byte compact size headers.
  for (const auto& trunc_hdr : {
           make_blob({arixv::kModeStored}),
           make_blob({arixv::kModeStored, 0xC0}),
           make_blob({arixv::kModeStored, 0xFC, 0x00}),
           make_blob({arixv::kModeStored, 0xFD, 0x00, 0x00}),
           make_blob({arixv::kModeStored, 0xFE, 0x00, 0x00, 0x00}),
       }) {
    assert(arixv::Decode(trunc_hdr.data(), trunc_hdr.size(), out) ==
           arixv::Status::kTruncated);
    assert(out.empty());
  }

  // Invalid compact size tag 0xFF and non-canonical 0xFE (< 16,858,304).
  for (const auto& bad_hdr : {
           make_blob({arixv::kModeStored, 0xFF}),
           make_blob({arixv::kModeSpirv, 0xFF}),
           make_blob({arixv::kModeStored, 0xFE, 0x00, 0x00, 0x00, 0x00}),
           make_blob({arixv::kModeStored, 0xFE, 0xBF, 0x3C, 0x01, 0x01}),
       }) {
    assert(arixv::Decode(bad_hdr.data(), bad_hdr.size(), out) ==
           arixv::Status::kBadContainer);
    assert(out.empty());
  }

  // Huge claimed sizes (OOM protection and SPIR-V uint32 overflow check).
  {
    // UINT32_MAX in stored mode -> kTooLarge.
    const auto huge_stored =
        make_blob({arixv::kModeStored, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF});
    assert(arixv::Decode(huge_stored.data(), huge_stored.size(), out) ==
           arixv::Status::kTooLarge);
    assert(out.empty());

    // UINT32_MAX in coded mode -> kTooLarge.
    const auto huge_coded =
        make_blob({arixv::kModeCoded, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF});
    assert(arixv::Decode(huge_coded.data(), huge_coded.size(), out) ==
           arixv::Status::kTooLarge);
    assert(out.empty());

    // body_words overflowing uint32_t bytes in SPIR-V mode -> kBadContainer.
    // kMaxSpirvBodyWords = (0xFFFFFFFF - 20) / 4 = 0x3FFFFFFA; 0x3FFFFFFB overflows.
    const auto overflow_spirv =
        make_blob({arixv::kModeSpirv, 0xFE, 0xFB, 0xFF, 0xFF, 0x3F});
    assert(arixv::Decode(overflow_spirv.data(), overflow_spirv.size(), out) ==
           arixv::Status::kBadContainer);
    assert(out.empty());

    // Max valid body_words (0x3FFFFFFA -> ~4 GiB) exceeds default 1 GiB limit -> kTooLarge.
    const auto max_spirv =
        make_blob({arixv::kModeSpirv, 0xFE, 0xFA, 0xFF, 0xFF, 0x3F});
    assert(arixv::Decode(max_spirv.data(), max_spirv.size(), out) ==
           arixv::Status::kTooLarge);
    assert(out.empty());
  }

  const auto unknown = make_blob({99, 0});
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

void ConcurrentEncodeDecode() {
  const std::vector<uint32_t> words = {
      0x07230203u, 0x00010300u, 0x0008000bu, 15, 0,
      (2u << 16) | 17, 1,
      (3u << 16) | 14, 0, 1,
      (6u << 16) | 15, 4, 1, 0x6e69616d, 0, 2,
      (3u << 16) | 16, 1, 7,
      (4u << 16) | 5, 2, 0x0074756f, 0,
      (4u << 16) | 71, 2, 30, 0,
      (2u << 16) | 19, 3,
      (3u << 16) | 33, 4, 3,
      (3u << 16) | 22, 5, 32,
      (4u << 16) | 23, 6, 5, 4,
      (4u << 16) | 32, 7, 3, 6,
      (4u << 16) | 59, 7, 2, 3,
      (4u << 16) | 43, 5, 8, 0x3f800000u,
      (7u << 16) | 44, 6, 9, 8, 8, 8, 8,
      (5u << 16) | 54, 3, 1, 0, 4,
      (2u << 16) | 248, 10,
      (3u << 16) | 62, 2, 9,
      (1u << 16) | 253,
      (1u << 16) | 56,
  };
  std::vector<uint8_t> spirv(words.size() * 4);
  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t w = words[i];
    spirv[i * 4 + 0] = static_cast<uint8_t>(w & 0xffu);
    spirv[i * 4 + 1] = static_cast<uint8_t>((w >> 8) & 0xffu);
    spirv[i * 4 + 2] = static_cast<uint8_t>((w >> 16) & 0xffu);
    spirv[i * 4 + 3] = static_cast<uint8_t>((w >> 24) & 0xffu);
  }
  const std::vector<uint8_t> baseline =
      arixv::Encode(spirv.data(), spirv.size());

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&spirv, &baseline]() {
      for (int i = 0; i < 16; ++i) {
        const std::vector<uint8_t> encoded =
            arixv::Encode(spirv.data(), spirv.size());
        assert(encoded == baseline);
        std::vector<uint8_t> decoded;
        assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
               arixv::Status::kOk);
        assert(decoded == spirv);
      }
    });
  }
  for (std::thread& th : threads) {
    th.join();
  }
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
  CompactSizeEdgeCases();
  HostileInputs();
  DecoderCompatShim();
  ConcurrentEncodeDecode();
  return 0;
}
