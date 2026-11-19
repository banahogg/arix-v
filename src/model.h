// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_MODEL_H_
#define ARIXV_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arixv.h"

namespace arixv {

bool IsSpirvModelEligible(const uint8_t* data, size_t size);

void EncodeModule(const uint8_t* data, size_t size,
                  std::vector<uint8_t>& out_payload);

// Each call builds and frees its own model.
Status DecodeModule(const uint8_t* payload, size_t payload_size,
                    size_t expected_original_size, std::vector<uint8_t>& out);

// Experimental entropy-free residual path used to measure the decode ceiling
// of the structural model. The payload is the sequence of structural choices
// encoded as packed fixed-width fields and ULEB128 residuals: there is no range
// coding, probability update, or external dependency. This is intentionally
// not an ARX2 container mode.
void EncodeModuleRawResidual(const uint8_t* data, size_t size,
                             std::vector<uint8_t>& out_payload);

Status DecodeModuleRawResidual(const uint8_t* payload, size_t payload_size,
                               size_t expected_original_size,
                               std::vector<uint8_t>& out);

}  // namespace arixv

#endif  // ARIXV_MODEL_H_
