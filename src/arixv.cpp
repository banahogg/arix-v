// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "arixv.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "model.h"
#include "prob.h"
#include "range_coder.h"
#include "tuning.h"

namespace arixv {
namespace {

void AppendContainerMagic(std::vector<uint8_t>& out) {
  out.insert(out.end(), kContainerMagic,
             kContainerMagic + kContainerMagicBytes);
}

bool HasContainerMagic(const uint8_t* data, size_t size) {
  return size >= kContainerMagicBytes &&
         std::equal(kContainerMagic, kContainerMagic + kContainerMagicBytes,
                    data);
}

constexpr uint32_t kSizeOneByteLimit = 192u;
constexpr uint32_t kSizeTwoByteLimit = kSizeOneByteLimit + 60u * 256u;     // 15,552
constexpr uint32_t kSizeThreeByteLimit = kSizeTwoByteLimit + (1u << 16);   // 81,088
constexpr uint32_t kSizeFourByteLimit = kSizeThreeByteLimit + (1u << 24);  // 16,858,304
constexpr uint32_t kSpirvHeaderBytes = 20u;
constexpr uint32_t kMaxSpirvBodyWords =
    (std::numeric_limits<uint32_t>::max() - kSpirvHeaderBytes) / 4u;

// Compact canonical 1..5-byte prefix-coded uint32:
//   0x00..0xBF          -> value (0..191)
//   0xC0..0xFB + 1 byte -> 192 + ((b0 - 192) << 8) + b1 (192..15,551)
//   0xFC + 2 bytes LE   -> 15,552 + u16 (15,552..81,087)
//   0xFD + 3 bytes LE   -> 81,088 + u24 (81,088..16,858,303)
//   0xFE + 4 bytes LE   -> u32 (must be >= 16,858,304)
//   0xFF                -> invalid
void AppendCompactUint32(uint32_t value, std::vector<uint8_t>& out) {
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

Status ReadCompactUint32(const uint8_t* data, size_t size, size_t* pos,
                         uint32_t* value) {
  if (*pos >= size) {
    return Status::kTruncated;
  }
  const uint8_t b0 = data[(*pos)++];
  if (b0 < 0xC0u) {
    *value = b0;
    return Status::kOk;
  }
  if (b0 <= 0xFBu) {
    if (*pos >= size) {
      return Status::kTruncated;
    }
    const uint8_t b1 = data[(*pos)++];
    *value = kSizeOneByteLimit + (static_cast<uint32_t>(b0 - 0xC0u) << 8) + b1;
    return Status::kOk;
  }
  if (b0 == 0xFCu) {
    if (size - *pos < 2) {
      return Status::kTruncated;
    }
    const uint32_t v = static_cast<uint32_t>(data[*pos]) |
                       (static_cast<uint32_t>(data[*pos + 1]) << 8);
    *pos += 2;
    *value = kSizeTwoByteLimit + v;
    return Status::kOk;
  }
  if (b0 == 0xFDu) {
    if (size - *pos < 3) {
      return Status::kTruncated;
    }
    const uint32_t v = static_cast<uint32_t>(data[*pos]) |
                       (static_cast<uint32_t>(data[*pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[*pos + 2]) << 16);
    *pos += 3;
    *value = kSizeThreeByteLimit + v;
    return Status::kOk;
  }
  if (b0 == 0xFEu) {
    if (size - *pos < 4) {
      return Status::kTruncated;
    }
    const uint32_t v = static_cast<uint32_t>(data[*pos]) |
                       (static_cast<uint32_t>(data[*pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[*pos + 2]) << 16) |
                       (static_cast<uint32_t>(data[*pos + 3]) << 24);
    *pos += 4;
    if (v < kSizeFourByteLimit) {
      return Status::kBadContainer;
    }
    *value = v;
    return Status::kOk;
  }
  return Status::kBadContainer;
}

size_t CompactUint32Size(uint32_t value) {
  if (value < kSizeOneByteLimit) return 1;
  if (value < kSizeTwoByteLimit) return 2;
  if (value < kSizeThreeByteLimit) return 3;
  if (value < kSizeFourByteLimit) return 4;
  return 5;
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

std::vector<uint8_t> EncodeStored(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(kContainerMagicBytes + size + 1 +
              CompactUint32Size(static_cast<uint32_t>(size)));
  AppendContainerMagic(out);
  out.push_back(kModeStored);
  AppendCompactUint32(static_cast<uint32_t>(size), out);
  out.insert(out.end(), data, data + size);
  return out;
}

}  // namespace

std::vector<uint8_t> Encode(const uint8_t* data, size_t size,
                            const EncodeOptions& opts) {
  if (size > std::numeric_limits<uint32_t>::max()) {
    return {};
  }

  const bool spirv_eligible = IsSpirvModelEligible(data, size);
  const uint32_t header_size =
      spirv_eligible ? static_cast<uint32_t>((size - kSpirvHeaderBytes) / 4u)
                     : static_cast<uint32_t>(size);

  std::vector<uint8_t> coded;
  coded.reserve(kContainerMagicBytes + size + 1 +
                CompactUint32Size(header_size) + kRangeFlushBytes);
  AppendContainerMagic(coded);
  coded.push_back(spirv_eligible ? kModeSpirv : kModeCoded);
  AppendCompactUint32(header_size, coded);

  if (spirv_eligible) {
    EncodeModule(data, size, coded);
  } else {
    RangeEncoder range(coded);
    BitEncoder coder(range);
    ByteModel model;
    CodeBytePayload(coder, model, data, size, [](uint8_t) {});
    coder.Flush();
  }

  const size_t stored_overhead =
      kContainerMagicBytes + 1 +
      CompactUint32Size(static_cast<uint32_t>(size));
  if (!opts.force_coded && coded.size() >= size + stored_overhead) {
    return EncodeStored(data, size);
  }
  return coded;
}

Status Decode(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
              size_t max_output) {
  out.clear();
  if (!HasContainerMagic(data, size)) {
    return Status::kBadContainer;
  }

  size_t pos = kContainerMagicBytes;
  if (pos >= size) {
    return Status::kTruncated;
  }
  const uint8_t mode = data[pos++];
  if (mode != kModeStored && mode != kModeCoded && mode != kModeSpirv) {
    return Status::kBadContainer;
  }

  uint32_t encoded_size = 0;
  Status status = ReadCompactUint32(data, size, &pos, &encoded_size);
  if (status != Status::kOk) {
    return status;
  }

  uint32_t original_size = encoded_size;
  if (mode == kModeSpirv) {
    if (encoded_size > kMaxSpirvBodyWords) {
      return Status::kBadContainer;
    }
    original_size = kSpirvHeaderBytes + encoded_size * 4u;
  }
  if (original_size > max_output) {
    return Status::kTooLarge;
  }

  if (mode == kModeStored) {
    const size_t payload_size = size - pos;
    if (payload_size < original_size) {
      return Status::kTruncated;
    }
    if (payload_size != original_size) {
      return Status::kBadContainer;
    }
    out.assign(data + pos, data + size);
    return Status::kOk;
  }

  if (mode == kModeSpirv) {
    return DecodeModule(data + pos, size - pos, original_size, out);
  }

  out.reserve(CappedDecodeReserve(size - pos, original_size));
  RangeDecoder range(data + pos, size - pos);
  BitDecoder coder(range);
  ByteModel byte_model;
  CodeBytePayload(coder, byte_model, nullptr, original_size,
                  [&out](uint8_t b) { out.push_back(b); });
  if (coder.exhausted()) {
    out.clear();
    return coder.range_exhausted() ? Status::kTruncated : Status::kBadContainer;
  }
  if (!coder.ConsumedExactly()) {
    out.clear();
    return Status::kBadContainer;
  }
  return Status::kOk;
}

const char* StatusToString(Status status) {
  switch (status) {
    case Status::kOk:
      return "ok";
    case Status::kBadContainer:
      return "bad container";
    case Status::kTruncated:
      return "truncated";
    case Status::kTooLarge:
      return "output larger than max_output";
  }
  return "unknown status";
}

}  // namespace arixv
