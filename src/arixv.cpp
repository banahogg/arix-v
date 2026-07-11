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

void AppendUvarint32(uint32_t value, std::vector<uint8_t>& out) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>(value | 0x80u));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

Status ReadUvarint32(const uint8_t* data, size_t size, size_t* pos,
                     uint32_t* value) {
  uint64_t result = 0;
  for (int i = 0; i < kMaxSizeVarintBytes; ++i) {
    if (*pos >= size) {
      return Status::kTruncated;
    }
    const uint8_t byte = data[(*pos)++];
    result |= static_cast<uint64_t>(byte & 0x7Fu) << (7 * i);
    if ((byte & 0x80u) == 0) {
      if (result > std::numeric_limits<uint32_t>::max()) {
        return Status::kBadContainer;
      }
      *value = static_cast<uint32_t>(result);
      return Status::kOk;
    }
  }
  return Status::kBadContainer;
}

size_t Uvarint32Size(uint32_t value) {
  size_t size = 1;
  while (value >= 0x80) {
    ++size;
    value >>= 7;
  }
  return size;
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
              Uvarint32Size(static_cast<uint32_t>(size)));
  AppendContainerMagic(out);
  out.push_back(kModeStored);
  AppendUvarint32(static_cast<uint32_t>(size), out);
  out.insert(out.end(), data, data + size);
  return out;
}

}  // namespace

struct DecoderState {
  ModuleDecoder spirv_decoder;
  ByteModel byte_model;
};

std::vector<uint8_t> Encode(const uint8_t* data, size_t size,
                            const EncodeOptions& opts) {
  if (size > std::numeric_limits<uint32_t>::max()) {
    return {};
  }

  std::vector<uint8_t> coded;
  coded.reserve(kContainerMagicBytes + size + 1 +
                Uvarint32Size(static_cast<uint32_t>(size)) + kRangeFlushBytes);
  const bool spirv_eligible = IsSpirvModelEligible(data, size);
  AppendContainerMagic(coded);
  coded.push_back(spirv_eligible ? kModeSpirv : kModeCoded);
  AppendUvarint32(static_cast<uint32_t>(size), coded);

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
      kContainerMagicBytes + 1 + Uvarint32Size(static_cast<uint32_t>(size));
  if (!opts.force_coded && coded.size() >= size + stored_overhead) {
    return EncodeStored(data, size);
  }
  return coded;
}

Decoder::Decoder() : state_(std::make_unique<DecoderState>()) {}
Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

Status Decoder::Decode(const uint8_t* data, size_t size,
                       std::vector<uint8_t>& out, size_t max_output) {
  out.clear();
  if (!HasContainerMagic(data, size)) {
    return Status::kBadContainer;
  }

  size_t pos = kContainerMagicBytes;
  const uint8_t mode = data[pos++];
  if (mode != kModeStored && mode != kModeCoded && mode != kModeSpirv) {
    return Status::kBadContainer;
  }

  uint32_t original_size = 0;
  Status status = ReadUvarint32(data, size, &pos, &original_size);
  if (status != Status::kOk) {
    return status;
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
    if (original_size < 20 || (original_size % 4) != 0) {
      return Status::kBadContainer;
    }
    return state_->spirv_decoder.Decode(data + pos, size - pos, original_size,
                                        out);
  }

  out.reserve(CappedDecodeReserve(size - pos, original_size));
  RangeDecoder range(data + pos, size - pos);
  BitDecoder coder(range);
  BeginByteModelGeneration(state_->byte_model);
  CodeBytePayload(coder, state_->byte_model, nullptr, original_size,
                  [&out](uint8_t b) { out.push_back(b); });
  if (coder.exhausted()) {
    out.clear();
    return Status::kTruncated;
  }
  if (range.consumed_size() != size - pos) {
    out.clear();
    return Status::kBadContainer;
  }
  return Status::kOk;
}

Status Decode(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
              size_t max_output) {
  Decoder decoder;
  return decoder.Decode(data, size, out, max_output);
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
