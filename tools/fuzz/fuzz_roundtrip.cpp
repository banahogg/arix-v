// Self-contained randomized fuzzer for the two robustness contracts,
// no libFuzzer dependency:
//   1. Round-trip: Decode(Encode(x)) == x for arbitrary bytes x, with both
//      default options and force_coded.
//   2. Decode safety: Decode(arbitrary bytes) returns bytes or a controlled
//      error, never crashes, and respects max_output.
//
// Usage: fuzz_roundtrip [--iters=N] [--seed=S] [<corpus files...>]
// Corpus files seed the mutation pool; without them only random inputs run.
// Exit 0 = no violations found.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "arixv/arixv.h"

namespace {

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

uint64_t g_failures = 0;

void CheckRoundTrip(const std::vector<uint8_t>& input, bool force_coded,
                    const char* label, uint64_t iter) {
  arixv::EncodeOptions opts;
  opts.force_coded = force_coded;
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size(), opts);
  if (!force_coded && encoded.size() > input.size() + 10) {
    std::fprintf(stderr,
                 "FAIL[%s#%llu]: expansion bound violated (%zu -> %zu)\n",
                 label, static_cast<unsigned long long>(iter), input.size(),
                 encoded.size());
    ++g_failures;
    return;
  }
  std::vector<uint8_t> decoded;
  const arixv::Status st =
      arixv::Decode(encoded.data(), encoded.size(), decoded);
  if (st != arixv::Status::kOk || decoded != input) {
    std::fprintf(stderr, "FAIL[%s#%llu]: round-trip mismatch (status=%d, %zu "
                         "bytes in, %zu out, force_coded=%d)\n",
                 label, static_cast<unsigned long long>(iter),
                 static_cast<int>(st), input.size(), decoded.size(),
                 force_coded ? 1 : 0);
    ++g_failures;
  }
}

void CheckDecodeSafety(const std::vector<uint8_t>& blob) {
  std::vector<uint8_t> out;
  // Small cap so hostile size declarations must be rejected, not allocated.
  const arixv::Status st = arixv::Decode(blob.data(), blob.size(), out,
                                           1u << 24);
  if (st == arixv::Status::kOk && out.size() > (1u << 24)) {
    std::fprintf(stderr, "FAIL: decode exceeded max_output (%zu bytes)\n",
                 out.size());
    ++g_failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t iters = 2000;
  uint64_t seed = 0x5eed51234ULL;
  std::vector<std::vector<uint8_t>> pool;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--iters=", 0) == 0) {
      iters = std::stoull(arg.substr(8));
    } else if (arg.rfind("--seed=", 0) == 0) {
      seed = std::stoull(arg.substr(7));
    } else {
      std::vector<uint8_t> data = ReadFile(arg);
      if (!data.empty()) pool.push_back(std::move(data));
    }
  }
  std::mt19937_64 rng(seed);

  for (uint64_t it = 0; it < iters; ++it) {
    const int kind = static_cast<int>(rng() % 4);
    std::vector<uint8_t> input;
    const char* label = "random";
    if (kind == 0 || pool.empty()) {
      // Random bytes, random size 0..64KiB, occasionally word-aligned with a
      // SPIR-V magic so the modeled path is exercised.
      size_t n = static_cast<size_t>(rng() % 65536);
      if ((rng() & 3) == 0) n &= ~size_t{3};
      input.resize(n);
      for (auto& b : input) b = static_cast<uint8_t>(rng());
      if (n >= 20 && (rng() & 1)) {
        const uint32_t magic = (rng() & 1) ? 0x07230203u : 0x03022307u;
        std::memcpy(input.data(), &magic, 4);
      }
    } else {
      // Mutate a corpus file: bit flips, truncation, word splice/insertion.
      label = "mutate";
      input = pool[rng() % pool.size()];
      const int op = static_cast<int>(rng() % 4);
      if (op == 0) {
        for (int flips = static_cast<int>(rng() % 32) + 1; flips-- > 0;) {
          if (input.empty()) break;
          input[rng() % input.size()] ^=
              static_cast<uint8_t>(1u << (rng() % 8));
        }
      } else if (op == 1) {
        input.resize(rng() % (input.size() + 1));
      } else if (op == 2 && input.size() >= 8) {
        // Splice a word range to a random position (keeps 4-alignment).
        const size_t words = input.size() / 4;
        const size_t src = rng() % words;
        const size_t len = 1 + rng() % std::min<size_t>(words - src, 16);
        const size_t dst = (rng() % words) * 4;
        std::vector<uint8_t> chunk(input.begin() + src * 4,
                                   input.begin() + (src + len) * 4);
        input.insert(input.begin() + dst, chunk.begin(), chunk.end());
      } else {
        // Overwrite a random word with a hostile value.
        if (input.size() >= 4) {
          const uint32_t hostile[] = {0xFFFFFFFFu, 0x0000FFFFu, 0xFFFF0000u,
                                      0x00010000u, 0u, 0x00040000u | 0xFFu};
          const uint32_t v = hostile[rng() % 6];
          std::memcpy(&input[(rng() % (input.size() / 4)) * 4], &v, 4);
        }
      }
    }
    CheckRoundTrip(input, false, label, it);
    CheckRoundTrip(input, true, label, it);

    // Decode-safety: random blobs and mutated *encodings*.
    std::vector<uint8_t> blob;
    if ((rng() & 1) && !input.empty()) {
      blob = arixv::Encode(input.data(), input.size());
      for (int flips = static_cast<int>(rng() % 16) + 1; flips-- > 0;) {
        blob[rng() % blob.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
      }
      if (rng() & 1) blob.resize(rng() % (blob.size() + 1));
    } else {
      blob.resize(rng() % 4096);
      for (auto& b : blob) b = static_cast<uint8_t>(rng());
    }
    CheckDecodeSafety(blob);
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%llu failures\n",
                 static_cast<unsigned long long>(g_failures));
    return 1;
  }
  std::printf("fuzz_roundtrip: %llu iterations OK\n",
              static_cast<unsigned long long>(iters));
  return 0;
}
