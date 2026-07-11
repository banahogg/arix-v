// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_RANGE_CODER_H_
#define ARIXV_RANGE_CODER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tuning.h"

namespace arixv {

#if ARIXV_KPROB_BITS == 12 && ARIXV_KPROB_SHIFT >= 1 && \
    ARIXV_KPROB_SHIFT <= 5 && ARIXV_KPROB_ADAPT_MIN_SHIFT == 1

struct Prob {
  static constexpr uint16_t kValueMask = (1u << kProbBits) - 1u;
  static constexpr uint16_t kFirstSteadyStage =
      static_cast<uint16_t>(1u << (kProbShift - 1));
  static constexpr uint16_t kPackedSaturation =
      kProbAdaptSaturation < kFirstSteadyStage
          ? kProbAdaptSaturation
          : static_cast<uint16_t>(kFirstSteadyStage - 1u);
  static_assert(kPackedSaturation < 16,
                "packed probability counter must fit in four bits");

  // Low kProbBits bits hold the probability. The high bits hold the warm-up
  // update count; kPackedSaturation means "steady state". For the default
  // schedule this is bitstream-identical to the old uint16_t counter because
  // stage 16 already uses kProbShift.
  uint16_t state = kProbInit;

  uint16_t raw() const { return state; }
  uint16_t value() const { return state & kValueMask; }
  static uint16_t ValueFromRaw(uint16_t raw) { return raw & kValueMask; }

  void Update(int bit) {
    UpdateFromRaw(state, bit);
  }

  void UpdateFromRaw(uint16_t old_state, int bit) {
    uint16_t count = old_state >> kProbBits;
    uint16_t v = old_state & kValueMask;
    int shift = kProbShift;
    if (count < kPackedSaturation) {
      ++count;
      shift = 32 - __builtin_clz(static_cast<unsigned int>(count));
    }
    if (bit == 0) {
      v = static_cast<uint16_t>(v + ((kProbScale - v) >> shift));
    } else {
      v = static_cast<uint16_t>(v - (v >> shift));
    }
    state = static_cast<uint16_t>((count << kProbBits) | v);
  }

  void UpdateSteady(int bit) {
    const uint16_t old_state = state;
    uint16_t v = old_state & kValueMask;
    if (bit == 0) {
      v = static_cast<uint16_t>(v + ((kProbScale - v) >> kProbShift));
    } else {
      v = static_cast<uint16_t>(v - (v >> kProbShift));
    }
    state = static_cast<uint16_t>((old_state & ~kValueMask) | v);
  }
};

static_assert(sizeof(Prob) == sizeof(uint16_t),
              "default probability model should stay packed");

#else

struct Prob {
  uint16_t v = kProbInit;
  // Saturating update count. Early updates use a larger step (rate ~1/(n+2))
  // so per-file models warm up fast — cold start dominates on small modules —
  // settling to the steady kProbShift rate. Deterministic and symmetric: both
  // sides see identical update sequences.
  uint16_t n = 0;

  uint16_t raw() const { return v; }
  uint16_t value() const { return v; }
  static uint16_t ValueFromRaw(uint16_t raw) { return raw; }

  int AdaptShift() {
    if (n >= kProbAdaptSaturation) {
      return kProbShift;
    }
    const uint16_t stage = ++n;
    // Bit width of stage (>= 1): position of the high bit plus one. Same
    // result as counting shifts, one instruction instead of a loop.
    int width = 32 - __builtin_clz(static_cast<unsigned int>(stage));
    if (width < kProbAdaptMinShift) {
      width = kProbAdaptMinShift;
    }
    return width < kProbShift ? width : kProbShift;
  }

  void Update(int bit) {
    const int shift = AdaptShift();
    if (bit == 0) {
      v = static_cast<uint16_t>(v + ((kProbScale - v) >> shift));
    } else {
      v = static_cast<uint16_t>(v - (v >> shift));
    }
  }

  void UpdateFromRaw(uint16_t, int bit) { Update(bit); }

  void UpdateSteady(int bit) {
    if (bit == 0) {
      v = static_cast<uint16_t>(v + ((kProbScale - v) >> kProbShift));
    } else {
      v = static_cast<uint16_t>(v - (v >> kProbShift));
    }
  }
};

#endif

class RangeEncoder {
 public:
  explicit RangeEncoder(std::vector<uint8_t>& out) : out_(out) {}

  void EncodeBit(Prob& p, int bit) {
    const uint16_t raw = p.raw();
    const uint32_t bound = (range_ >> kProbBits) * Prob::ValueFromRaw(raw);
    if (bit == 0) {
      range_ = bound;
    } else {
      low_ += bound;
      range_ -= bound;
    }
    p.UpdateFromRaw(raw, bit);
    Normalize();
  }

  void EncodeBitDirect(int bit) {
    range_ >>= 1;
    if (bit != 0) {
      low_ += range_;
    }
    Normalize();
  }

  void Flush() {
    for (int i = 0; i < kRangeFlushBytes; ++i) {
      ShiftLow();
    }
  }

 private:
  void Normalize() {
    while (range_ < kRangeTop) {
      ShiftLow();
      range_ <<= 8;
    }
  }

  void ShiftLow() {
    if (static_cast<uint32_t>(low_) < 0xFF000000u ||
        static_cast<uint32_t>(low_ >> 32) != 0) {
      uint8_t temp = cache_;
      do {
        out_.push_back(static_cast<uint8_t>(
            temp + static_cast<uint8_t>(low_ >> 32)));
        temp = 0xFF;
      } while (--cache_size_ != 0);
      cache_ = static_cast<uint8_t>(static_cast<uint32_t>(low_) >> 24);
    }
    ++cache_size_;
    low_ = static_cast<uint32_t>(low_) << 8;
  }

  std::vector<uint8_t>& out_;
  uint64_t low_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint8_t cache_ = 0;
  uint64_t cache_size_ = 1;
};

class RangeDecoder {
 public:
  RangeDecoder(const uint8_t* data, size_t size) : data_(data), size_(size) {
    (void)ReadByte();  // LZMA streams start with a dummy byte.
    for (int i = 0; i < 4; ++i) {
      code_ = (code_ << 8) | ReadByte();
    }
  }

  int DecodeBit(Prob& p) {
    const uint16_t raw = p.raw();
    const uint32_t bound = (range_ >> kProbBits) * Prob::ValueFromRaw(raw);
    int bit = 0;
    if (code_ < bound) {
      range_ = bound;
    } else {
      code_ -= bound;
      range_ -= bound;
      bit = 1;
    }
    p.UpdateFromRaw(raw, bit);
    Normalize();
    return bit;
  }

  int DecodeBitDirect() {
    range_ >>= 1;
    int bit = 0;
    if (code_ >= range_) {
      code_ -= range_;
      bit = 1;
    }
    Normalize();
    return bit;
  }

  bool exhausted() const { return exhausted_; }
  size_t consumed_size() const { return pos_; }

 private:
  uint8_t ReadByte() {
    if (pos_ >= size_) {
      exhausted_ = true;
      return 0;
    }
    return data_[pos_++];
  }

  void Normalize() {
    while (range_ < kRangeTop) {
      range_ <<= 8;
      code_ = (code_ << 8) | ReadByte();
    }
  }

  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  uint32_t code_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  bool exhausted_ = false;
};

}  // namespace arixv

#endif  // ARIXV_RANGE_CODER_H_
