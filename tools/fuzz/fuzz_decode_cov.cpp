// Coverage-guided hostile-decode harness (libFuzzer entry point). Build with
// clang++ -fsanitize=fuzzer[,address,undefined], or with afl-clang-fast++
// -fsanitize=fuzzer (AFL++ substitutes its libAFLDriver for persistent mode).
//
// Feeds arbitrary bytes to Decode as an untrusted blob and checks:
//   - no crash / sanitizer report on any input (the point of the exercise)
//   - output never exceeds max_output, for a roomy and a tiny cap
//   - the two caps agree: same bytes when both succeed, and the tiny cap
//     never succeeds where the roomy cap failed

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "arixv/arixv.h"

namespace {

constexpr size_t kRoomyCap = size_t{1} << 26;  // 64 MiB
constexpr size_t kTinyCap = 64;

[[noreturn]] void Fail(const char* what, size_t in_size, arixv_status roomy,
                       arixv_status tiny) {
  std::fprintf(stderr, "FAIL: %s (input=%zu bytes, roomy=%s, tiny=%s)\n", what,
               in_size, arixv_status_string(roomy), arixv_status_string(tiny));
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  arixv_header_info info{};
  info.struct_size = sizeof(info);
  const arixv_status inspect_st = arixv_inspect(data, size, &info);

  size_t queried = 0x5A5A5A5Au;
  const arixv_status query_st =
      arixv_decode(data, size, nullptr, &queried, nullptr);
  if (query_st == ARIXV_STATUS_OK) {
    if (inspect_st != ARIXV_STATUS_OK || info.uncompressed_size != queried) {
      Fail("inspect/query mismatch", size, inspect_st, query_st);
    }
  }

  uint8_t tiny_buf[kTinyCap];
  std::memset(tiny_buf, 0xCC, sizeof(tiny_buf));
  size_t tiny_size = sizeof(tiny_buf);
  const arixv_status tiny =
      arixv_decode(data, size, tiny_buf, &tiny_size, nullptr);
  if (tiny == ARIXV_STATUS_BUFFER_TOO_SMALL) {
    for (uint8_t b : tiny_buf) {
      if (b != 0xCC) {
        Fail("BUFFER_TOO_SMALL modified out buffer", size, query_st, tiny);
      }
    }
    if (query_st != ARIXV_STATUS_OK || tiny_size != queried) {
      Fail("BUFFER_TOO_SMALL size mismatch", size, query_st, tiny);
    }
  } else if (tiny == ARIXV_STATUS_OK && tiny_size > kTinyCap) {
    Fail("output exceeded tiny cap", size, query_st, tiny);
  }

  if (query_st == ARIXV_STATUS_OK && queried <= kRoomyCap) {
    std::vector<uint8_t> roomy_out(queried);
    uint8_t empty_sink = 0;
    void* dst = queried == 0 ? static_cast<void*>(&empty_sink)
                             : static_cast<void*>(roomy_out.data());
    size_t roomy_size = queried;
    const arixv_status roomy =
        arixv_decode(data, size, dst, &roomy_size, nullptr);
    if (roomy == ARIXV_STATUS_OK && roomy_size != queried) {
      Fail("roomy decode size mismatch", size, roomy, tiny);
    }
    if (tiny == ARIXV_STATUS_OK) {
      if (roomy != ARIXV_STATUS_OK) {
        Fail("tiny cap succeeded where roomy cap failed", size, roomy, tiny);
      }
      if (tiny_size != roomy_size ||
          (tiny_size > 0 &&
           std::memcmp(tiny_buf, roomy_out.data(), tiny_size) != 0)) {
        Fail("decode output depends on buffer capacity", size, roomy, tiny);
      }
    }
  }
  return 0;
}
