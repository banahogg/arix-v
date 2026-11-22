// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_MODEL_H_
#define ARIXV_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arixv/arixv.h"
#include "platform.h"

namespace arixv {
inline namespace v1 {

bool IsSpirvModelEligible(const uint8_t* data, size_t size);

// Self-check for tests: the generated primed string match table equals one
// rebuilt from the primer with this file's hash.
bool PrimedStringMatchTableIsConsistent();

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
void EncodeModule(const uint8_t* data, size_t size,
                  std::vector<uint8_t>& out_payload);
#endif

// Each call builds and frees its own model. Writes exactly
// `expected_original_size` bytes into caller-provided `out` on ARIXV_STATUS_OK.
arixv_status DecodeModule(const uint8_t* payload, size_t payload_size,
                          size_t expected_original_size, uint8_t* out);

#ifdef ARIXV_ENABLE_RAW_RESIDUAL
// Experimental entropy-free residual path used to measure the decode ceiling
// of the structural model. The payload is the sequence of structural choices
// encoded as packed fixed-width fields and ULEB128 residuals: there is no range
// coding, probability update, or external dependency. This is intentionally
// not an ARX2 container mode.
#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
void EncodeModuleRawResidual(const uint8_t* data, size_t size,
                             std::vector<uint8_t>& out_payload);
#endif

arixv_status DecodeModuleRawResidual(const uint8_t* payload,
                                     size_t payload_size,
                                     size_t expected_original_size,
                                     uint8_t* out);
#endif

}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_MODEL_H_
