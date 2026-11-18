// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_PROB_H_
#define ARIXV_PROB_H_

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "range_coder.h"
#include "tuning.h"

namespace arixv {

// ARIXV_STATS decision counting (encoder-only diagnostics). The encoder makes
// exactly the decisions the decoder will, so counting them at encode time
// measures the decoder's serial decision chain. g_stat_cat is the field
// category (set by CodeModule, -1 = off); g_stat_site is the coding primitive.
enum StatSite {
  kSiteFlag = 0, kSiteBitTree, kSiteUintLength, kSiteUintMantissa, kSiteByte,
  kSiteCount,
};
constexpr int kStatMaxCats = 16;
inline int g_stat_cat = -1;
inline int g_stat_site = kSiteFlag;
inline uint64_t g_stat_adaptive[kStatMaxCats][kSiteCount];
inline uint64_t g_stat_direct[kStatMaxCats];

// Tags decisions made inside a primitive. Only the outermost primitive wins
// (CodeUint's length tree counts as uint length, not as a generic bit tree).
// Compiles away for decoders.
template <typename Coder>
struct StatSiteScope {
  int saved = kSiteFlag;
  explicit StatSiteScope(StatSite site) {
    if constexpr (Coder::kEncoding) {
      saved = g_stat_site;
      if (saved == kSiteFlag) g_stat_site = site;
    }
  }
  ~StatSiteScope() {
    if constexpr (Coder::kEncoding) g_stat_site = saved;
  }
};

class BitEncoder {
 public:
  static constexpr bool kEncoding = true;
  static constexpr bool kRawResidual = false;

  explicit BitEncoder(RangeEncoder& enc) : enc_(enc) {}

  bool CodeBit(Prob& p, bool bit_for_encoder) {
    if (g_stat_cat >= 0) {
      ++g_stat_adaptive[g_stat_cat][g_stat_site];
    }
    if (cost_sink_ != nullptr) {
      const double p0 = static_cast<double>(p.value()) / kProbScale;
      *cost_sink_ -= std::log2(bit_for_encoder ? 1.0 - p0 : p0);
    }
    enc_.EncodeBit(p, bit_for_encoder ? 1 : 0);
    return bit_for_encoder;
  }

  uint32_t CodeBitsDirect(uint32_t value, int nbits) {
    if (g_stat_cat >= 0) {
      g_stat_direct[g_stat_cat] += static_cast<uint64_t>(nbits);
    }
    if (cost_sink_ != nullptr) {
      *cost_sink_ += nbits;
    }
    for (int i = nbits - 1; i >= 0; --i) {
      enc_.EncodeBitDirect(static_cast<int>((value >> i) & 1u));
    }
    return value;
  }

  // Stats instrumentation (ARIXV_STATS): while set, the entropy cost of
  // every decision is accumulated into *sink. Encoder-only; the decoder's
  // counterpart is a no-op so shared model code can call it unconditionally.
  void SetCostSink(double* sink) { cost_sink_ = sink; }

  void Flush() { enc_.Flush(); }
  bool exhausted() const { return false; }

 private:
  RangeEncoder& enc_;
  double* cost_sink_ = nullptr;
};

class BitDecoder {
 public:
  static constexpr bool kEncoding = false;
  static constexpr bool kRawResidual = false;

  explicit BitDecoder(RangeDecoder& dec) : dec_(dec) {}

  bool CodeBit(Prob& p, bool /*bit_for_encoder*/) {
    return dec_.DecodeBit(p) != 0;
  }

  uint32_t CodeBitsDirect(uint32_t /*value*/, int nbits) {
    uint32_t value = 0;
    for (int i = 0; i < nbits; ++i) {
      value = (value << 1) | static_cast<uint32_t>(dec_.DecodeBitDirect());
    }
    return value;
  }

  template <int kBits>
  uint32_t DecodeTree(Prob* probs) {
    return dec_.template DecodeTree<kBits>(probs);
  }
  uint32_t DecodeSequence(Prob* probs, int n) {
    return dec_.DecodeSequence(probs, n);
  }

  bool exhausted() const { return dec_.exhausted(); }

  // No-op mirror of BitEncoder::SetCostSink so shared model code can call it.
  void SetCostSink(double*) {}

 private:
  RangeDecoder& dec_;
};

template <int kBits>
struct BitTree {
  static_assert(kBits > 0, "BitTree must contain at least one bit");
  std::array<Prob, (1u << kBits) - 1> probs{};

  template <typename Coder>
  uint32_t CodeValue(Coder& coder, uint32_t value) {
    if constexpr (Coder::kRawResidual) {
      return coder.CodeBitsDirect(value, kBits);
    }
    if constexpr (!Coder::kEncoding && !Coder::kRawResidual) {
      return coder.template DecodeTree<kBits>(probs.data());
    }
    StatSiteScope<Coder> site(kSiteBitTree);
    uint32_t node = 1;
    uint32_t out = 0;
    for (int bit_index = kBits - 1; bit_index >= 0; --bit_index) {
      const bool hint = ((value >> bit_index) & 1u) != 0;
      const bool bit = coder.CodeBit(probs[node - 1], hint);
      out = (out << 1) | static_cast<uint32_t>(bit);
      node = (node << 1) | static_cast<uint32_t>(bit);
    }
    return Coder::kEncoding ? value : out;
  }
};

struct UintModel {
  BitTree<kUintLengthBits> length;
  std::array<std::array<Prob, kUintHighBits>, 33> high{};
  std::array<Prob, kUintRestBits> rest{};
};

inline uint32_t BitWidth(uint32_t v) {
  uint32_t n = 0;
  while (v != 0) {
    ++n;
    v >>= 1;
  }
  return n;
}

template <typename Coder>
uint32_t CodeUint(Coder& coder, UintModel& model, uint32_t v) {
  if constexpr (Coder::kRawResidual) {
    (void)model;
    return coder.CodeUint32(v);
  }
  uint32_t nbits = Coder::kEncoding ? BitWidth(v) : 0;
  {
    StatSiteScope<Coder> site(kSiteUintLength);
    nbits = model.length.CodeValue(coder, nbits);
  }
  if (nbits == 0) {
    return 0;
  }
  StatSiteScope<Coder> site(kSiteUintMantissa);
  // Invalid streams can spell larger lengths; clamp to the modeled uint32
  // domain to avoid out-of-bounds contexts while keeping decoding deterministic.
  if (nbits > 32) {
    nbits = 32;
  }

  if constexpr (!Coder::kEncoding && !Coder::kRawResidual) {
    const int mantissa = static_cast<int>(nbits) - 1;
    const int high = mantissa < kUintHighBits ? mantissa : kUintHighBits;
    uint32_t out = coder.DecodeSequence(model.high[nbits].data(), high);
    if (mantissa > high) {
      out = (out << (mantissa - high)) |
            coder.DecodeSequence(model.rest.data(), mantissa - high);
    }
    return (1u << mantissa) | out;
  }
  uint32_t out = 1u << (nbits - 1);
  for (uint32_t j = 0; j + 1 < nbits; ++j) {
    const uint32_t src_pos = nbits - 2 - j;
    const bool hint = ((v >> src_pos) & 1u) != 0;
    Prob& p = (j < kUintHighBits) ? model.high[nbits][j]
                                  : model.rest[j - kUintHighBits];
    const bool bit = coder.CodeBit(p, hint);
    if (bit) {
      out |= 1u << src_pos;
    }
  }
  return Coder::kEncoding ? v : out;
}

template <typename Coder>
int CodeRank(Coder& coder, Prob slots[kRankCount + 1], int rank) {
  for (int i = 0; i < kRankCount; ++i) {
    if (coder.CodeBit(slots[i], rank == i)) {
      return i;
    }
  }
  return kRankEscape;
}

struct ByteModel {
  std::array<std::unique_ptr<BitTree<kByteBits>>, kByteContextCount> tree{};
  std::array<uint32_t, kByteContextCount> stamp{};
  uint32_t generation = 1;
};

inline void BeginByteModelGeneration(ByteModel& model) {
  ++model.generation;
  if (model.generation == 0) {
    model.stamp.fill(0);
    model.generation = 1;
  }
}

template <typename Coder>
uint8_t CodeByte(Coder& coder, ByteModel& model, uint8_t prev, uint8_t b) {
  if constexpr (Coder::kRawResidual) {
    (void)model;
    (void)prev;
    return static_cast<uint8_t>(coder.CodeBitsDirect(b, kByteBits));
  }
  StatSiteScope<Coder> site(kSiteByte);
  std::unique_ptr<BitTree<kByteBits>>& tree = model.tree[prev];
  if (!tree) {
    tree = std::make_unique<BitTree<kByteBits>>();
  } else if (model.stamp[prev] != model.generation) {
    *tree = BitTree<kByteBits>{};
  }
  model.stamp[prev] = model.generation;
  return static_cast<uint8_t>(tree->CodeValue(coder, b));
}

}  // namespace arixv

#endif  // ARIXV_PROB_H_
