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
#define ARIXV_KPROB_SHIFT 3
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
#define ARIXV_KRANGE_FLUSH_BYTES 3
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
// Derived so that overriding ARIXV_KUINT_HIGH_BITS alone stays valid (24 for
// the default 8).
#define ARIXV_KUINT_REST_BITS (32 - ARIXV_KUINT_HIGH_BITS)
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

// Container mode for verbatim payloads.
constexpr uint8_t kModeStored = ARIXV_KMODE_STORED;

// Container mode for M1 range-coded order-1 byte payloads.
constexpr uint8_t kModeCoded = ARIXV_KMODE_CODED;

// Container mode for the M2 SPIR-V structural model.
constexpr uint8_t kModeSpirv = ARIXV_KMODE_SPIRV;

// Maximum byte count encoded by the compact uint32 size field.
constexpr int kMaxSizeVarintBytes = ARIXV_KMAX_SIZE_VARINT_BYTES;

// Maximum number of bytes emitted by RangeEncoder::Flush().
constexpr int kRangeFlushBytes = ARIXV_KRANGE_FLUSH_BYTES;

// Minimum number of bytes the range decoder reads past the encoder's last byte
// (the narrow-range 3-byte flush leaves 2 don't-care tail bytes; wide-range
// flushes emit 2 bytes with 3 slack bytes, and empty streams emit 0 with 4).
constexpr int kRangeTailSlack = 5 - kRangeFlushBytes;
static_assert(kRangeTailSlack == 2, "flush/slack must cover a 4-byte window");

// Byte contexts used by the M1 order-1 byte model.
constexpr int kByteContextCount = ARIXV_KBYTE_CONTEXT_COUNT;

// Number of bits in a byte.
constexpr int kByteBits = ARIXV_KBYTE_BITS;

// Recency-list capacities for rank coding. Caps bound tracker
// memory; drop-oldest eviction past the cap.
constexpr int kRankListCap = ARIXV_KRANK_LIST_CAP;
constexpr int kRankListCapIds = ARIXV_KRANK_LIST_CAP_IDS;
constexpr int kRankListCapTypes = ARIXV_KRANK_LIST_CAP_TYPES;

// Symbol-coded BitTree CDFs (symbol_coder.h): adaptation shift starts at 1
// and grows with the per-table count up to the tree's cap (also the steady
// rate used for shadow training). Byte trees (high-entropy string and
// fallback bytes) adapt more slowly.
#ifndef ARIXV_KSYMBOL_RATE_CAP
#define ARIXV_KSYMBOL_RATE_CAP 5
#endif
#ifndef ARIXV_KBYTE_SYMBOL_RATE_CAP
#define ARIXV_KBYTE_SYMBOL_RATE_CAP 7
#endif
constexpr int kSymbolRateCap = ARIXV_KSYMBOL_RATE_CAP;
constexpr int kByteSymbolRateCap = ARIXV_KBYTE_SYMBOL_RATE_CAP;

// Result-ID prediction: the smallest never-seen ID above the previous
// result ID, scanning at most this many already-seen IDs (bounds decode work
// on hostile input; falls back to previous + 1).
#ifndef ARIXV_KRESULT_SCAN_CAP
#define ARIXV_KRESULT_SCAN_CAP 64
#endif
constexpr int kResultScanCap = ARIXV_KRESULT_SCAN_CAP;

// Grammar cold-start priors (docs/development/ratio.md item 7): initial
// bit-0 probability (of kProbScale) for a validity flag — known opcode,
// grammar enumerant, word count consistent with the grammar, canonical header
// fields — so a fresh flag starts on the spec-valid side: 1/32. Insensitive
// from 1/32 to 1/2048 on the corpus; the weakest of those is kept.
#ifndef ARIXV_KVALID_FLAG_PRIOR
#define ARIXV_KVALID_FLAG_PRIOR (ARIXV_KPROB_SCALE / 32)
#endif
constexpr uint16_t kValidFlagPrior = static_cast<uint16_t>(ARIXV_KVALID_FLAG_PRIOR);

// String match model: log2 entries of the order-2 context hash table
// (positions of the byte that last followed each context; 1024 entries is a
// 4 KB table, within ~1 KB of an exact 64K-entry one on the corpus). The
// primed table is generated (scripts/gen_grammar_tables.py, match_bits), so
// this is not a build-time knob; arixv_test checks the two agree.
constexpr int kStringMatchBits = 10;

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
// A 32-bit value has up to 31 mantissa bits below its leading 1; the shared
// `rest` probabilities must cover every one past kUintHighBits, or CodeUint
// indexes past them (an override of ARIXV_KUINT_HIGH_BITS alone used to do
// exactly that with the old fixed default of 24).
static_assert(kUintHighBits >= 0 && kUintHighBits + kUintRestBits >= 31,
              "kUintRestBits must cover the mantissa bits past kUintHighBits");

}  // namespace arixv

#endif  // ARIXV_TUNING_H_
