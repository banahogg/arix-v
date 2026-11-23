// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

// Public-surface C API contract test.
// Links strictly against the shipped `arixv::decoder` (and `arixv::encoder`
// when built with ARIXV_BUILD_ENCODER=ON) with no private `src/` headers, so
// shared builds exercise the actual exported DSO/DLL ABI directly.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "arixv/arixv.h"
#include "test_util.h"

namespace {

constexpr uint8_t kMinimalArx2[] = {
    0x41, 0x52, 0x58, 0x32, 0x02, 0x00, 0x10, 0x6c, 0xe3, 0x3f, 0xa3,
};
constexpr uint8_t kMinimalSpv[] = {
    0x03, 0x02, 0x23, 0x07, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr size_t kMaxSpan =
    static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max());
constexpr size_t kPlatformMaxSize =
    (kMaxSpan - 16u) < static_cast<size_t>(std::numeric_limits<uint32_t>::max())
        ? (kMaxSpan - 16u)
        : static_cast<size_t>(std::numeric_limits<uint32_t>::max());

void TestVersionAndStatus() {
  const uint32_t expected =
      (static_cast<uint32_t>(ARIXV_VERSION_MAJOR) << 16) |
      (static_cast<uint32_t>(ARIXV_VERSION_MINOR) << 8) |
      static_cast<uint32_t>(ARIXV_VERSION_PATCH);
  CHECK(arixv_version() == expected);
  for (arixv_status st = ARIXV_STATUS_OK; st <= ARIXV_STATUS_DEPENDENCY_INCOMPLETE; ++st) {
    const char* s = arixv_status_string(st);
    CHECK(s != nullptr && s[0] != '\0');
  }
  CHECK(arixv_status_string(-1) != nullptr);
  CHECK(arixv_status_string(999) != nullptr);
}

void TestInspectAndDecode() {
  arixv_header_info info{};
  info.struct_size = ARIXV_HEADER_INFO_V1_SIZE;
  info.reserved = 0xCAFEBABEu;
  CHECK(arixv_inspect(kMinimalArx2, sizeof(kMinimalArx2), &info) ==
        ARIXV_STATUS_OK);
  CHECK(info.magic == 0x32585241u);
  CHECK(info.mode == ARIXV_MODE_SPIRV);
  CHECK(info.reserved == 0);
  CHECK(info.uncompressed_size == sizeof(kMinimalSpv));

  // Invalid inspect arguments & struct sizes.
  CHECK(arixv_inspect(nullptr, 0, &info) == ARIXV_STATUS_INVALID_ARGUMENT);
  CHECK(arixv_inspect(kMinimalArx2, sizeof(kMinimalArx2), nullptr) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  info.struct_size = ARIXV_HEADER_INFO_V1_SIZE - 1;
  CHECK(arixv_inspect(kMinimalArx2, sizeof(kMinimalArx2), &info) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  info.struct_size = 4097;
  CHECK(arixv_inspect(kMinimalArx2, sizeof(kMinimalArx2), &info) ==
        ARIXV_STATUS_INVALID_ARGUMENT);

  // Forward-compatible larger header_info struct: v1 fields populated, tail zeroed.
  struct FutureHeaderInfo {
    arixv_header_info base;
    uint64_t extra;
  };
  FutureHeaderInfo finfo{};
  finfo.base.struct_size = sizeof(FutureHeaderInfo);
  finfo.base.reserved = 0xDEADBEEFu;
  finfo.extra = 0x1122334455667788ull;
  CHECK(arixv_inspect(kMinimalArx2, sizeof(kMinimalArx2), &finfo.base) ==
        ARIXV_STATUS_OK);
  CHECK(finfo.base.magic == 0x32585241u);
  CHECK(finfo.base.mode == ARIXV_MODE_SPIRV);
  CHECK(finfo.base.reserved == 0);
  CHECK(finfo.base.uncompressed_size == sizeof(kMinimalSpv));
  CHECK(finfo.extra == 0);

  // Future ARX? magic -> UNSUPPORTED_FORMAT.
  const uint8_t future_blob[] = {'A', 'R', 'X', '3', 0, 0};
  info.struct_size = sizeof(info);
  CHECK(arixv_inspect(future_blob, sizeof(future_blob), &info) ==
        ARIXV_STATUS_UNSUPPORTED_FORMAT);
  size_t sz = 99;
  CHECK(arixv_decode(future_blob, sizeof(future_blob), nullptr, &sz, nullptr) ==
        ARIXV_STATUS_UNSUPPORTED_FORMAT);
  CHECK(sz == 99);

  // Two-call decode + optimistic BUFFER_TOO_SMALL retry.
  CHECK(arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), nullptr, &sz,
                     nullptr) == ARIXV_STATUS_OK);
  CHECK(sz == sizeof(kMinimalSpv));

  uint8_t small_buf[16];
  std::memset(small_buf, 0xAA, sizeof(small_buf));
  size_t cap = sizeof(small_buf);
  CHECK(arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), small_buf, &cap,
                     nullptr) == ARIXV_STATUS_BUFFER_TOO_SMALL);
  CHECK(cap == sizeof(kMinimalSpv));
  for (uint8_t b : small_buf) {
    CHECK(b == 0xAA);
  }

  // Forward-compatible larger options struct (zeroed tail ok, non-zero rejected).
  struct FutureDecodeOpts {
    arixv_decode_options base;
    uint64_t extra;
  };
  FutureDecodeOpts opts{{sizeof(FutureDecodeOpts), 0}, 0};
  uint32_t words[5] = {};
  cap = sizeof(words);
  CHECK(arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), words, &cap,
                     &opts.base) == ARIXV_STATUS_OK);
  CHECK(cap == sizeof(kMinimalSpv));
  CHECK(std::memcmp(words, kMinimalSpv, sizeof(kMinimalSpv)) == 0);

  opts.extra = 1;
  CHECK(arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), words, &cap,
                     &opts.base) == ARIXV_STATUS_INVALID_ARGUMENT);

  // Stored-mode 4 GiB hostile header: TRUNCATED on 64-bit, TOO_LARGE on 32-bit,
  // never asks caller to allocate 4 GiB.
  const uint8_t huge_stored[] = {'A', 'R', 'X', '2', 0, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF};
  size_t huge_q = 123;
  const arixv_status huge_st =
      arixv_decode(huge_stored, sizeof(huge_stored), nullptr, &huge_q, nullptr);
  if (0xFFFFFFFFull > kPlatformMaxSize) {
    CHECK(huge_st == ARIXV_STATUS_TOO_LARGE);
  } else {
    CHECK(huge_st == ARIXV_STATUS_TRUNCATED);
  }
  CHECK(huge_q == 123);

  // 0-byte stored blob decode.
  const uint8_t empty_stored[] = {'A', 'R', 'X', '2', 0, 0};
  size_t zero_sz = 77;
  CHECK(arixv_decode(empty_stored, sizeof(empty_stored), nullptr, &zero_sz,
                     nullptr) == ARIXV_STATUS_OK);
  CHECK(zero_sz == 0);
  uint8_t sentinel = 0x5A;
  CHECK(arixv_decode(empty_stored, sizeof(empty_stored), &sentinel, &zero_sz,
                     nullptr) == ARIXV_STATUS_OK);
  CHECK(zero_sz == 0 && sentinel == 0x5A);

  // Concurrent stateless decode through the shipped library.
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([]() {
      for (int i = 0; i < 64; ++i) {
        uint8_t out[sizeof(kMinimalSpv)];
        size_t out_sz = sizeof(out);
        CHECK(arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), out, &out_sz,
                           nullptr) == ARIXV_STATUS_OK);
        CHECK(out_sz == sizeof(kMinimalSpv));
        CHECK(std::memcmp(out, kMinimalSpv, sizeof(kMinimalSpv)) == 0);
      }
    });
  }
  for (std::thread& th : threads) {
    th.join();
  }
}

#if ARIXV_HAS_ENCODER_
void TestEncode() {
  CHECK(arixv_encode_bound(0, nullptr) == 6);
  CHECK(arixv_encode_bound(kPlatformMaxSize, nullptr) ==
        10 + kPlatformMaxSize);
  CHECK(arixv_encode_bound(kPlatformMaxSize + 1u, nullptr) == 0);

  struct FutureEncodeOpts {
    arixv_encode_options base;
    uint64_t extra;
  };
  FutureEncodeOpts opts{{sizeof(FutureEncodeOpts), 0}, 0};
  const size_t bound = arixv_encode_bound(sizeof(kMinimalSpv), &opts.base);
  CHECK(bound == 5 + 1 + sizeof(kMinimalSpv));

  std::vector<uint8_t> encoded(bound);
  size_t out_sz = encoded.size();
  CHECK(arixv_encode(kMinimalSpv, sizeof(kMinimalSpv), encoded.data(), &out_sz,
                     &opts.base) == ARIXV_STATUS_OK);
  CHECK(out_sz == sizeof(kMinimalArx2));
  CHECK(std::memcmp(encoded.data(), kMinimalArx2, sizeof(kMinimalArx2)) == 0);

  opts.extra = 42;
  CHECK(arixv_encode_bound(sizeof(kMinimalSpv), &opts.base) == 0);
  CHECK(arixv_encode(kMinimalSpv, sizeof(kMinimalSpv), encoded.data(), &out_sz,
                     &opts.base) == ARIXV_STATUS_INVALID_ARGUMENT);

  // NULL input allowed for 0-byte encode.
  uint8_t empty_out[16];
  size_t empty_sz = sizeof(empty_out);
  CHECK(arixv_encode(nullptr, 0, empty_out, &empty_sz, nullptr) ==
        ARIXV_STATUS_OK);
  CHECK(empty_sz == 6);
}
#endif

#if ARIXV_HAS_PACK_
// Pack API through the shipped library (exports, struct sizes, round trip).
void TestPackSurface() {
  static_assert(sizeof(arixv_pack_info) == ARIXV_PACK_INFO_V1_SIZE, "");
  static_assert(sizeof(arixv_pack_module_info) == 16, "");
  static_assert(sizeof(arixv_pack_outputs) == ARIXV_PACK_OUTPUTS_V1_SIZE, "");
  static_assert(sizeof(arixv_pack_decode_options) ==
                    ARIXV_PACK_DECODE_OPTIONS_V1_SIZE, "");
  arixv_pack_info info{};
  info.struct_size = sizeof(info);
  CHECK(arixv_pack_inspect(kMinimalArx2, sizeof(kMinimalArx2), &info) ==
        ARIXV_STATUS_BAD_CONTAINER);
  CHECK(arixv_pack_inspect(nullptr, 0, &info) == ARIXV_STATUS_INVALID_ARGUMENT);
  const uint8_t future[] = {'A', 'R', 'X', 'P', 2};
  CHECK(arixv_pack_inspect(future, sizeof(future), &info) ==
        ARIXV_STATUS_UNSUPPORTED_FORMAT);
  CHECK(arixv_pack_reset_scratch(future, sizeof(future), nullptr, 0) ==
        ARIXV_STATUS_UNSUPPORTED_FORMAT);
  CHECK(arixv_pack_decode(future, sizeof(future), nullptr, nullptr) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
#if ARIXV_HAS_ENCODER_
  static_assert(sizeof(arixv_pack_encode_options) ==
                    ARIXV_PACK_ENCODE_OPTIONS_V1_SIZE, "");
  const uint8_t bytes[] = {1, 2, 3};
  const void* mods[] = {kMinimalSpv, bytes, kMinimalSpv, nullptr};
  const size_t sizes[] = {sizeof(kMinimalSpv), sizeof(bytes),
                          sizeof(kMinimalSpv), 0};
  size_t n = arixv_pack_encode_bound(sizes, 4, nullptr);
  CHECK(n > 0);
  // A size beyond the addressable span is rejected, never wrapped (ILP32).
  if (sizeof(size_t) == 4) {
    const size_t huge = UINT32_MAX;
    CHECK(arixv_pack_encode_bound(&huge, 1, nullptr) == 0);
  }
  std::vector<uint8_t> pack(n);
  CHECK(arixv_pack_encode(mods, sizes, 4, pack.data(), &n, nullptr) ==
        ARIXV_STATUS_OK);
  pack.resize(n);
  CHECK(arixv_pack_inspect(pack.data(), pack.size(), &info) == ARIXV_STATUS_OK);
  CHECK(info.module_count == 4 && info.total_output_size == 2 * 20 + 3);
  std::vector<uint8_t> a(20), b(3), c(20);
  void* outs[] = {a.data(), b.data(), c.data(), nullptr};
  const arixv_pack_outputs o = {sizeof(o), 4, outs, sizes};
  CHECK(arixv_pack_decode(pack.data(), pack.size(), &o, nullptr) ==
        ARIXV_STATUS_OK);
  CHECK(std::memcmp(a.data(), kMinimalSpv, 20) == 0 &&
        std::memcmp(b.data(), bytes, 3) == 0 &&
        std::memcmp(c.data(), kMinimalSpv, 20) == 0);
  size_t q = 0;
  CHECK(arixv_decode(pack.data(), pack.size(), nullptr, &q, nullptr) ==
        ARIXV_STATUS_UNSUPPORTED_FORMAT);
#endif
}
#endif  // ARIXV_HAS_PACK_

}  // namespace

int main() {
  TestVersionAndStatus();
  TestInspectAndDecode();
#if ARIXV_HAS_ENCODER_
  TestEncode();
#endif
#if ARIXV_HAS_PACK_
  TestPackSurface();
#endif
  return 0;
}
