#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "arixv/arixv.h"
#include "container.h"
#include "model.h"
#include "test_util.h"
#include "tuning.h"

namespace {

uint8_t ContainerMode(const std::vector<uint8_t>& encoded) {
  CHECK(encoded.size() > arixv::kContainerMagicBytes);
  CHECK(std::equal(arixv::kContainerMagic,
                    arixv::kContainerMagic + arixv::kContainerMagicBytes,
                    encoded.begin()));
  return encoded[arixv::kContainerMagicBytes];
}

std::vector<uint8_t> EncodeBytes(const std::vector<uint8_t>& input,
                                 bool force_coded = false) {
  std::vector<uint8_t> out;
  if (force_coded) {
    CHECK(arixv::EncodeInternal(input.data(), input.size(), true, out) ==
          ARIXV_STATUS_OK);
    return out;
  }
  const size_t bound = arixv_encode_bound(input.size(), nullptr);
  CHECK(bound >= 6);
  out.resize(bound);
  size_t out_size = out.size();
  CHECK(arixv_encode(input.data(), input.size(), out.data(), &out_size,
                     nullptr) == ARIXV_STATUS_OK);
  out.resize(out_size);
  return out;
}

arixv_status DecodeBytes(const uint8_t* data, size_t size,
                         std::vector<uint8_t>& out) {
  out.clear();
  size_t required = 0;
  arixv_status st = arixv_decode(data, size, nullptr, &required, nullptr);
  if (st != ARIXV_STATUS_OK) {
    return st;
  }
  out.resize(required);
  uint8_t empty_sink = 0;
  void* dst = required == 0 ? static_cast<void*>(&empty_sink)
                            : static_cast<void*>(out.data());
  size_t out_size = required;
  st = arixv_decode(data, size, dst, &out_size, nullptr);
  if (st == ARIXV_STATUS_OK) {
    out.resize(out_size);
  } else {
    out.clear();
  }
  return st;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  CHECK(size >= 0);
  in.seekg(0, std::ios::beg);
  data.resize(static_cast<size_t>(size));
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), size);
  }
  return static_cast<bool>(in);
}

void RoundTripBytes(const std::vector<uint8_t>& input, bool force_coded) {
  const std::vector<uint8_t> encoded = EncodeBytes(input, force_coded);
  (void)ContainerMode(encoded);
  std::vector<uint8_t> decoded;
  CHECK(DecodeBytes(encoded.data(), encoded.size(), decoded) ==
        ARIXV_STATUS_OK);
  CHECK(decoded == input);
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
  const std::vector<uint8_t> encoded = EncodeBytes(input, true);
  CHECK(encoded == expected);
  CHECK(ContainerMode(encoded) == arixv::kModeSpirv);

  std::vector<uint8_t> decoded;
  CHECK(DecodeBytes(expected.data(), expected.size(), decoded) ==
        ARIXV_STATUS_OK);
  CHECK(decoded == input);
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
  const std::vector<uint8_t> encoded = EncodeBytes(input, true);
  CHECK(encoded == expected);
  CHECK(ContainerMode(encoded) == arixv::kModeSpirv);

  std::vector<uint8_t> decoded;
  CHECK(DecodeBytes(expected.data(), expected.size(), decoded) ==
        ARIXV_STATUS_OK);
  CHECK(decoded == input);
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
    const std::vector<uint8_t> stored = EncodeBytes(input, false);
    CHECK(stored.size() <= input.size() + 10);

    // Forced-coded path: the accelerated early-adaptation schedule costs a
    // few percent on incompressible data (oscillation before saturation);
    // that trade buys ~4% absolute corpus ratio and only force_coded (an
    // internal test knob) ever exposes it. Bound it loosely so regressions
    // still surface.
    const std::vector<uint8_t> encoded = EncodeBytes(input, true);
    CHECK(encoded.size() <= input.size() + input.size() / 20 + 1024);
    std::vector<uint8_t> decoded;
    CHECK(DecodeBytes(encoded.data(), encoded.size(), decoded) ==
          ARIXV_STATUS_OK);
    CHECK(decoded == input);
    std::vector<uint8_t> stored_decoded;
    CHECK(DecodeBytes(stored.data(), stored.size(), stored_decoded) ==
          ARIXV_STATUS_OK);
    CHECK(stored_decoded == input);
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
    CHECK(ReadFile(entry.path(), input));
    RoundTripBytes(input, false);
    RoundTripBytes(input, true);
    ++count;
  }
  CHECK(count > 0);
}

void StoredModeForIncompressible() {
  std::mt19937 rng(0x51525354u);
  std::vector<uint8_t> input(16384);
  for (uint8_t& b : input) {
    b = static_cast<uint8_t>(rng());
  }
  const std::vector<uint8_t> encoded = EncodeBytes(input, false);
  CHECK(!encoded.empty());
  CHECK(ContainerMode(encoded) == arixv::kModeStored);
  CHECK(encoded.size() <= input.size() + 10);
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
    const std::vector<uint8_t> stored = EncodeBytes(input, false);
    CHECK(ContainerMode(stored) == arixv::kModeStored);
    const size_t expected_size_bytes =
        sz < 192u ? 1 : (sz < 15552u ? 2 : (sz < 81088u ? 3 : 4));
    CHECK(stored.size() ==
          arixv::kContainerMagicBytes + 1 + expected_size_bytes + sz);
    CHECK(stored.size() == arixv_encode_bound(sz, nullptr));
    CHECK(stored.size() <= sz + 10);
    std::vector<uint8_t> decoded;
    CHECK(DecodeBytes(stored.data(), stored.size(), decoded) ==
          ARIXV_STATUS_OK);
    CHECK(decoded == input);

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
    const std::vector<uint8_t> encoded = EncodeBytes(spirv, true);
    CHECK(ContainerMode(encoded) == arixv::kModeSpirv);
    std::vector<uint8_t> decoded;
    CHECK(DecodeBytes(encoded.data(), encoded.size(), decoded) ==
          ARIXV_STATUS_OK);
    CHECK(decoded == spirv);
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
      const arixv_status status =
          DecodeBytes(trunc.data(), trunc.size(), out);
      CHECK(status != ARIXV_STATUS_OK);
      CHECK(out.empty());
    }
  };

  const std::vector<uint8_t> input = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  const std::vector<uint8_t> valid = EncodeBytes(input, true);
  check_all_prefixes_fail(valid);

  // Exact 4-byte magic without mode byte must return TRUNCATED safely.
  {
    std::vector<uint8_t> just_magic = make_blob({});
    std::vector<uint8_t> out;
    CHECK(DecodeBytes(just_magic.data(), just_magic.size(), out) ==
          ARIXV_STATUS_TRUNCATED);
    CHECK(out.empty());
  }

  // Every prefix of a SPIR-V blob (both header-only and full module).
  {
    const std::vector<uint8_t> hdr_only = {
        0x03, 0x02, 0x23, 0x07, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const std::vector<uint8_t> enc_hdr = EncodeBytes(hdr_only, true);
    check_all_prefixes_fail(enc_hdr);
    std::vector<uint8_t> padded_hdr = enc_hdr;
    padded_hdr.push_back(0);
    std::vector<uint8_t> out;
    CHECK(DecodeBytes(padded_hdr.data(), padded_hdr.size(), out) ==
          ARIXV_STATUS_BAD_CONTAINER);
    CHECK(out.empty());
  }

  std::vector<uint8_t> padded = valid;
  padded.push_back(0);
  std::vector<uint8_t> out;
  CHECK(DecodeBytes(padded.data(), padded.size(), out) ==
        ARIXV_STATUS_BAD_CONTAINER);

  std::vector<uint8_t> wrong_magic = valid;
  wrong_magic[0] ^= 1;
  CHECK(DecodeBytes(wrong_magic.data(), wrong_magic.size(), out) ==
        ARIXV_STATUS_BAD_CONTAINER);
  CHECK(out.empty());

  std::vector<uint8_t> stored = EncodeBytes(input, false);
  CHECK(ContainerMode(stored) == arixv::kModeStored);
  check_all_prefixes_fail(stored);

  // Truncated multi-byte compact size headers.
  for (const auto& trunc_hdr : {
           make_blob({arixv::kModeStored}),
           make_blob({arixv::kModeStored, 0xC0}),
           make_blob({arixv::kModeStored, 0xFC, 0x00}),
           make_blob({arixv::kModeStored, 0xFD, 0x00, 0x00}),
           make_blob({arixv::kModeStored, 0xFE, 0x00, 0x00, 0x00}),
       }) {
    CHECK(DecodeBytes(trunc_hdr.data(), trunc_hdr.size(), out) ==
          ARIXV_STATUS_TRUNCATED);
    CHECK(out.empty());
  }

  // Invalid compact size tag 0xFF and non-canonical 0xFE (< 16,858,304).
  for (const auto& bad_hdr : {
           make_blob({arixv::kModeStored, 0xFF}),
           make_blob({arixv::kModeSpirv, 0xFF}),
           make_blob({arixv::kModeStored, 0xFE, 0x00, 0x00, 0x00, 0x00}),
           make_blob({arixv::kModeStored, 0xFE, 0xBF, 0x3C, 0x01, 0x01}),
       }) {
    CHECK(DecodeBytes(bad_hdr.data(), bad_hdr.size(), out) ==
          ARIXV_STATUS_BAD_CONTAINER);
    CHECK(out.empty());
  }

  // Huge claimed sizes (OOM protection, 32-bit TOO_LARGE, and SPIR-V uint32
  // overflow check) tested without allocating multi-GiB buffers.
  {
    const auto huge_stored =
        make_blob({arixv::kModeStored, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF});
    const auto huge_coded =
        make_blob({arixv::kModeCoded, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF});
    // Max valid body_words: (0xFFFFFFFF - 20) / 4 = 0x3FFFFFFA -> 4294967292 B.
    const auto max_spirv =
        make_blob({arixv::kModeSpirv, 0xFE, 0xFA, 0xFF, 0xFF, 0x3F});
    // 0x3FFFFFFB overflows uint32_t bytes -> BAD_CONTAINER on all platforms.
    const auto overflow_spirv =
        make_blob({arixv::kModeSpirv, 0xFE, 0xFB, 0xFF, 0xFF, 0x3F});

    arixv_header_info info{};
    info.struct_size = sizeof(info);
    CHECK(arixv_inspect(overflow_spirv.data(), overflow_spirv.size(), &info) ==
          ARIXV_STATUS_BAD_CONTAINER);
    size_t sz = 123;
    CHECK(arixv_decode(overflow_spirv.data(), overflow_spirv.size(), nullptr,
                       &sz, nullptr) == ARIXV_STATUS_BAD_CONTAINER);
    CHECK(sz == 123);

    const std::pair<std::vector<uint8_t>, uint64_t> huge_cases[] = {
        {huge_stored, 0xFFFFFFFFull},
        {huge_coded, 0xFFFFFFFFull},
        {max_spirv, 4294967292ull},
    };
    for (const auto& [blob, expected_bytes] : huge_cases) {
      // arixv_inspect always reports the full 64-bit declared size.
      arixv_header_info h{};
      h.struct_size = sizeof(h);
      CHECK(arixv_inspect(blob.data(), blob.size(), &h) == ARIXV_STATUS_OK);
      CHECK(h.uncompressed_size == expected_bytes);

      uint8_t small_buf[16];
      std::memset(small_buf, 0x5A, sizeof(small_buf));
      size_t q = 777;
      size_t cap = sizeof(small_buf);
      if (expected_bytes > arixv::kPlatformMaxUncompressedSize) {
        // 32-bit ILP32: exceeds PTRDIFF_MAX - 16 -> TOO_LARGE.
        CHECK(arixv_decode(blob.data(), blob.size(), nullptr, &q, nullptr) ==
              ARIXV_STATUS_TOO_LARGE);
        CHECK(q == 777);
        CHECK(arixv_decode(blob.data(), blob.size(), small_buf, &cap,
                           nullptr) == ARIXV_STATUS_TOO_LARGE);
        CHECK(cap == sizeof(small_buf));
      } else {
        // 64-bit: size query reports declared size; small buffer rejects with
        // BUFFER_TOO_SMALL without allocating or touching `small_buf`.
        CHECK(arixv_decode(blob.data(), blob.size(), nullptr, &q, nullptr) ==
              ARIXV_STATUS_OK);
        CHECK(q == static_cast<size_t>(expected_bytes));
        CHECK(arixv_decode(blob.data(), blob.size(), small_buf, &cap,
                           nullptr) == ARIXV_STATUS_BUFFER_TOO_SMALL);
        CHECK(cap == static_cast<size_t>(expected_bytes));
      }
      for (uint8_t b : small_buf) {
        CHECK(b == 0x5A);
      }
    }
  }

  const auto unknown = make_blob({99, 0});
  CHECK(DecodeBytes(unknown.data(), unknown.size(), out) ==
        ARIXV_STATUS_BAD_CONTAINER);
}

void CApiContractTests() {
  // 1. Version and status strings.
  CHECK(arixv_version() ==
        ((static_cast<uint32_t>(ARIXV_VERSION_MAJOR) << 16) |
         (static_cast<uint32_t>(ARIXV_VERSION_MINOR) << 8) |
         static_cast<uint32_t>(ARIXV_VERSION_PATCH)));
  for (arixv_status st = ARIXV_STATUS_OK; st <= ARIXV_STATUS_TOO_LARGE; ++st) {
    const char* s = arixv_status_string(st);
    CHECK(s != nullptr && s[0] != '\0');
  }
  CHECK(arixv_status_string(-1) != nullptr);
  CHECK(arixv_status_string(999) != nullptr);

  // Prepare representative blobs for all 3 modes.
  const std::vector<uint8_t> stored_in = {0x11, 0x22, 0x33, 0x44, 0x55};
  const std::vector<uint8_t> stored_blob = EncodeBytes(stored_in, false);
  CHECK(ContainerMode(stored_blob) == ARIXV_MODE_STORED);

  const std::string rep_text(256, 'A');
  const std::vector<uint8_t> coded_in(rep_text.begin(), rep_text.end());
  const std::vector<uint8_t> coded_blob = EncodeBytes(coded_in, false);
  CHECK(ContainerMode(coded_blob) == ARIXV_MODE_CODED);

  const std::vector<uint32_t> spirv_words = {
      0x07230203u, 0x00010300u, 0u, 3u, 0u,
      (2u << 16) | 17u, 1u,
      (3u << 16) | 14u, 0u, 1u,
  };
  std::vector<uint8_t> spirv_in(spirv_words.size() * 4);
  for (size_t i = 0; i < spirv_words.size(); ++i) {
    const uint32_t w = spirv_words[i];
    spirv_in[i * 4 + 0] = static_cast<uint8_t>(w & 0xFFu);
    spirv_in[i * 4 + 1] = static_cast<uint8_t>((w >> 8) & 0xFFu);
    spirv_in[i * 4 + 2] = static_cast<uint8_t>((w >> 16) & 0xFFu);
    spirv_in[i * 4 + 3] = static_cast<uint8_t>((w >> 24) & 0xFFu);
  }
  const std::vector<uint8_t> spirv_blob = EncodeBytes(spirv_in, false);
  CHECK(ContainerMode(spirv_blob) == ARIXV_MODE_SPIRV);

  // 2. arixv_inspect: modes, ARX? unsupported format, invalid args & struct_size.
  const uint32_t expected_magic = 0x32585241u;  // "ARX2" LE
  for (const auto& [blob, mode, orig_sz] : std::vector<
           std::tuple<const std::vector<uint8_t>*, uint32_t, uint64_t>>{
           {&stored_blob, ARIXV_MODE_STORED, stored_in.size()},
           {&coded_blob, ARIXV_MODE_CODED, coded_in.size()},
           {&spirv_blob, ARIXV_MODE_SPIRV, spirv_in.size()},
       }) {
    arixv_header_info info{};
    info.struct_size = sizeof(info);
    info.reserved = 0xDEADBEEFu;
    CHECK(arixv_inspect(blob->data(), blob->size(), &info) == ARIXV_STATUS_OK);
    CHECK(info.magic == expected_magic);
    CHECK(info.mode == mode);
    CHECK(info.reserved == 0);
    CHECK(info.uncompressed_size == orig_sz);
  }

  for (const uint8_t ver_byte : {'0', '1', '3', 'B', '\0'}) {
    const uint8_t future_magic[6] = {'A', 'R', 'X', ver_byte, 0, 0};
    arixv_header_info info{};
    info.struct_size = sizeof(info);
    CHECK(arixv_inspect(future_magic, sizeof(future_magic), &info) ==
          ARIXV_STATUS_UNSUPPORTED_FORMAT);
    size_t sz = 42;
    CHECK(arixv_decode(future_magic, sizeof(future_magic), nullptr, &sz,
                       nullptr) == ARIXV_STATUS_UNSUPPORTED_FORMAT);
    CHECK(sz == 42);
  }

  {
    arixv_header_info info{};
    CHECK(arixv_inspect(nullptr, stored_blob.size(), &info) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(arixv_inspect(stored_blob.data(), stored_blob.size(), nullptr) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    info.struct_size = 0;
    CHECK(arixv_inspect(stored_blob.data(), stored_blob.size(), &info) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    info.struct_size = sizeof(arixv_header_info) - 1;
    CHECK(arixv_inspect(stored_blob.data(), stored_blob.size(), &info) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    info.struct_size = sizeof(arixv_header_info) + 4;
    CHECK(arixv_inspect(stored_blob.data(), stored_blob.size(), &info) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    info.struct_size = sizeof(arixv_header_info);
    const size_t over_span =
        static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max()) + 1u;
    CHECK(arixv_inspect(stored_blob.data(), over_span, &info) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
  }

  // 3. arixv_decode: optimistic BUFFER_TOO_SMALL retry, sentinel preservation,
  //    extra-capacity decode, invalid opts/pointers, and uint32_t-aligned output.
  {
    std::vector<uint8_t> buf(spirv_in.size() + 16, 0xCC);
    size_t cap = spirv_in.size() - 1;
    CHECK(arixv_decode(spirv_blob.data(), spirv_blob.size(), buf.data(), &cap,
                       nullptr) == ARIXV_STATUS_BUFFER_TOO_SMALL);
    CHECK(cap == spirv_in.size());
    for (uint8_t b : buf) {
      CHECK(b == 0xCC);
    }

    // Zero capacity with non-NULL out on non-empty blob -> BUFFER_TOO_SMALL.
    cap = 0;
    CHECK(arixv_decode(spirv_blob.data(), spirv_blob.size(), buf.data(), &cap,
                       nullptr) == ARIXV_STATUS_BUFFER_TOO_SMALL);
    CHECK(cap == spirv_in.size());
    CHECK(buf[0] == 0xCC);

    // Retry with oversize buffer -> exact bytes written, tail untouched.
    cap = buf.size();
    arixv_decode_options valid_opts{sizeof(arixv_decode_options), 0};
    CHECK(arixv_decode(spirv_blob.data(), spirv_blob.size(), buf.data(), &cap,
                       &valid_opts) == ARIXV_STATUS_OK);
    CHECK(cap == spirv_in.size());
    CHECK(std::equal(spirv_in.begin(), spirv_in.end(), buf.begin()));
    for (size_t i = spirv_in.size(); i < buf.size(); ++i) {
      CHECK(buf[i] == 0xCC);
    }

    // Direct decode into uint32_t-aligned Vulkan-style buffer.
    size_t req_bytes = 0;
    CHECK(arixv_decode(spirv_blob.data(), spirv_blob.size(), nullptr,
                       &req_bytes, nullptr) == ARIXV_STATUS_OK);
    CHECK(req_bytes == spirv_words.size() * sizeof(uint32_t));
    std::vector<uint32_t> vk_words(req_bytes / sizeof(uint32_t), 0u);
    size_t out_bytes = vk_words.size() * sizeof(uint32_t);
    CHECK(arixv_decode(spirv_blob.data(), spirv_blob.size(), vk_words.data(),
                       &out_bytes, nullptr) == ARIXV_STATUS_OK);
    CHECK(out_bytes == req_bytes);
    CHECK(std::memcmp(vk_words.data(), spirv_in.data(), req_bytes) == 0);

    // Invalid decode args and options leave *out_size unchanged.
    size_t keep = 555;
    CHECK(arixv_decode(nullptr, spirv_blob.size(), buf.data(), &keep,
                       nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(keep == 555);
    CHECK(arixv_decode(spirv_blob.data(), spirv_blob.size(), buf.data(),
                       nullptr, nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    const size_t over_span =
        static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max()) + 1u;
    CHECK(arixv_decode(spirv_blob.data(), over_span, nullptr, &keep, nullptr) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(keep == 555);

    const arixv_decode_options bad_opts[] = {
        {0, 0},
        {sizeof(arixv_decode_options) - 1, 0},
        {sizeof(arixv_decode_options) + 4, 0},
        {sizeof(arixv_decode_options), 1},
    };
    for (const auto& o : bad_opts) {
      size_t sz = 888;
      CHECK(arixv_decode(spirv_blob.data(), spirv_blob.size(), buf.data(), &sz,
                         &o) == ARIXV_STATUS_INVALID_ARGUMENT);
      CHECK(sz == 888);
    }
  }

  // 4. 0-byte payloads (both stored and forced-coded): out==NULL vs out!=NULL.
  for (bool force_coded : {false, true}) {
    const std::vector<uint8_t> empty_blob = EncodeBytes({}, force_coded);
    size_t queried = 999;
    CHECK(arixv_decode(empty_blob.data(), empty_blob.size(), nullptr, &queried,
                       nullptr) == ARIXV_STATUS_OK);
    CHECK(queried == 0);

    uint8_t sentinel = 0xEE;
    size_t out_sz = 0;
    CHECK(arixv_decode(empty_blob.data(), empty_blob.size(), &sentinel,
                       &out_sz, nullptr) == ARIXV_STATUS_OK);
    CHECK(out_sz == 0);
    CHECK(sentinel == 0xEE);
  }

  // 5. arixv_encode_bound & arixv_encode contract tests.
  {
    CHECK(arixv_encode_bound(0, nullptr) == 6);
    const arixv_encode_options valid_opts{sizeof(arixv_encode_options), 0};
    CHECK(arixv_encode_bound(0, &valid_opts) == 6);
    CHECK(arixv_encode_bound(191, nullptr) == 5 + 1 + 191);
    CHECK(arixv_encode_bound(192, nullptr) == 5 + 2 + 192);
    CHECK(arixv_encode_bound(15551, nullptr) == 5 + 2 + 15551);
    CHECK(arixv_encode_bound(15552, nullptr) == 5 + 3 + 15552);
    CHECK(arixv_encode_bound(81087, nullptr) == 5 + 3 + 81087);
    CHECK(arixv_encode_bound(81088, nullptr) == 5 + 4 + 81088);
    CHECK(arixv_encode_bound(16858303, nullptr) == 5 + 4 + 16858303);
    CHECK(arixv_encode_bound(16858304, nullptr) == 5 + 5 + 16858304);
    CHECK(arixv_encode_bound(arixv::kPlatformMaxUncompressedSize, nullptr) ==
          5 + 5 + arixv::kPlatformMaxUncompressedSize);
    CHECK(arixv_encode_bound(arixv::kPlatformMaxUncompressedSize + 1u,
                             nullptr) == 0);
    CHECK(arixv_encode_bound(std::numeric_limits<size_t>::max(), nullptr) == 0);

    const arixv_encode_options bad_opts[] = {
        {0, 0},
        {sizeof(arixv_encode_options) - 1, 0},
        {sizeof(arixv_encode_options) + 4, 0},
        {sizeof(arixv_encode_options), 1},
    };
    for (const auto& o : bad_opts) {
      CHECK(arixv_encode_bound(16, &o) == 0);
      uint8_t out_buf[32];
      std::memset(out_buf, 0x77, sizeof(out_buf));
      size_t out_sz = sizeof(out_buf);
      CHECK(arixv_encode(stored_in.data(), stored_in.size(), out_buf, &out_sz,
                         &o) == ARIXV_STATUS_INVALID_ARGUMENT);
      CHECK(out_sz == sizeof(out_buf));
      CHECK(out_buf[0] == 0x77);
    }

    // in == NULL && in_size == 0 is valid and produces a 6-byte container.
    uint8_t empty_out[16];
    size_t empty_out_sz = sizeof(empty_out);
    CHECK(arixv_encode(nullptr, 0, empty_out, &empty_out_sz, &valid_opts) ==
          ARIXV_STATUS_OK);
    CHECK(empty_out_sz == 6);
    size_t dec_sz = 99;
    CHECK(arixv_decode(empty_out, empty_out_sz, nullptr, &dec_sz, nullptr) ==
          ARIXV_STATUS_OK);
    CHECK(dec_sz == 0);

    // Invalid pointer/size arguments.
    size_t sz = 16;
    CHECK(arixv_encode(nullptr, 1, empty_out, &sz, nullptr) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(arixv_encode(stored_in.data(), stored_in.size(), nullptr, &sz,
                       nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(arixv_encode(stored_in.data(), stored_in.size(), empty_out, nullptr,
                       nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(arixv_encode(stored_in.data(),
                       arixv::kPlatformMaxUncompressedSize + 1u, empty_out, &sz,
                       nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);

    // BUFFER_TOO_SMALL retry on arixv_encode leaves out untouched.
    std::vector<uint8_t> small(spirv_blob.size(), 0xBB);
    size_t small_cap = spirv_blob.size() - 1;
    CHECK(arixv_encode(spirv_in.data(), spirv_in.size(), small.data(),
                       &small_cap, nullptr) == ARIXV_STATUS_BUFFER_TOO_SMALL);
    CHECK(small_cap == spirv_blob.size());
    for (uint8_t b : small) {
      CHECK(b == 0xBB);
    }
    CHECK(arixv_encode(spirv_in.data(), spirv_in.size(), small.data(),
                       &small_cap, nullptr) == ARIXV_STATUS_OK);
    CHECK(small_cap == spirv_blob.size());
    CHECK(small == spirv_blob);
  }
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
  const std::vector<uint8_t> baseline = EncodeBytes(spirv, false);

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&spirv, &baseline]() {
      for (int i = 0; i < 16; ++i) {
        const std::vector<uint8_t> encoded = EncodeBytes(spirv, false);
        CHECK(encoded == baseline);
        std::vector<uint8_t> decoded;
        CHECK(DecodeBytes(encoded.data(), encoded.size(), decoded) ==
              ARIXV_STATUS_OK);
        CHECK(decoded == spirv);
      }
    });
  }
  for (std::thread& th : threads) {
    th.join();
  }
}

}  // namespace

int main() {
  CHECK(arixv::PrimedStringMatchTableIsConsistent());
  BasicRoundTrips();
  GoldenSpirvEncoding();
  GoldenSpirvModuleEncoding();
  RandomRoundTrips();
  CorpusRoundTrips();
  StoredModeForIncompressible();
  CompactSizeEdgeCases();
  HostileInputs();
  CApiContractTests();
  ConcurrentEncodeDecode();
  return 0;
}
