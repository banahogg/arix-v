// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_ARIXV_H_
#define ARIXV_ARIXV_H_

// ARIX-V (Arithmetic Reduction Intermediate Transform - V): a standalone,
// purely adaptive SPIR-V compressor. This header is the entire public
// interface; link the .cpp files in this directory into your application.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace arixv {

// Exact four-byte format identifier for blobs produced by this release. This
// is the complete format-versioning mechanism: a format change must change
// all four bytes, and a decoder accepts only an exact match.
inline constexpr uint8_t kContainerMagic[] = {'A', 'R', 'X', '2'};
inline constexpr size_t kContainerMagicBytes = sizeof(kContainerMagic);

struct EncodeOptions {
  // Always emit the range-coded payload, even when the stored-mode fallback
  // would be smaller. For tests and benchmarks; leave false in production.
  bool force_coded = false;
};

enum class Status {
  kOk,
  kBadContainer,  // wrong magic, bad mode/header, or trailing bytes
  kTruncated,     // blob ends before the declared output is complete
  kTooLarge,      // declared output size exceeds max_output
};

struct DecoderState;

// Caller-owned reusable decoder. Not thread-safe: use one instance per
// concurrent decode job, such as one per pool worker or a checked-out decoder
// pool. Internal model memory persists across Decode calls and grows toward
// the high-water mark of inputs seen by this instance.
class Decoder {
 public:
  Decoder();
  ~Decoder();
  Decoder(Decoder&&) noexcept;
  Decoder& operator=(Decoder&&) noexcept;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  Status Decode(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                size_t max_output = 1u << 30);

 private:
  std::unique_ptr<DecoderState> state_;
};

// Human-readable name for a Status value, e.g. for error messages.
const char* StatusToString(Status status);

// Compresses `size` bytes at `data` — a SPIR-V module or arbitrary bytes —
// into a self-contained blob decodable by Decode. Never expands the input by
// more than the container header (10 bytes at most): incompressible input
// falls back to stored mode. Returns an empty vector only when size exceeds
// UINT32_MAX; an empty vector is never a valid encoding.
std::vector<uint8_t> Encode(const uint8_t* data, size_t size,
                            const EncodeOptions& opts = EncodeOptions{});

// Decompresses a blob produced by Encode, reproducing the original input
// byte-exactly in `out`. The blob is treated as untrusted: the container is
// validated, and memory use is bounded by `max_output` rather than by the
// blob's own header. Returns kOk and the full output, or a failure Status
// with `out` left empty.
Status Decode(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
              size_t max_output = 1u << 30);

}  // namespace arixv

#endif  // ARIXV_ARIXV_H_
