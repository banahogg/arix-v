// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "arixv/arixv.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "container.h"
#include "model.h"
#include "platform.h"
#include "prob.h"
#include "range_coder.h"
#include "tuning.h"

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
namespace arixv {
inline namespace v1 {
namespace {

constexpr uint32_t kEncodeOptionsV1Size =
    static_cast<uint32_t>(sizeof(arixv_encode_options));

bool ValidateEncodeOptions(const arixv_encode_options* opts) {
  if (opts == nullptr) {
    return true;
  }
  if (opts->struct_size < kEncodeOptionsV1Size ||
      opts->struct_size > sizeof(arixv_encode_options)) {
    return false;
  }
  return opts->flags == 0;
}

void EncodeStored(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
  out.clear();
  const size_t overhead =
      kContainerMagicBytes + 1 +
      CompactUint32Size(static_cast<uint32_t>(size));
  if (size <= std::numeric_limits<size_t>::max() - overhead) {
    out.reserve(size + overhead);
  }
  AppendContainerMagic(out);
  out.push_back(kModeStored);
  AppendCompactUint32(static_cast<uint32_t>(size), out);
  if (size > 0) {
    out.insert(out.end(), data, data + size);
  }
}

}  // namespace

arixv_status EncodeInternal(const uint8_t* data, size_t size, bool force_coded,
                            std::vector<uint8_t>& out) {
  out.clear();
  if (data == nullptr && size != 0) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (size > kPlatformMaxUncompressedSize) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }

  const bool spirv_eligible = IsSpirvModelEligible(data, size);
  const uint32_t header_size =
      spirv_eligible ? static_cast<uint32_t>((size - kSpirvHeaderBytes) / 4u)
                     : static_cast<uint32_t>(size);

  const size_t reserve_extra = kContainerMagicBytes + 1 +
                               CompactUint32Size(header_size) +
                               kRangeFlushBytes;
  if (size <= std::numeric_limits<size_t>::max() - reserve_extra) {
    out.reserve(size + reserve_extra);
  }
  AppendContainerMagic(out);
  out.push_back(spirv_eligible ? kModeSpirv : kModeCoded);
  AppendCompactUint32(header_size, out);

  if (spirv_eligible) {
    EncodeModule(data, size, out);
  } else {
    RangeEncoder range(out);
    BitEncoder coder(range);
    ByteModel model;
    CodeBytePayload(coder, model, data, size, [](uint8_t) {});
    coder.Flush();
  }

  const size_t stored_overhead =
      kContainerMagicBytes + 1 +
      CompactUint32Size(static_cast<uint32_t>(size));
  if (!force_coded && out.size() >= stored_overhead &&
      out.size() - stored_overhead >= size) {
    EncodeStored(data, size, out);
  }
  return ARIXV_STATUS_OK;
}

namespace {

arixv_status EncodeImpl(const void* in, size_t in_size, void* out,
                        size_t* out_size, const arixv_encode_options* opts) {
  if (out == nullptr || out_size == nullptr) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (in == nullptr && in_size != 0) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (!ValidateEncodeOptions(opts)) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (in_size > kPlatformMaxUncompressedSize) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }

  std::vector<uint8_t> encoded;
  const arixv_status status = EncodeInternal(
      static_cast<const uint8_t*>(in), in_size, /*force_coded=*/false, encoded);
  if (status != ARIXV_STATUS_OK) {
    return status;
  }
  if (*out_size < encoded.size()) {
    *out_size = encoded.size();
    return ARIXV_STATUS_BUFFER_TOO_SMALL;
  }
  std::memcpy(out, encoded.data(), encoded.size());
  *out_size = encoded.size();
  return ARIXV_STATUS_OK;
}

}  // namespace
}  // namespace v1
}  // namespace arixv

extern "C" {

size_t arixv_encode_bound(size_t in_size,
                          const arixv_encode_options* opts) ARIXV_NOEXCEPT {
  if (!arixv::v1::ValidateEncodeOptions(opts)) {
    return 0;
  }
  if (in_size > arixv::v1::kPlatformMaxUncompressedSize) {
    return 0;
  }
  return arixv::v1::kContainerMagicBytes + 1 +
         arixv::v1::CompactUint32Size(static_cast<uint32_t>(in_size)) + in_size;
}

arixv_status arixv_encode(const void* in, size_t in_size, void* out,
                          size_t* out_size,
                          const arixv_encode_options* opts) ARIXV_NOEXCEPT {
#if ARIXV_HAS_EXCEPTIONS
  try {
    return arixv::v1::EncodeImpl(in, in_size, out, out_size, opts);
  } catch (const std::bad_alloc&) {
    return ARIXV_STATUS_OUT_OF_MEMORY;
  } catch (...) {
    return ARIXV_STATUS_OUT_OF_MEMORY;
  }
#else
  return arixv::v1::EncodeImpl(in, in_size, out, out_size, opts);
#endif
}

}  // extern "C"
#endif
