// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "arixv/arixv.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "container.h"
#include "model.h"
#include "platform.h"
#include "prob.h"
#include "range_coder.h"
#include "tuning.h"

namespace arixv {
inline namespace v1 {
namespace {

constexpr uint32_t kHeaderInfoV1Size = ARIXV_HEADER_INFO_V1_SIZE;
constexpr uint32_t kDecodeOptionsV1Size = ARIXV_DECODE_OPTIONS_V1_SIZE;

static_assert(kHeaderInfoV1Size == 24u, "v1 arixv_header_info size is frozen");
static_assert(sizeof(arixv_header_info) == kHeaderInfoV1Size);
static_assert(offsetof(arixv_header_info, struct_size) == 0u);
static_assert(offsetof(arixv_header_info, magic) == 4u);
static_assert(offsetof(arixv_header_info, mode) == 8u);
static_assert(offsetof(arixv_header_info, reserved) == 12u);
static_assert(offsetof(arixv_header_info, uncompressed_size) == 16u);

static_assert(kDecodeOptionsV1Size == 8u,
              "v1 arixv_decode_options size is frozen");
static_assert(sizeof(arixv_decode_options) == kDecodeOptionsV1Size);
static_assert(offsetof(arixv_decode_options, struct_size) == 0u);
static_assert(offsetof(arixv_decode_options, flags) == 4u);

struct ParsedHeader {
  uint32_t magic = 0;
  uint32_t mode = 0;
  uint64_t original_size = 0;
  size_t payload_offset = 0;
};

arixv_status ReadCompactUint32(const uint8_t* data, size_t size, size_t* pos,
                               uint32_t* value) {
  if (*pos >= size) {
    return ARIXV_STATUS_TRUNCATED;
  }
  const uint8_t b0 = data[(*pos)++];
  if (b0 < 0xC0u) {
    *value = b0;
    return ARIXV_STATUS_OK;
  }
  if (b0 <= 0xFBu) {
    if (*pos >= size) {
      return ARIXV_STATUS_TRUNCATED;
    }
    const uint8_t b1 = data[(*pos)++];
    *value = kSizeOneByteLimit + (static_cast<uint32_t>(b0 - 0xC0u) << 8) + b1;
    return ARIXV_STATUS_OK;
  }
  if (b0 == 0xFCu) {
    if (size - *pos < 2) {
      return ARIXV_STATUS_TRUNCATED;
    }
    const uint32_t v = static_cast<uint32_t>(data[*pos]) |
                       (static_cast<uint32_t>(data[*pos + 1]) << 8);
    *pos += 2;
    *value = kSizeTwoByteLimit + v;
    return ARIXV_STATUS_OK;
  }
  if (b0 == 0xFDu) {
    if (size - *pos < 3) {
      return ARIXV_STATUS_TRUNCATED;
    }
    const uint32_t v = static_cast<uint32_t>(data[*pos]) |
                       (static_cast<uint32_t>(data[*pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[*pos + 2]) << 16);
    *pos += 3;
    *value = kSizeThreeByteLimit + v;
    return ARIXV_STATUS_OK;
  }
  if (b0 == 0xFEu) {
    if (size - *pos < 4) {
      return ARIXV_STATUS_TRUNCATED;
    }
    const uint32_t v = static_cast<uint32_t>(data[*pos]) |
                       (static_cast<uint32_t>(data[*pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[*pos + 2]) << 16) |
                       (static_cast<uint32_t>(data[*pos + 3]) << 24);
    *pos += 4;
    if (v < kSizeFourByteLimit) {
      return ARIXV_STATUS_BAD_CONTAINER;
    }
    *value = v;
    return ARIXV_STATUS_OK;
  }
  return ARIXV_STATUS_BAD_CONTAINER;
}

arixv_status ParseHeader(const uint8_t* data, size_t size, ParsedHeader* out) {
  if (size > kMaxPlatformSpan) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (size < kContainerMagicBytes) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }
  if (data[0] != kContainerMagic[0] || data[1] != kContainerMagic[1] ||
      data[2] != kContainerMagic[2]) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }
  if (data[3] != kContainerMagic[3]) {
    return ARIXV_STATUS_UNSUPPORTED_FORMAT;
  }

  size_t pos = kContainerMagicBytes;
  if (pos >= size) {
    return ARIXV_STATUS_TRUNCATED;
  }
  const uint8_t mode = data[pos++];
  if (mode != kModeStored && mode != kModeCoded && mode != kModeSpirv) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }

  uint32_t encoded_size = 0;
  const arixv_status size_status =
      ReadCompactUint32(data, size, &pos, &encoded_size);
  if (size_status != ARIXV_STATUS_OK) {
    return size_status;
  }

  uint64_t original_size = encoded_size;
  if (mode == kModeSpirv) {
    if (encoded_size > kMaxSpirvBodyWords) {
      return ARIXV_STATUS_BAD_CONTAINER;
    }
    original_size = static_cast<uint64_t>(kSpirvHeaderBytes) +
                    static_cast<uint64_t>(encoded_size) * 4u;
  }

  out->magic = static_cast<uint32_t>(data[0]) |
               (static_cast<uint32_t>(data[1]) << 8) |
               (static_cast<uint32_t>(data[2]) << 16) |
               (static_cast<uint32_t>(data[3]) << 24);
  out->mode = mode;
  out->original_size = original_size;
  out->payload_offset = pos;
  return ARIXV_STATUS_OK;
}

arixv_status DecodeImpl(const void* in, size_t in_size, void* out,
                        size_t* out_size, const arixv_decode_options* opts) {
  if (in == nullptr || out_size == nullptr) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (opts != nullptr) {
    if (opts->struct_size < kDecodeOptionsV1Size ||
        opts->struct_size > kMaxOptionsStructSize) {
      return ARIXV_STATUS_INVALID_ARGUMENT;
    }
    if (opts->flags != 0) {
      return ARIXV_STATUS_INVALID_ARGUMENT;
    }
    if (opts->struct_size > sizeof(arixv_decode_options) &&
        !HasZeroTrailingBytes(opts, sizeof(arixv_decode_options),
                              opts->struct_size)) {
      return ARIXV_STATUS_INVALID_ARGUMENT;
    }
  }

  const auto* data = static_cast<const uint8_t*>(in);
  ParsedHeader hdr;
  const arixv_status hdr_status = ParseHeader(data, in_size, &hdr);
  if (hdr_status != ARIXV_STATUS_OK) {
    return hdr_status;
  }
  if (hdr.original_size > kPlatformMaxUncompressedSize) {
    return ARIXV_STATUS_TOO_LARGE;
  }
  const size_t required = static_cast<size_t>(hdr.original_size);
  const uint8_t* payload = data + hdr.payload_offset;
  const size_t payload_size = in_size - hdr.payload_offset;

  // For stored mode, validate exact payload length in O(1) even on size
  // queries so a tiny truncated header cannot trick callers into multi-GiB
  // allocations.
  if (hdr.mode == kModeStored) {
    if (payload_size < required) {
      return ARIXV_STATUS_TRUNCATED;
    }
    if (payload_size != required) {
      return ARIXV_STATUS_BAD_CONTAINER;
    }
  }

  if (out == nullptr) {
    *out_size = required;
    return ARIXV_STATUS_OK;
  }
  if (*out_size < required) {
    *out_size = required;
    return ARIXV_STATUS_BUFFER_TOO_SMALL;
  }

  if (hdr.mode == kModeStored) {
    if (required > 0) {
      std::memcpy(out, payload, required);
    }
    *out_size = required;
    return ARIXV_STATUS_OK;
  }

  if (hdr.mode == kModeSpirv) {
    const arixv_status status =
        DecodeModule(payload, payload_size, required,
                     static_cast<uint8_t*>(out));
    if (status != ARIXV_STATUS_OK) {
      return status;
    }
    *out_size = required;
    return ARIXV_STATUS_OK;
  }

  // Raw byte-coded mode (kModeCoded): decode directly into caller's `out`.
  auto* dst = static_cast<uint8_t*>(out);
  size_t written = 0;
  RangeDecoder range(payload, payload_size);
  BitDecoder coder(range);
  ByteModel byte_model;
  CodeBytePayload(coder, byte_model, nullptr, required,
                  [dst, &written](uint8_t b) { dst[written++] = b; });
  if (coder.exhausted()) {
    return coder.range_exhausted() ? ARIXV_STATUS_TRUNCATED
                                   : ARIXV_STATUS_BAD_CONTAINER;
  }
  if (written != required) {
    return ARIXV_STATUS_TRUNCATED;
  }
  if (!coder.ConsumedExactly()) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }
  *out_size = required;
  return ARIXV_STATUS_OK;
}

}  // namespace
}  // namespace v1
}  // namespace arixv

extern "C" {

uint32_t ARIXV_CALL arixv_version(void) ARIXV_NOEXCEPT {
  return (static_cast<uint32_t>(ARIXV_VERSION_MAJOR) << 16) |
         (static_cast<uint32_t>(ARIXV_VERSION_MINOR) << 8) |
         static_cast<uint32_t>(ARIXV_VERSION_PATCH);
}

const char* ARIXV_CALL arixv_status_string(arixv_status status) ARIXV_NOEXCEPT {
  switch (status) {
    case ARIXV_STATUS_OK:
      return "ok";
    case ARIXV_STATUS_BAD_CONTAINER:
      return "bad container";
    case ARIXV_STATUS_UNSUPPORTED_FORMAT:
      return "unsupported format";
    case ARIXV_STATUS_TRUNCATED:
      return "truncated";
    case ARIXV_STATUS_BUFFER_TOO_SMALL:
      return "buffer too small";
    case ARIXV_STATUS_INVALID_ARGUMENT:
      return "invalid argument";
    case ARIXV_STATUS_OUT_OF_MEMORY:
      return "out of memory";
    case ARIXV_STATUS_TOO_LARGE:
      return "output too large for platform";
  }
  return "unknown status";
}

arixv_status ARIXV_CALL arixv_inspect(const void* in, size_t in_size,
                                      arixv_header_info* out_info) ARIXV_NOEXCEPT {
  if (in == nullptr || out_info == nullptr) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (out_info->struct_size < arixv::v1::kHeaderInfoV1Size ||
      out_info->struct_size > arixv::v1::kMaxOptionsStructSize) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  const uint32_t caller_size = out_info->struct_size;
  arixv::v1::ParsedHeader hdr;
  const arixv_status status =
      arixv::v1::ParseHeader(static_cast<const uint8_t*>(in), in_size, &hdr);
  if (status != ARIXV_STATUS_OK) {
    return status;
  }
  out_info->magic = hdr.magic;
  out_info->mode = hdr.mode;
  out_info->reserved = 0;
  out_info->uncompressed_size = hdr.original_size;
  if (caller_size > sizeof(arixv_header_info)) {
    std::memset(reinterpret_cast<uint8_t*>(out_info) + sizeof(arixv_header_info),
                0, caller_size - sizeof(arixv_header_info));
  }
  return ARIXV_STATUS_OK;
}

arixv_status ARIXV_CALL arixv_decode(const void* in, size_t in_size, void* out,
                                     size_t* out_size,
                                     const arixv_decode_options* opts) ARIXV_NOEXCEPT {
#if ARIXV_HAS_EXCEPTIONS
  try {
    return arixv::v1::DecodeImpl(in, in_size, out, out_size, opts);
  } catch (const std::bad_alloc&) {
    return ARIXV_STATUS_OUT_OF_MEMORY;
  } catch (...) {
    return ARIXV_STATUS_OUT_OF_MEMORY;
  }
#else
  return arixv::v1::DecodeImpl(in, in_size, out, out_size, opts);
#endif
}

}  // extern "C"
