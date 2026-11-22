// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "arixv/arixv.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "container.h"
#include "model.h"
#include "prob.h"
#include "range_coder.h"
#include "tuning.h"

namespace arixv {
inline namespace v1 {
namespace {

bool HasContainerMagic(const uint8_t* data, size_t size) {
  return size >= kContainerMagicBytes &&
         std::equal(kContainerMagic, kContainerMagic + kContainerMagicBytes,
                    data);
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

}  // namespace

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
    out.resize(original_size);
    const Status mod_status =
        DecodeModule(data + pos, size - pos, original_size, out.data());
    if (mod_status != Status::kOk) {
      out.clear();
    }
    return mod_status;
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

}  // namespace v1
}  // namespace arixv
