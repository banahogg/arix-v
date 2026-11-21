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

  // A fresh (warm-up count 0) probability whose bit-0 probability is
  // value / kProbScale, for cold-start priors.
  static Prob WithValue(uint16_t value) {
    Prob p;
    p.state = value & kValueMask;
    return p;
  }

  uint16_t raw() const { return state; }
  uint16_t value() const { return state & kValueMask; }
  static uint16_t ValueFromRaw(uint16_t raw) { return raw & kValueMask; }

  void Update(int bit) {
    UpdateFromRaw(state, bit);
  }

  void UpdateFromRaw(uint16_t old_state, int bit) {
    uint32_t count = old_state >> kProbBits;
    uint32_t v = old_state & kValueMask;
    uint32_t shift;
    if constexpr (kProbShift == 3 && kPackedSaturation == 3) {
      shift = 1u + ((count + 1u) >> 1);
      count += count < 3u ? 1u : 0u;
    } else {
      shift = kProbShift;
      if (count < kPackedSaturation) {
        ++count;
        shift = 32u - static_cast<uint32_t>(
                          __builtin_clz(static_cast<unsigned int>(count)));
      }
    }
    if (bit == 0) {
      v = v + ((kProbScale - v) >> shift);
    } else {
      v = v - (v >> shift);
    }
    state = static_cast<uint16_t>((count << kProbBits) | v);
  }

  void UpdateFromRawBranchless(uint16_t old_state, int bit) {
    uint32_t count = old_state >> kProbBits;
    const uint32_t v = old_state & kValueMask;
    uint32_t shift;
    if constexpr (kProbShift == 3 && kPackedSaturation == 3) {
      shift = 1u + ((count + 1u) >> 1);
      count += count < 3u ? 1u : 0u;
    } else {
      shift = kProbShift;
      if (count < kPackedSaturation) {
        ++count;
        shift = 32u - static_cast<uint32_t>(
                          __builtin_clz(static_cast<unsigned int>(count)));
      }
    }
    const uint32_t v0 = v + ((kProbScale - v) >> shift);
    const uint32_t v1 = v - (v >> shift);
    const uint32_t new_v = bit ? v1 : v0;
    state = static_cast<uint16_t>((count << kProbBits) | new_v);
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

  static Prob WithValue(uint16_t value) {
    Prob p;
    p.v = value;
    return p;
  }

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
  void UpdateFromRawBranchless(uint16_t, int bit) { Update(bit); }
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

  // Adaptive minimal flush:
  // - Empty stream (range_ == 0xFFFFFFFFu): no bits were coded, emit 0 bytes
  //   (all 4 decoder lookahead bytes are don't-care).
  // - Wide final range (range_ >= 0x01FFFFFFu = 2 * 2^24 - 1): rounding low_
  //   up to a multiple of 2^24 guarantees every 24-bit suffix lies inside
  //   [low_, low_ + range_), so only 2 flush bytes are needed (3 slack bytes).
  // - Narrow final range (2^24 <= range_ < 0x01FFFFFFu): round low_ up to a
  //   multiple of 2^16 and emit 3 flush bytes (2 slack bytes).
  static constexpr uint32_t kRangeShortFlushMin = 0x01FFFFFFu;

  void Flush() {
    if (range_ == 0xFFFFFFFFu) {
      return;
    }
    if (range_ >= kRangeShortFlushMin) {
      low_ = (low_ + 0xFFFFFFu) & ~static_cast<uint64_t>(0xFFFFFFu);
      for (int i = 0; i < 2; ++i) {
        ShiftLow();
      }
      return;
    }
    low_ = (low_ + 0xFFFFu) & ~static_cast<uint64_t>(0xFFFFu);
    for (int i = 0; i < kRangeFlushBytes; ++i) {
      ShiftLow();
    }
  }

  std::vector<uint8_t>& out() { return out_; }

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
        const uint8_t b =
            static_cast<uint8_t>(temp + static_cast<uint8_t>(low_ >> 32));
        // LZMA's first byte sits above the initial 32-bit window and is
        // always 0; it is not stored.
        if (started_) {
          out_.push_back(b);
        }
        started_ = true;
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
  bool started_ = false;
};

class RangeDecoder {
 public:
  RangeDecoder(const uint8_t* data, size_t size) : data_(data), size_(size) {
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

  // Decodes n bits MSB first, bit i using its own probs[i].
  uint32_t DecodeSequence(Prob* probs, int n) {
    State st = Load();
    uint32_t value = 0;
    for (int i = 0; i < n; ++i) {
      value = (value << 1) |
              static_cast<uint32_t>(DecodeBitLocal(st, probs[i]));
    }
    Store(st);
    return value;
  }

  int DecodeBitDirect() {
    range_ >>= 1;
    const int bit = code_ >= range_ ? 1 : 0;
    code_ -= range_ & -static_cast<uint32_t>(bit);
    Normalize();
    return bit;
  }

  static constexpr uint32_t kRangeShortFlushMin = 0x01FFFFFFu;
  static constexpr size_t kMaxRangeTailSlack = 4;

  // Tail slack depends solely on range_, which stays bit-identical between
  // encoder and decoder: 4 for an empty stream (0 bits decoded), 3 when
  // range_ >= 0x01FFFFFFu, and 2 otherwise.
  size_t TailSlack() const {
    return static_cast<size_t>(kRangeTailSlack) +
           static_cast<size_t>(range_ >= kRangeShortFlushMin) +
           static_cast<size_t>(range_ == 0xFFFFFFFFu);
  }

  bool exhausted() const { return exhausted_ || pos_ > size_ + TailSlack(); }
  // Bytes the encoder actually wrote (reads minus the don't-care tail).
  size_t consumed_size() const {
    const size_t slack = TailSlack();
    return pos_ >= slack ? pos_ - slack : 0;
  }
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  struct State {
    uint32_t code;
    uint32_t range;
    size_t pos;
  };

  State Load() const { return State{code_, range_, pos_}; }
  void Store(const State& st) {
    code_ = st.code;
    range_ = st.range;
    pos_ = st.pos;
  }

  // Same arithmetic as DecodeBit/Normalize/ReadByte, on local state.
  // Used for uint mantissa runs (~42% branch mispredict rate), so keep the
  // code < bound decision and probability update branchless.
  int DecodeBitLocal(State& st, Prob& p) {
    const uint16_t raw = p.raw();
    const uint32_t bound = (st.range >> kProbBits) * Prob::ValueFromRaw(raw);
    const int bit = st.code >= bound ? 1 : 0;
    const uint32_t mask = -static_cast<uint32_t>(bit);
    st.code -= bound & mask;
    st.range = (bound & ~mask) | ((st.range - bound) & mask);
    p.UpdateFromRawBranchless(raw, bit);
    while (st.range < kRangeTop) {
      st.range <<= 8;
      st.code = (st.code << 8) | Fetch(st.pos);
    }
    return bit;
  }

  // Up to kMaxRangeTailSlack reads past the end are virtual zeros (the flush's
  // don't-care tail); anything further means the stream was cut short. The
  // tighter bound pos_ <= size_ + TailSlack() is enforced by exhausted().
  uint8_t Fetch(size_t& pos) {
    if (pos < size_) {
      return data_[pos++];
    }
    if (pos < size_ + kMaxRangeTailSlack) {
      ++pos;
      return 0;
    }
    exhausted_ = true;
    return 0;
  }
  uint8_t ReadByte() { return Fetch(pos_); }

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
