// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_MODEL_H_
#define ARIXV_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "arixv.h"

namespace arixv {

bool IsSpirvModelEligible(const uint8_t* data, size_t size);

void EncodeModule(const uint8_t* data, size_t size,
                  std::vector<uint8_t>& out_payload);

class ModuleDecoder {
 public:
  ModuleDecoder();
  ~ModuleDecoder();
  ModuleDecoder(ModuleDecoder&&) noexcept;
  ModuleDecoder& operator=(ModuleDecoder&&) noexcept;
  ModuleDecoder(const ModuleDecoder&) = delete;
  ModuleDecoder& operator=(const ModuleDecoder&) = delete;

  Status Decode(const uint8_t* payload, size_t payload_size,
                size_t expected_original_size, std::vector<uint8_t>& out);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

Status DecodeModule(const uint8_t* payload, size_t payload_size,
                    size_t expected_original_size, std::vector<uint8_t>& out);

}  // namespace arixv

#endif  // ARIXV_MODEL_H_
