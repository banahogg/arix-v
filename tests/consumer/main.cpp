// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <arixv/arixv.h>

// Private headers under src/ must never leak onto downstream include paths.
#ifdef __has_include
#if __has_include("tuning.h") || __has_include("platform.h") || \
    __has_include("model.h") || __has_include("tracker.h") || \
    __has_include("container.h")
#error "Private ARIX-V headers leaked into public include path"
#endif
#endif

#if CONSUMER_HAS_ENCODER
#if !ARIXV_HAS_ENCODER_
#error "arixv::encoder must propagate ARIXV_ENCODER=1"
#endif
#else
#if ARIXV_HAS_ENCODER_
#error "arixv::decoder must not expose encoder declarations"
#endif
#endif

int main() {
  // Golden 11-byte ARX2 encoding of a minimal 20-byte SPIR-V 1.3 header.
  static constexpr uint8_t kMinimalArx2[] = {
      0x41, 0x52, 0x58, 0x32, 0x02, 0x00, 0x10, 0x6c, 0xe3, 0x3f, 0xa3,
  };
  static constexpr uint8_t kMinimalSpv[] = {
      0x03, 0x02, 0x23, 0x07,  // magic
      0x00, 0x03, 0x01, 0x00,  // version 1.3
      0x00, 0x00, 0x00, 0x00,  // generator 0
      0x01, 0x00, 0x00, 0x00,  // bound 1
      0x00, 0x00, 0x00, 0x00,  // schema 0
  };

  if (arixv_version() != ((uint32_t{ARIXV_VERSION_MAJOR} << 16) |
                          (uint32_t{ARIXV_VERSION_MINOR} << 8) |
                          uint32_t{ARIXV_VERSION_PATCH})) {
    std::fprintf(stderr, "consumer_smoke (C++17): version mismatch\n");
    return 1;
  }

  arixv_header_info info{};
  info.struct_size = sizeof(info);
  if (arixv_inspect(kMinimalArx2, sizeof(kMinimalArx2), &info) !=
          ARIXV_STATUS_OK ||
      info.mode != ARIXV_MODE_SPIRV ||
      info.uncompressed_size != sizeof(kMinimalSpv)) {
    std::fprintf(stderr, "consumer_smoke (C++17): inspect failed\n");
    return 1;
  }

  size_t decoded_size = 0;
  if (arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), nullptr, &decoded_size,
                   nullptr) != ARIXV_STATUS_OK ||
      decoded_size != sizeof(kMinimalSpv)) {
    std::fprintf(stderr, "consumer_smoke (C++17): decode size query failed\n");
    return 1;
  }
  std::vector<uint8_t> decoded(decoded_size);
  if (arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), decoded.data(),
                   &decoded_size, nullptr) != ARIXV_STATUS_OK) {
    std::fprintf(stderr, "consumer_smoke (C++17): decode failed\n");
    return 1;
  }
  if (decoded_size != sizeof(kMinimalSpv) ||
      !std::equal(decoded.begin(), decoded.end(), kMinimalSpv)) {
    std::fprintf(stderr, "consumer_smoke (C++17): decoded bytes mismatch\n");
    return 1;
  }

#if CONSUMER_HAS_ENCODER
  const size_t bound = arixv_encode_bound(decoded.size(), nullptr);
  if (bound < 6) {
    std::fprintf(stderr, "consumer_smoke (C++17): encode bound failed\n");
    return 1;
  }
  std::vector<uint8_t> encoded(bound);
  size_t encoded_size = encoded.size();
  if (arixv_encode(decoded.data(), decoded.size(), encoded.data(),
                   &encoded_size, nullptr) != ARIXV_STATUS_OK) {
    std::fprintf(stderr, "consumer_smoke (C++17): encode failed\n");
    return 1;
  }
  encoded.resize(encoded_size);
  if (encoded.size() != sizeof(kMinimalArx2) ||
      !std::equal(encoded.begin(), encoded.end(), kMinimalArx2)) {
    std::fprintf(stderr, "consumer_smoke (C++17): re-encoded bytes mismatch\n");
    return 1;
  }
#endif

  if (arixv_status_string(ARIXV_STATUS_OK) == nullptr) {
    return 1;
  }
  std::printf("consumer_smoke (C++17): ok (encoder=%d)\n",
              static_cast<int>(CONSUMER_HAS_ENCODER));
  return 0;
}
