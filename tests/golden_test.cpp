// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

// Cross-architecture ARX2 golden vector test.
//
// Verifies that committed ARX2 fixtures decode to exact expected SHA-256
// digests on every architecture and build mode (SIMD, scalar, 32-bit, 64-bit,
// decoder-only, shared, static, -fno-exceptions). When the encoder is enabled,
// also verifies that re-encoding each decoded payload produces byte-identical
// ARX2 output.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "arixv/arixv.h"
#include "test_util.h"

#ifndef ARIXV_ENCODER
#define ARIXV_ENCODER 1
#endif

namespace {

// Minimal self-contained FIPS 180-4 SHA-256 (returns lowercase 64-char hex).
std::string Sha256Hex(const uint8_t* data, size_t size) {
  static constexpr uint32_t kK[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
  };
  auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };

  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  auto compress = [&](const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t temp1 = hh + S1 + ch + kK[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  };

  size_t pos = 0;
  while (size - pos >= 64) {
    compress(data + pos);
    pos += 64;
  }
  uint8_t tail[128] = {};
  const size_t rem = size - pos;
  for (size_t i = 0; i < rem; ++i) {
    tail[i] = data[pos + i];
  }
  tail[rem] = 0x80u;
  const size_t total_tail = (rem < 56) ? 64 : 128;
  const uint64_t bit_len = static_cast<uint64_t>(size) * 8u;
  for (int i = 0; i < 8; ++i) {
    tail[total_tail - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu);
  }
  compress(tail);
  if (total_tail == 128) {
    compress(tail + 64);
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (uint32_t word : h) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kHex[(word >> shift) & 0xFu]);
    }
  }
  return out;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), {});
  return true;
}

struct GoldenVector {
  const char* filename;
  uint8_t expected_mode;
  size_t orig_size;
  const char* orig_sha256;
  size_t enc_size;
  const char* enc_sha256;
};

constexpr GoldenVector kVectors[] = {
    {"01_stored_empty.arx2", 0, 0,
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 6,
     "930b724b15f00218ffb4f23fc19932aeab6403223727aa228842fe12863f3c6e"},
    {"02_stored_random.arx2", 0, 256,
     "c1f7ae93af4c1686c882edf61ce40cc06774a3ebdc7581aa19d1966f4ad7a33d", 263,
     "5b5ca53c93b361de3e51fd7b98f51de2f8aa28de065ae01fc824c56a40ced0a6"},
    {"03_coded_bytes.arx2", 1, 934,
     "74acae918d5decb836ae763974ea46479b4d4ab9f045d00fd08c848be83dd44d", 280,
     "a52e816a39fafa40d93d843f1569c6de9bfa5449d4e947a139164ee663eda0a5"},
    {"04_spirv_minimal.arx2", 2, 20,
     "05d5c330eb3e59549558270d86da0cb595413937c3ff00100c7c93608e0ed7bc", 11,
     "b315247a87404e93db0480c5aea5710b9952d605bf87220c7c43bf7f70823364"},
    {"05_spirv_swapped.arx2", 2, 128,
     "d1a5f4cecf000b826047e9e5c8d2246ec2f10c67faa81e219fab0d1a05cf67c0", 26,
     "a8e92d79946ce5c7f85b50ce383014322a9fb3bcc9aa8f0a81fbc29b27929d1a"},
    {"06_spirv_rich_shader.arx2", 2, 4320,
     "e239ae8fcae8f23ff0d615c7bbb3441d3b4271003f226f467be059f23def280b", 526,
     "f47d5051c89078b8f96afecc333907227c9b2bf3b2bd20c77ebf8f3a45cec1e5"},
    {"07_spirv_unknown_future.arx2", 2, 232,
     "82dcbdfe2773c4019e0110ff12efc952a7fd601ad50b2fa7b40c95f910b7ab90", 118,
     "ee74c892592d815b86d4e84fef38b15cce801ffb793bccd58a541a6f4be19790"},
    {"08_spirv_large_stress.arx2", 2, 15368,
     "721b7345707a14ccc1b28ccd5a998c430293c2426ba73966646146f3b59ef882", 872,
     "f700dd5c2da57ab0ff05f5ede860cc1c8d462615018d8e42e298296c7a6eb568"},
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dir = "tests/golden";
  if (argc >= 3 && std::string(argv[1]) == "--dir") {
    dir = argv[2];
  } else if (argc == 2) {
    dir = argv[1];
  }

  // Sanity-check SHA-256 implementation against NIST empty-string vector.
  CHECK(Sha256Hex(nullptr, 0) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  for (const GoldenVector& vec : kVectors) {
    const std::filesystem::path path = dir / vec.filename;
    std::vector<uint8_t> encoded;
    CHECK(ReadFile(path, encoded));
    CHECK(encoded.size() == vec.enc_size);
    CHECK(encoded.size() > arixv::kContainerMagicBytes);
    CHECK(encoded[arixv::kContainerMagicBytes] == vec.expected_mode);
    CHECK(Sha256Hex(encoded.data(), encoded.size()) == vec.enc_sha256);

    std::vector<uint8_t> decoded;
    CHECK(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
          arixv::Status::kOk);
    CHECK(decoded.size() == vec.orig_size);
    CHECK(Sha256Hex(decoded.data(), decoded.size()) == vec.orig_sha256);

#if ARIXV_ENCODER
    const std::vector<uint8_t> reencoded =
        arixv::Encode(decoded.data(), decoded.size());
    CHECK(reencoded.size() == vec.enc_size);
    CHECK(reencoded == encoded);
    CHECK(Sha256Hex(reencoded.data(), reencoded.size()) == vec.enc_sha256);
#endif
  }

  std::printf("golden_test: %zu vectors passed (encoder=%d)\n",
              sizeof(kVectors) / sizeof(kVectors[0]),
              static_cast<int>(ARIXV_ENCODER));
  return 0;
}
