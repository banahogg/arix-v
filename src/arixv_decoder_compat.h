// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_ARIXV_DECODER_COMPAT_H_
#define ARIXV_ARIXV_DECODER_COMPAT_H_

// Source-compatibility shim for the former reusable arixv::Decoder.
//
// Earlier releases offered a caller-owned Decoder that kept model memory
// between calls to avoid per-call allocation. Stateless arixv::Decode() now
// runs within ~2% of it, so the library has a single decode path and this
// class simply forwards to Decode(). It keeps no state, so the old
// thread-safety and memory-retention caveats no longer apply. New code should
// call arixv::Decode() directly; this header exists only so existing callers
// keep compiling, and may be removed in a future release.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arixv/arixv.h"

namespace arixv {
inline namespace v1 {

class Decoder {
 public:
  Decoder() = default;
  Decoder(Decoder&&) noexcept = default;
  Decoder& operator=(Decoder&&) noexcept = default;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  Status Decode(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                size_t max_output = 1u << 30) {
    return arixv::Decode(data, size, out, max_output);
  }
};

}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_ARIXV_DECODER_COMPAT_H_
