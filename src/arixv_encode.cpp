// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "arixv/arixv.h"

#include <cstdint>
#include <limits>
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

std::vector<uint8_t> EncodeStored(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  const size_t overhead =
      kContainerMagicBytes + 1 +
      CompactUint32Size(static_cast<uint32_t>(size));
  if (size <= std::numeric_limits<size_t>::max() - overhead) {
    out.reserve(size + overhead);
  }
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
  const size_t reserve_extra = kContainerMagicBytes + 1 +
                               CompactUint32Size(header_size) +
                               kRangeFlushBytes;
  if (size <= std::numeric_limits<size_t>::max() - reserve_extra) {
    coded.reserve(size + reserve_extra);
  }
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
  if (!opts.force_coded && coded.size() >= stored_overhead &&
      coded.size() - stored_overhead >= size) {
    return EncodeStored(data, size);
  }
  return coded;
}

}  // namespace v1
}  // namespace arixv
#endif
