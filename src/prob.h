// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_PROB_H_
#define ARIXV_PROB_H_

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "arena.h"
#include "range_coder.h"
#include "symbol_coder.h"
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

#ifdef ARIXV_TRACE_DECISIONS
// Diagnostics build only (tools/bench/ans_microbench.cpp): every adaptive
// encoder decision as (context, bit), in coding order.
struct TracedDecision {
  const Prob* ctx;
  bool bit;
};
inline std::vector<TracedDecision>* g_decision_trace = nullptr;
// Every encoder BitTree value as (tree, depth, value), in coding order.
struct TracedTree {
  const void* tree;
  int bits;
  uint32_t value;
};
inline std::vector<TracedTree>* g_tree_trace = nullptr;
#endif

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
#ifdef ARIXV_TRACE_DECISIONS
    if (g_decision_trace != nullptr) {
      g_decision_trace->push_back({&p, bit_for_encoder});
    }
#endif
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

  // One symbol of an adaptive kN-symbol CDF, on the rANS stream.
  template <int kN, int kRateCap>
  uint32_t CodeSymbol(SymbolCdf& t, uint32_t s) {
    if (g_stat_cat >= 0) {
      ++g_stat_adaptive[g_stat_cat][g_stat_site];
    }
    uint16_t c[17];
    (void)SymbolCdfLookup<kN>(t, kSymbolScale, c);
    const uint32_t lo = c[s];
    const uint32_t f = static_cast<uint32_t>(c[s + 1]) - lo;
    if (cost_sink_ != nullptr) {
      *cost_sink_ -= std::log2(static_cast<double>(f) / kSymbolScale);
    }
    rans_.Put(lo, f);
    SymbolCdfUpdate<kN, kRateCap>(t, static_cast<int>(s));
    return s;
  }

  // Stats instrumentation (ARIXV_STATS): while set, the entropy cost of
  // every decision is accumulated into *sink. Encoder-only; the decoder's
  // counterpart is a no-op so shared model code can call it unconditionally.
  void SetCostSink(double* sink) { cost_sink_ = sink; }

  // Range-coder bytes, then the rANS words (see symbol_coder.h).
  void Flush() {
    enc_.Flush();
    rans_.Flush(enc_.out());
  }
  bool exhausted() const { return false; }

 private:
  RangeEncoder& enc_;
  RansEncoder rans_;
  double* cost_sink_ = nullptr;
};

class BitDecoder {
 public:
  static constexpr bool kEncoding = false;
  static constexpr bool kRawResidual = false;

  explicit BitDecoder(RangeDecoder& dec)
      : dec_(dec), rans_(dec.data(), dec.size()) {}

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

  uint32_t DecodeSequence(Prob* probs, int n) {
    return dec_.DecodeSequence(probs, n);
  }

  template <int kN, int kRateCap>
  ARIXV_FORCE_INLINE uint32_t CodeSymbol(SymbolCdf& t, uint32_t /*s*/) {
    return static_cast<uint32_t>(rans_.template DecodeSymbol<kN, kRateCap>(t));
  }

  // Either stream ran out (decoders stop early on this). Only the forward
  // range stream running off the end means the payload was cut short; the
  // backward rANS stream running past the front means it is malformed.
  bool exhausted() const { return dec_.exhausted() || rans_.exhausted(); }
  bool range_exhausted() const { return dec_.exhausted(); }

  // The range bytes (from the front) and rANS words (from the back) must
  // tile the payload exactly, and the rANS state must end where it began.
  bool ConsumedExactly() const {
    return dec_.consumed_size() + rans_.consumed_size() == dec_.size() &&
           rans_.FinishedCleanly();
  }

  // No-op mirror of BitEncoder::SetCostSink so shared model code can call it.
  void SetCostSink(double*) {}

 private:
  RangeDecoder& dec_;
  RansDecoder rans_;
};

// A kBits-wide value coded MSB-first in <= 4-bit chunks (the first takes
// kBits % 4, or 4), each chunk one symbol of an adaptive CDF selected by the
// value prefix above it: the context structure of a binary tree with fewer,
// wider nodes. Value-initialized (zeroed) tables are uniform.
template <int kBits, int kRateCap = kSymbolRateCap>
struct BitTree {
  static_assert(kBits > 0 && kBits <= 16, "BitTree depth must be 1..16");
  static constexpr int kFirstChunk = kBits % 4 == 0 ? 4 : kBits % 4;
  static constexpr int NumTables() {
    int tables = 0;
    for (int prefix = 0, chunk = kFirstChunk; prefix < kBits;
         prefix += chunk, chunk = 4) {
      tables += 1 << prefix;
    }
    return tables;
  }
  std::array<SymbolCdf, NumTables()> tables{};

  template <typename Coder>
  uint32_t CodeValue(Coder& coder, uint32_t value) {
    if constexpr (Coder::kRawResidual) {
      return coder.CodeBitsDirect(value, kBits);
    } else {
#ifdef ARIXV_TRACE_DECISIONS
      if (Coder::kEncoding && g_tree_trace != nullptr) {
        g_tree_trace->push_back({this, kBits, value});
      }
#endif
      StatSiteScope<Coder> site(kSiteBitTree);
      const uint32_t out = CodeFrom<0>(coder, value, 0, 0);
      return Coder::kEncoding ? value : out;
    }
  }

 private:
  template <int kPrefix>
  static constexpr int Chunk() { return kPrefix == 0 ? kFirstChunk : 4; }

  template <int kPrefix, typename Coder>
  uint32_t CodeFrom(Coder& coder, uint32_t value, uint32_t prefix_value,
                    uint32_t base) {
    constexpr int kChunk = Chunk<kPrefix>();
    constexpr int kShift = kBits - kPrefix - kChunk;
    const uint32_t want =
        Coder::kEncoding ? (value >> kShift) & ((1u << kChunk) - 1u) : 0u;
    const uint32_t s = coder.template CodeSymbol<1 << kChunk, kRateCap>(
        tables[base + prefix_value], want);
    const uint32_t next = (prefix_value << kChunk) | s;
    if constexpr (kPrefix + kChunk < kBits) {
      return CodeFrom<kPrefix + kChunk>(coder, value, next,
                                        base + (1u << kPrefix));
    } else {
      return next;
    }
  }
};

// A uint is a bit length (bit tree) then its mantissa below the leading 1:
// the first kUintHighBits mantissa bits use per-(length, position)
// probabilities, deeper ones the shared `rest` probabilities.
//
// Lengths up to kUintHighBits use fewer than kUintHighBits positions, so
// their rows are packed triangularly inline (row b starts at (b-1)(b-2)/2).
// Longer rows and `rest` are only reached by values >= 2^kUintHighBits and
// live in a block allocated on first use: most contexts (word counts, small
// deltas) never need it. Same probabilities in the same roles as one eager
// [33][kUintHighBits] table, and a fresh block starts at kProbInit.
constexpr int kUintShortRows = kUintHighBits;  // bit lengths 1..kUintHighBits
constexpr int kUintShortProbs = kUintShortRows * (kUintShortRows - 1) / 2;
constexpr int kUintLongRows = 32 - kUintShortRows;  // kUintHighBits+1..32

struct UintLongRows {
  std::array<std::array<Prob, kUintHighBits>, kUintLongRows> high{};
  std::array<Prob, kUintRestBits> rest{};
};

struct UintModel {
  BitTree<kUintLengthBits> length;
  std::array<Prob, kUintShortProbs> short_rows{};
  ArenaPtr<UintLongRows> long_rows;

  // Probabilities for the first min(nbits - 1, kUintHighBits) mantissa bits.
  Prob* HighRow(Arena& arena, uint32_t nbits) {
    if (nbits <= static_cast<uint32_t>(kUintShortRows)) {
      return short_rows.data() + (nbits - 1) * (nbits - 2) / 2;
    }
    return Long(arena).high[nbits - kUintShortRows - 1].data();
  }
  Prob* Rest(Arena& arena) { return Long(arena).rest.data(); }

 private:
  UintLongRows& Long(Arena& arena) {
    if (!long_rows) {
      long_rows.Make(arena);
    }
    return *long_rows;
  }
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
uint32_t CodeUint(Coder& coder, Arena& arena, UintModel& model, uint32_t v) {
  if constexpr (Coder::kRawResidual) {
    (void)arena;
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
    uint32_t out = coder.DecodeSequence(model.HighRow(arena, nbits), high);
    if (mantissa > high) {
      out = (out << (mantissa - high)) |
            coder.DecodeSequence(model.Rest(arena), mantissa - high);
    }
    return (1u << mantissa) | out;
  }
  uint32_t out = 1u << (nbits - 1);
  Prob* const high_row = model.HighRow(arena, nbits);
  for (uint32_t j = 0; j + 1 < nbits; ++j) {
    const uint32_t src_pos = nbits - 2 - j;
    const bool hint = ((v >> src_pos) & 1u) != 0;
    Prob& p = (j < kUintHighBits) ? high_row[j]
                                  : model.Rest(arena)[j - kUintHighBits];
    const bool bit = coder.CodeBit(p, hint);
    if (bit) {
      out |= 1u << src_pos;
    }
  }
  return Coder::kEncoding ? v : out;
}

// An 8-bit value, symbol-for-symbol identical to BitTree<8, kRateCap> (a
// 16-symbol high-nibble root, then a 16-symbol low-nibble leaf per high
// nibble), with each leaf table allocated on first use: bytes after a given
// byte, and enum/ext-inst indices, use only a few high nibbles, so most of
// the 544-byte eager tree was never touched. A fresh leaf is zeroed
// (uniform), like the eager tree's untouched tables.
template <int kRateCap>
struct LazyLeafTree8 {
  static_assert(kByteBits == 8 && BitTree<kByteBits>::kFirstChunk == 4,
                "LazyLeafTree8 mirrors BitTree<8>'s 4/4 chunk layout");
  SymbolCdf root;
  std::array<ArenaPtr<SymbolCdf>, 16> leaf;

  template <typename Coder>
  uint32_t CodeValue(Coder& coder, Arena& arena, uint32_t value) {
    if constexpr (Coder::kRawResidual) {
      (void)arena;
      return coder.CodeBitsDirect(value, kByteBits);
    } else {
#ifdef ARIXV_TRACE_DECISIONS
      if (Coder::kEncoding && g_tree_trace != nullptr) {
        g_tree_trace->push_back({this, kByteBits, value});
      }
#endif
      StatSiteScope<Coder> site(kSiteBitTree);
      const uint32_t high = coder.template CodeSymbol<16, kRateCap>(
          root, (value >> 4) & 15u);
      ArenaPtr<SymbolCdf>& low_table = leaf[high];
      if (!low_table) {
        low_table.Make(arena);
      }
      const uint32_t low = coder.template CodeSymbol<16, kRateCap>(
          *low_table, value & 15u);
      return Coder::kEncoding ? value : ((high << 4) | low);
    }
  }
};

using ByteTree = LazyLeafTree8<kByteSymbolRateCap>;

// Order-1 byte model: one tree per previous byte, created on first use.
struct ByteModel {
  std::array<ArenaPtr<ByteTree>, kByteContextCount> tree{};
  Arena arena;  // owns the per-context trees and their leaf tables
};

template <typename Coder>
uint8_t CodeByte(Coder& coder, ByteModel& model, uint8_t prev, uint8_t b) {
  if constexpr (Coder::kRawResidual) {
    (void)model;
    (void)prev;
    return static_cast<uint8_t>(coder.CodeBitsDirect(b, kByteBits));
  }
  StatSiteScope<Coder> site(kSiteByte);
  ArenaPtr<ByteTree>& tree = model.tree[prev];
  if (!tree) {
    tree.Make(model.arena);
  }
  return static_cast<uint8_t>(tree->CodeValue(coder, model.arena, b));
}

}  // namespace arixv

#endif  // ARIXV_PROB_H_
