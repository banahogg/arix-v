// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// Adaptive multi-symbol coding for BitTree values: each tree is split
// MSB-first into <= 4-bit chunks, each chunk coded as one symbol of an
// adaptive 2..16-symbol CDF chosen by the value prefix above it, over an
// rANS stream that sits beside the binary range-coder stream.
//
// Payload layout (no length field): the range coder's bytes run forward from
// the start of the payload; the rANS words (little-endian uint16) run
// backward from the end. A valid payload is tiled exactly by the two, and the
// rANS state finishes at its initial value kRansL.

#ifndef ARIXV_SYMBOL_CODER_H_
#define ARIXV_SYMBOL_CODER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tuning.h"

#if defined(__SSE2__) && !defined(ARIXV_NO_SIMD)
#define ARIXV_SYMBOL_SSE2 1
#include <emmintrin.h>
#else
#define ARIXV_SYMBOL_SSE2 0
#endif

namespace arixv {

constexpr int kSymbolScaleBits = 15;
constexpr uint32_t kSymbolScale = 1u << kSymbolScaleBits;
constexpr uint32_t kRansL = 1u << 16;

// One adaptive CDF over N = 2..16 symbols in 32 bytes. Stored as offsets
// from the uniform CDF so that a zeroed table *is* the uniform model (every
// BitTree{} reset and lazy zeroing keeps working). Lane 0 of a CDF is always
// 0, so d[0] holds the warm-up count instead.
//
// Coding CDF: C[0] = 0; C[i] = d[i] + U[i] for 1 <= i <= 15; C[16] = 32768.
// U[i] = i * 32768 / N for i <= N and 32768 + (i - N) beyond, so C[N] is
// always 32768 and unused lanes stay >= 32768 (never selected). Adaptation
// moves C[i] toward i (i <= s) or 32768 - N + i (i > s); that keeps C
// strictly increasing, i.e. every symbol keeps frequency >= 1.
struct alignas(16) SymbolCdf {
  uint16_t d[16];
};
static_assert(sizeof(SymbolCdf) == 32, "SymbolCdf must stay one half line");

template <int kN>
struct SymbolCdfConst {
  static_assert(kN >= 2 && kN <= 16, "alphabet must be 2..16 symbols");
  static constexpr uint16_t U(int i) {
    return static_cast<uint16_t>(i <= kN ? i * static_cast<int>(kSymbolScale / kN)
                                         : static_cast<int>(kSymbolScale) + (i - kN));
  }
  alignas(16) static constexpr uint16_t kUniform[16] = {
      U(0), U(1), U(2),  U(3),  U(4),  U(5),  U(6),  U(7),
      U(8), U(9), U(10), U(11), U(12), U(13), U(14), U(15)};
  static constexpr uint16_t kTop = static_cast<uint16_t>(kSymbolScale - kN);
};

// Coding CDF into c[0..16]; returns the largest s with c[s] <= v (v < 32768),
// or -1 when called with v = kSymbolScale (no search wanted).
template <int kN>
inline int SymbolCdfLookup(const SymbolCdf& t, uint32_t v, uint16_t c[17]) {
  using K = SymbolCdfConst<kN>;
#if ARIXV_SYMBOL_SSE2
  const __m128i* d = reinterpret_cast<const __m128i*>(t.d);
  const __m128i* u = reinterpret_cast<const __m128i*>(K::kUniform);
  const __m128i lane0 = _mm_setr_epi16(-1, 0, 0, 0, 0, 0, 0, 0);
  const __m128i c0 = _mm_andnot_si128(
      lane0, _mm_add_epi16(_mm_load_si128(d), _mm_load_si128(u)));
  const __m128i c1 = _mm_add_epi16(_mm_load_si128(d + 1), _mm_load_si128(u + 1));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(c), c0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(c + 8), c1);
  c[16] = static_cast<uint16_t>(kSymbolScale);
  // C <= v  <=>  saturating C - v == 0. C is increasing, so the true lanes
  // form a prefix and the count is ctz(~mask) (popcount would be a libgcc
  // call without -mpopcnt).
  const __m128i vv = _mm_set1_epi16(static_cast<short>(v));
  const __m128i z = _mm_setzero_si128();
  const __m128i le0 = _mm_cmpeq_epi16(_mm_subs_epu16(c0, vv), z);
  const __m128i le1 = _mm_cmpeq_epi16(_mm_subs_epu16(c1, vv), z);
  const uint32_t mask =
      static_cast<uint32_t>(_mm_movemask_epi8(_mm_packs_epi16(le0, le1)));
  return __builtin_ctz(~mask) - 1;
#else
  int s = -1;
  c[0] = 0;
  for (int i = 1; i < 16; ++i) {
    c[i] = static_cast<uint16_t>(t.d[i] + K::kUniform[i]);
  }
  c[16] = static_cast<uint16_t>(kSymbolScale);
  for (int i = 0; i < 16; ++i) {
    if (c[i] <= v) s = i;
    else break;
  }
  return s;
#endif
}

// Moves the CDF toward symbol s at the given shift; keeps d[0].
template <int kN>
inline void SymbolCdfAdapt(SymbolCdf& t, int s, int rate) {
  using K = SymbolCdfConst<kN>;
  const uint16_t count = t.d[0];
#if ARIXV_SYMBOL_SSE2
  const __m128i shift = _mm_cvtsi32_si128(rate);
  const __m128i top = _mm_set1_epi16(static_cast<short>(K::kTop));
  const __m128i sv = _mm_set1_epi16(static_cast<short>(s));
  const __m128i lanes[2] = {_mm_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7),
                            _mm_setr_epi16(8, 9, 10, 11, 12, 13, 14, 15)};
  __m128i* d = reinterpret_cast<__m128i*>(t.d);
  const __m128i* u = reinterpret_cast<const __m128i*>(K::kUniform);
  for (int k = 0; k < 2; ++k) {
    const __m128i dv = _mm_load_si128(d + k);
    const __m128i c = _mm_add_epi16(dv, _mm_load_si128(u + k));
    const __m128i up = _mm_cmpgt_epi16(lanes[k], sv);
    const __m128i target = _mm_add_epi16(lanes[k], _mm_and_si128(up, top));
    const __m128i delta = _mm_sra_epi16(_mm_sub_epi16(target, c), shift);
    _mm_store_si128(d + k, _mm_add_epi16(dv, delta));
  }
#else
  for (int i = 1; i < 16; ++i) {
    const uint16_t c = static_cast<uint16_t>(t.d[i] + K::kUniform[i]);
    const uint16_t target =
        static_cast<uint16_t>(i + (i > s ? K::kTop : 0));
    // Same as the 16-bit lane arithmetic: wrap, then arithmetic shift.
    const int16_t diff = static_cast<int16_t>(static_cast<uint16_t>(target - c));
    const int delta = diff >= 0 ? (diff >> rate) : -((-diff + (1 << rate) - 1) >> rate);
    t.d[i] = static_cast<uint16_t>(t.d[i] + delta);
  }
#endif
  t.d[0] = count;
}

// Count-warmed adaptation: shift 1, 2, 2, 3, ... capped at kRateCap; the
// count saturates once the cap is reached.
template <int kN, int kRateCap>
inline void SymbolCdfUpdate(SymbolCdf& t, int s) {
  static_assert(kRateCap >= 1 && kRateCap <= 14, "bad symbol rate cap");
  constexpr uint16_t kSaturation = static_cast<uint16_t>((1u << kRateCap) - 1u);
  const uint16_t count = t.d[0];
  int rate = 32 - __builtin_clz(static_cast<unsigned>(count) + 1u);
  if (rate > kRateCap) rate = kRateCap;
  SymbolCdfAdapt<kN>(t, s, rate);
  if (count < kSaturation) t.d[0] = static_cast<uint16_t>(count + 1);
}

// Steady-rate update without touching the warm-up count (shadow training).
template <int kN, int kRateCap>
inline void SymbolCdfTrain(SymbolCdf& t, int s) {
  SymbolCdfAdapt<kN>(t, s, kRateCap);
}

// Buffers symbols in coding order; Flush rANS-encodes them in reverse and
// appends the words, so the decoder (reading backward from the end) sees
// them in forward order.
class RansEncoder {
 public:
  void Put(uint32_t lo, uint32_t freq) { syms_.push_back(lo | (freq << 16)); }

  void Flush(std::vector<uint8_t>& out) {
    if (syms_.empty()) {
      return;  // a stream with no symbols costs nothing
    }
    uint32_t x = kRansL;
    for (size_t i = syms_.size(); i-- > 0;) {
      const uint32_t lo = syms_[i] & 0xffffu;
      const uint32_t f = syms_[i] >> 16;
      if (x >= ((kRansL >> kSymbolScaleBits) << 16) * f) {
        PutWord(out, x);
        x >>= 16;
      }
      x = ((x / f) << kSymbolScaleBits) + (x % f) + lo;
    }
    PutWord(out, x);
    PutWord(out, x >> 16);
    syms_.clear();
  }

 private:
  static void PutWord(std::vector<uint8_t>& out, uint32_t w) {
    out.push_back(static_cast<uint8_t>(w));
    out.push_back(static_cast<uint8_t>(w >> 8));
  }
  std::vector<uint32_t> syms_;
};

class RansDecoder {
 public:
  RansDecoder(const uint8_t* data, size_t size)
      : begin_(data), end_(data + size), p_(data + size) {}

  template <int kN, int kRateCap>
  int DecodeSymbol(SymbolCdf& t) {
    if (!started_) {
      started_ = true;
      x_ = ReadWord() << 16;
      x_ |= ReadWord();
    }
    uint16_t c[17];
    const uint32_t slot = x_ & (kSymbolScale - 1u);
    const int s = SymbolCdfLookup<kN>(t, slot, c);
    const uint32_t lo = c[s];
    const uint32_t f = static_cast<uint32_t>(c[s + 1]) - lo;
    x_ = f * (x_ >> kSymbolScaleBits) + slot - lo;
    if (x_ < kRansL) {
      x_ = (x_ << 16) | ReadWord();
    }
    SymbolCdfUpdate<kN, kRateCap>(t, s);
    return s;
  }

  bool exhausted() const { return exhausted_; }
  size_t consumed_size() const { return static_cast<size_t>(end_ - p_); }
  // A stream that decoded symbols must end in the encoder's initial state.
  bool FinishedCleanly() const { return !started_ || x_ == kRansL; }

 private:
  uint32_t ReadWord() {
    if (p_ - begin_ < 2) {
      exhausted_ = true;
      return 0;
    }
    p_ -= 2;
    return static_cast<uint32_t>(p_[0]) | (static_cast<uint32_t>(p_[1]) << 8);
  }

  const uint8_t* begin_;
  const uint8_t* end_;
  const uint8_t* p_;
  uint32_t x_ = 0;
  bool started_ = false;
  bool exhausted_ = false;
};

}  // namespace arixv

#endif  // ARIXV_SYMBOL_CODER_H_
