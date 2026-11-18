// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_TUNING_H_
#define ARIXV_TUNING_H_

#include <cstddef>
#include <cstdint>

#ifndef ARIXV_KPROB_BITS
#define ARIXV_KPROB_BITS 12
#endif
#ifndef ARIXV_KPROB_SCALE
#define ARIXV_KPROB_SCALE (1u << ARIXV_KPROB_BITS)
#endif
#ifndef ARIXV_KPROB_INIT
#define ARIXV_KPROB_INIT (ARIXV_KPROB_SCALE >> 1)
#endif
#ifndef ARIXV_KPROB_SHIFT
#define ARIXV_KPROB_SHIFT 5
#endif
#ifndef ARIXV_KPROB_ADAPT_SATURATION
#define ARIXV_KPROB_ADAPT_SATURATION 30
#endif
#ifndef ARIXV_KPROB_ADAPT_MIN_SHIFT
#define ARIXV_KPROB_ADAPT_MIN_SHIFT 1
#endif
#ifndef ARIXV_KRANGE_TOP
#define ARIXV_KRANGE_TOP (1u << 24)
#endif
#ifndef ARIXV_KRANK_COUNT
#define ARIXV_KRANK_COUNT 8
#endif
#ifndef ARIXV_KRANK_ESCAPE
#define ARIXV_KRANK_ESCAPE ARIXV_KRANK_COUNT
#endif
#ifndef ARIXV_KMODE_STORED
#define ARIXV_KMODE_STORED 0
#endif
#ifndef ARIXV_KMODE_CODED
#define ARIXV_KMODE_CODED 1
#endif
#ifndef ARIXV_KMODE_SPIRV
#define ARIXV_KMODE_SPIRV 2
#endif
#ifndef ARIXV_KMAX_SIZE_VARINT_BYTES
#define ARIXV_KMAX_SIZE_VARINT_BYTES 5
#endif
#ifndef ARIXV_KRANGE_FLUSH_BYTES
#define ARIXV_KRANGE_FLUSH_BYTES 5
#endif
#ifndef ARIXV_KBYTE_CONTEXT_COUNT
#define ARIXV_KBYTE_CONTEXT_COUNT 256
#endif
#ifndef ARIXV_KBYTE_BITS
#define ARIXV_KBYTE_BITS 8
#endif
#ifndef ARIXV_KRANK_LIST_CAP
#define ARIXV_KRANK_LIST_CAP 16
#endif
#ifndef ARIXV_KRANK_LIST_CAP_IDS
#define ARIXV_KRANK_LIST_CAP_IDS 16
#endif
#ifndef ARIXV_KRANK_LIST_CAP_TYPES
#define ARIXV_KRANK_LIST_CAP_TYPES 16
#endif
// Upper bound on the up-front ID-map reservation during decode. The decoder's
// word count comes from the (untrusted) header, so an unclamped reserve lets a
// tiny hostile container force a multi-GiB allocation. Real modules exceeding
// this size just pay a few extra rehashes.
#ifndef ARIXV_KMAX_RESERVE_WORDS
#define ARIXV_KMAX_RESERVE_WORDS (1u << 22)
#endif
// Ceiling on how much larger the decoded output may be than the coded payload
// when pre-reserving the output buffer. The claimed original size is untrusted,
// so a tiny hostile payload must not force a giant reservation; we reserve at
// most this multiple of the actual payload size (the corpus max expansion is
// ~18x, so this leaves ample headroom). Legitimate output past this point grows
// by normal geometric reallocation.
#ifndef ARIXV_KMAX_DECODE_EXPANSION
#define ARIXV_KMAX_DECODE_EXPANSION 32
#endif
#ifndef ARIXV_KUINT_LENGTH_BITS
#define ARIXV_KUINT_LENGTH_BITS 6
#endif
#ifndef ARIXV_KUINT_HIGH_BITS
#define ARIXV_KUINT_HIGH_BITS 8
#endif
#ifndef ARIXV_KUINT_REST_BITS
#define ARIXV_KUINT_REST_BITS 24
#endif

namespace arixv {

// Binary probability precision used by the range coder: probabilities are in
// [0, 1 << kProbBits].
constexpr int kProbBits = ARIXV_KPROB_BITS;

// Probability scale derived from kProbBits.
constexpr uint32_t kProbScale = ARIXV_KPROB_SCALE;

// Initial probability value, exactly 0.5 at kProbBits precision.
constexpr uint16_t kProbInit = static_cast<uint16_t>(ARIXV_KPROB_INIT);

// Default adaptive update rate. Larger values adapt more slowly.
constexpr int kProbShift = ARIXV_KPROB_SHIFT;

// Update count at which a probability slot stops using the accelerated
// early-adaptation schedule (rate ~1/(n+2), i.e. shift = bit_width(n)) and
// settles at the steady kProbShift rate. Cold start dominates on small
// modules, so early updates take much larger steps.
constexpr uint16_t kProbAdaptSaturation = ARIXV_KPROB_ADAPT_SATURATION;

// Floor of the early-adaptation shift schedule. 1 = fastest warm-up (best
// ratio on structured input, worst oscillation on incompressible input).
constexpr int kProbAdaptMinShift = ARIXV_KPROB_ADAPT_MIN_SHIFT;

// Range coder renormalization threshold.
constexpr uint32_t kRangeTop = ARIXV_KRANGE_TOP;

// Number of rank-list entries coded before falling back to the escape path.
constexpr int kRankCount = ARIXV_KRANK_COUNT;

// Rank value returned by CodeRank when no slot matches.
constexpr int kRankEscape = ARIXV_KRANK_ESCAPE;

// Container mode for verbatim payloads.
constexpr uint8_t kModeStored = ARIXV_KMODE_STORED;

// Container mode for M1 range-coded order-1 byte payloads.
constexpr uint8_t kModeCoded = ARIXV_KMODE_CODED;

// Container mode for the M2 SPIR-V structural model.
constexpr uint8_t kModeSpirv = ARIXV_KMODE_SPIRV;

// Maximum byte count encoded by the v1 uvarint size field.
constexpr int kMaxSizeVarintBytes = ARIXV_KMAX_SIZE_VARINT_BYTES;

// Maximum number of bytes emitted by RangeEncoder::Flush().
constexpr int kRangeFlushBytes = ARIXV_KRANGE_FLUSH_BYTES;

// Byte contexts used by the M1 order-1 byte model.
constexpr int kByteContextCount = ARIXV_KBYTE_CONTEXT_COUNT;

// Number of bits in a byte.
constexpr int kByteBits = ARIXV_KBYTE_BITS;

// Recency-list capacities for rank coding. Caps bound tracker
// memory; drop-oldest eviction past the cap.
constexpr int kRankListCap = ARIXV_KRANK_LIST_CAP;
constexpr int kRankListCapIds = ARIXV_KRANK_LIST_CAP_IDS;
constexpr int kRankListCapTypes = ARIXV_KRANK_LIST_CAP_TYPES;

// Depth of the rank-coding bit trees for the ID/type/forward-ref recency
// lists. Must cover every list position, so it tracks the largest cap.
constexpr int kRankTreeBits = 4;
static_assert((1 << kRankTreeBits) >= kRankListCap &&
                  (1 << kRankTreeBits) >= kRankListCapIds &&
                  (1 << kRankTreeBits) >= kRankListCapTypes,
              "rank trees must address every recency-list position");

// See ARIXV_KMAX_RESERVE_WORDS above.
constexpr size_t kMaxReserveWords = ARIXV_KMAX_RESERVE_WORDS;

// See ARIXV_KMAX_DECODE_EXPANSION above.
constexpr size_t kMaxDecodeExpansion = ARIXV_KMAX_DECODE_EXPANSION;

// Up-front output reservation for decoders. The declared size is
// attacker-controlled, so cap the reservation at a multiple of the actual
// payload; a tiny hostile container then cannot force a huge allocation.
// Genuine output beyond the cap grows by reallocation.
constexpr size_t CappedDecodeReserve(size_t payload_size,
                                     size_t declared_size) {
  return payload_size > declared_size / kMaxDecodeExpansion
             ? declared_size
             : payload_size * kMaxDecodeExpansion;
}

// Bit-width field size for CodeUint; covers values 0..32.
constexpr int kUintLengthBits = ARIXV_KUINT_LENGTH_BITS;

// Number of payload bits after the implicit MSB that get length-specific slots.
constexpr int kUintHighBits = ARIXV_KUINT_HIGH_BITS;

// Remaining uint32 payload bits after kUintHighBits.
constexpr int kUintRestBits = ARIXV_KUINT_REST_BITS;

}  // namespace arixv

#endif  // ARIXV_TUNING_H_
