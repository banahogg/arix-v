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
#include <vector>

#include "arixv.h"

namespace {

constexpr size_t kRoomyCap = size_t{1} << 26;  // 64 MiB
constexpr size_t kTinyCap = 64;

arixv::Status DecodeUnderTest(const uint8_t* data, size_t size,
                              std::vector<uint8_t>& out, size_t max_output) {
  return arixv::Decode(data, size, out, max_output);
}

[[noreturn]] void Fail(const char* what, size_t in_size, arixv::Status roomy,
                       arixv::Status tiny) {
  std::fprintf(stderr, "FAIL: %s (input=%zu bytes, roomy=%s, tiny=%s)\n", what,
               in_size, arixv::StatusToString(roomy),
               arixv::StatusToString(tiny));
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<uint8_t> roomy_out;
  const arixv::Status roomy = DecodeUnderTest(data, size, roomy_out, kRoomyCap);
  if (roomy_out.size() > kRoomyCap) {
    Fail("output exceeded roomy max_output", size, roomy, arixv::Status::kOk);
  }

  std::vector<uint8_t> tiny_out;
  const arixv::Status tiny = DecodeUnderTest(data, size, tiny_out, kTinyCap);
  if (tiny_out.size() > kTinyCap) {
    Fail("output exceeded tiny max_output", size, roomy, tiny);
  }
  if (tiny == arixv::Status::kOk) {
    if (roomy != arixv::Status::kOk) {
      Fail("tiny cap succeeded where roomy cap failed", size, roomy, tiny);
    }
    if (tiny_out != roomy_out) {
      Fail("decode output depends on max_output", size, roomy, tiny);
    }
  }
  return 0;
}
