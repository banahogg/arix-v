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

namespace {

[[noreturn]] void Fail(const char* what, bool force_coded, size_t in_size,
                       size_t enc_size, arixv::Status status) {
  std::fprintf(stderr,
               "FAIL: %s (force_coded=%d, input=%zu bytes, encoded=%zu "
               "bytes, status=%s)\n",
               what, force_coded ? 1 : 0, in_size, enc_size,
               arixv::StatusToString(status));
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  for (const bool force_coded : {false, true}) {
    arixv::EncodeOptions opts;
    opts.force_coded = force_coded;
    const std::vector<uint8_t> encoded = arixv::Encode(data, size, opts);
    if (encoded.empty()) {
      Fail("Encode returned empty", force_coded, size, 0, arixv::Status::kOk);
    }
    // Container header is a mode byte plus a <=5-byte uvarint32 size.
    if (!force_coded && encoded.size() > size + 6) {
      Fail("expansion bound violated", force_coded, size, encoded.size(),
           arixv::Status::kOk);
    }
    if (!force_coded && encoded != arixv::Encode(data, size, opts)) {
      Fail("Encode is nondeterministic", force_coded, size, encoded.size(),
           arixv::Status::kOk);
    }
    std::vector<uint8_t> decoded;
    const arixv::Status st =
        arixv::Decode(encoded.data(), encoded.size(), decoded);
    if (st != arixv::Status::kOk) {
      Fail("Decode of own encoding failed", force_coded, size, encoded.size(),
           st);
    }
    if (decoded.size() != size ||
        (size != 0 && std::memcmp(decoded.data(), data, size) != 0)) {
      Fail("round-trip mismatch", force_coded, size, encoded.size(), st);
    }
  }
  return 0;
}
