// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_CONTAINER_H_
#define ARIXV_CONTAINER_H_

// Internal container framing helpers shared between src/arixv.cpp (decoder)
// and src/arixv_encode.cpp (encoder).

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "arixv/arixv.h"
#include "prob.h"

namespace arixv {
inline namespace v1 {

inline constexpr uint32_t kSizeOneByteLimit = 192u;
inline constexpr uint32_t kSizeTwoByteLimit =
    kSizeOneByteLimit + 60u * 256u;  // 15,552
inline constexpr uint32_t kSizeThreeByteLimit =
    kSizeTwoByteLimit + (1u << 16);  // 81,088
inline constexpr uint32_t kSizeFourByteLimit =
    kSizeThreeByteLimit + (1u << 24);  // 16,858,304
inline constexpr uint32_t kSpirvHeaderBytes = 20u;
inline constexpr uint32_t kMaxSpirvBodyWords =
    (std::numeric_limits<uint32_t>::max() - kSpirvHeaderBytes) / 4u;

inline size_t CompactUint32Size(uint32_t value) {
  if (value < kSizeOneByteLimit) return 1;
  if (value < kSizeTwoByteLimit) return 2;
  if (value < kSizeThreeByteLimit) return 3;
  if (value < kSizeFourByteLimit) return 4;
  return 5;
}

inline void AppendContainerMagic(std::vector<uint8_t>& out) {
  out.insert(out.end(), kContainerMagic,
             kContainerMagic + kContainerMagicBytes);
}

// Compact canonical 1..5-byte prefix-coded uint32:
//   0x00..0xBF          -> value (0..191)
//   0xC0..0xFB + 1 byte -> 192 + ((b0 - 192) << 8) + b1 (192..15,551)
//   0xFC + 2 bytes LE   -> 15,552 + u16 (15,552..81,087)
//   0xFD + 3 bytes LE   -> 81,088 + u24 (81,088..16,858,303)
//   0xFE + 4 bytes LE   -> u32 (must be >= 16,858,304)
//   0xFF                -> invalid
inline void AppendCompactUint32(uint32_t value, std::vector<uint8_t>& out) {
  if (value < kSizeOneByteLimit) {
    out.push_back(static_cast<uint8_t>(value));
    return;
  }
  if (value < kSizeTwoByteLimit) {
    const uint32_t v = value - kSizeOneByteLimit;
    out.push_back(static_cast<uint8_t>(0xC0u + (v >> 8)));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    return;
  }
  if (value < kSizeThreeByteLimit) {
    const uint32_t v = value - kSizeTwoByteLimit;
    out.push_back(0xFCu);
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    return;
  }
  if (value < kSizeFourByteLimit) {
    const uint32_t v = value - kSizeThreeByteLimit;
    out.push_back(0xFDu);
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    return;
  }
  out.push_back(0xFEu);
  out.push_back(static_cast<uint8_t>(value & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

template <typename Coder, typename Sink>
void CodeBytePayload(Coder& coder, ByteModel& model, const uint8_t* input,
                     size_t size, Sink sink) {
  uint8_t prev = 0;
  for (size_t i = 0; i < size; ++i) {
    // `size` comes from the untrusted header. Once the coded stream is
    // exhausted, every further byte is decoded from past-the-end zero bytes;
    // stop rather than synthesizing up to the attacker-declared size. The
    // exhaustion check in the caller then reports kTruncated.
    if constexpr (!Coder::kEncoding) {
      if (coder.exhausted()) {
        break;
      }
    }
    const uint8_t hint = input == nullptr ? 0 : input[i];
    const uint8_t b = CodeByte(coder, model, prev, hint);
    sink(b);
    prev = b;
  }
}

}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_CONTAINER_H_
