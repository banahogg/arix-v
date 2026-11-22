// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <arixv/arixv.h>

// Private headers under src/ must never leak onto downstream include paths.
#ifdef __has_include
#if __has_include("tuning.h") || __has_include("platform.h") || \
    __has_include("model.h") || __has_include("tracker.h")
#error "Private ARIX-V headers leaked into public include path"
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

  std::vector<uint8_t> decoded;
  if (arixv::Decode(kMinimalArx2, sizeof(kMinimalArx2), decoded) !=
      arixv::Status::kOk) {
    std::fprintf(stderr, "consumer_smoke: Decode failed\n");
    return 1;
  }
  if (decoded.size() != sizeof(kMinimalSpv) ||
      !std::equal(decoded.begin(), decoded.end(), kMinimalSpv)) {
    std::fprintf(stderr, "consumer_smoke: decoded bytes mismatch\n");
    return 1;
  }

#if CONSUMER_HAS_ENCODER
  const std::vector<uint8_t> encoded =
      arixv::Encode(decoded.data(), decoded.size());
  if (encoded.size() != sizeof(kMinimalArx2) ||
      !std::equal(encoded.begin(), encoded.end(), kMinimalArx2)) {
    std::fprintf(stderr, "consumer_smoke: re-encoded bytes mismatch\n");
    return 1;
  }
#endif

  if (arixv::StatusToString(arixv::Status::kOk) == nullptr) {
    return 1;
  }
  std::printf("consumer_smoke: ok (encoder=%d)\n",
              static_cast<int>(CONSUMER_HAS_ENCODER));
  return 0;
}
