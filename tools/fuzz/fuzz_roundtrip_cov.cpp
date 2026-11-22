// Coverage-guided round-trip harness (libFuzzer entry point). Build with
// clang++ -fsanitize=fuzzer[,address,undefined], or with afl-clang-fast++
// -fsanitize=fuzzer (AFL++ substitutes its libAFLDriver for persistent mode).
//
// Contracts checked for arbitrary bytes x, in default and force_coded modes:
//   - Encode never returns empty for fuzz-sized inputs (only legal > 4 GiB)
//   - default mode respects the documented expansion bound (header only)
//   - Encode is deterministic (checked in default mode only, for speed)
//   - Decode(Encode(x)) == x with status kOk
// Violations print context and abort(), which the fuzzer records as a crash.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "arixv/arixv.h"
#include "container.h"

namespace {

[[noreturn]] void Fail(const char* what, bool force_coded, size_t in_size,
                       size_t enc_size, arixv_status status) {
  std::fprintf(stderr,
               "FAIL: %s (force_coded=%d, input=%zu bytes, encoded=%zu "
               "bytes, status=%s)\n",
               what, force_coded ? 1 : 0, in_size, enc_size,
               arixv_status_string(status));
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  for (const bool force_coded : {false, true}) {
    std::vector<uint8_t> encoded;
    if (force_coded) {
      const arixv_status st =
          arixv::EncodeInternal(data, size, /*force_coded=*/true, encoded);
      if (st != ARIXV_STATUS_OK || encoded.empty()) {
        Fail("EncodeInternal failed", force_coded, size, encoded.size(), st);
      }
    } else {
      const size_t bound = arixv_encode_bound(size, nullptr);
      if (bound == 0) {
        Fail("arixv_encode_bound returned 0", force_coded, size, 0,
             ARIXV_STATUS_INVALID_ARGUMENT);
      }
      encoded.resize(bound);
      size_t enc_size = encoded.size();
      const arixv_status st =
          arixv_encode(data, size, encoded.data(), &enc_size, nullptr);
      if (st != ARIXV_STATUS_OK || enc_size == 0 || enc_size > bound) {
        Fail("arixv_encode failed or exceeded bound", force_coded, size,
             enc_size, st);
      }
      encoded.resize(enc_size);

      std::vector<uint8_t> encoded2(bound);
      size_t enc_size2 = encoded2.size();
      const arixv_status st2 =
          arixv_encode(data, size, encoded2.data(), &enc_size2, nullptr);
      encoded2.resize(enc_size2);
      if (st2 != ARIXV_STATUS_OK || encoded2 != encoded) {
        Fail("arixv_encode is nondeterministic", force_coded, size, enc_size,
             st2);
      }
    }

    size_t required = 0;
    arixv_status st = arixv_decode(encoded.data(), encoded.size(), nullptr,
                                   &required, nullptr);
    if (st != ARIXV_STATUS_OK || required != size) {
      Fail("arixv_decode size query failed", force_coded, size, encoded.size(),
           st);
    }
    std::vector<uint8_t> decoded(required);
    uint8_t empty_sink = 0;
    void* dst = required == 0 ? static_cast<void*>(&empty_sink)
                              : static_cast<void*>(decoded.data());
    size_t out_size = required;
    st = arixv_decode(encoded.data(), encoded.size(), dst, &out_size, nullptr);
    if (st != ARIXV_STATUS_OK) {
      Fail("Decode of own encoding failed", force_coded, size, encoded.size(),
           st);
    }
    if (out_size != size ||
        (size != 0 && std::memcmp(decoded.data(), data, size) != 0)) {
      Fail("round-trip mismatch", force_coded, size, encoded.size(), st);
    }
  }
  return 0;
}
