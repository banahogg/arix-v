// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_PROB_H_
#define ARIXV_PROB_H_

#include <array>
#ifdef ARIXV_ENABLE_STATS
#include <cmath>
#endif
#include <cstdint>
#include <memory>
#include <vector>

#include "arena.h"
#include "platform.h"
#include "range_coder.h"
#include "symbol_coder.h"
#include "tuning.h"

namespace arixv {
inline namespace v1 {

// ARIXV_STATS decision counting (encoder-only diagnostics; compiled in only
// under -DARIXV_ENABLE_STATS so normal library builds have no mutable globals
// and Encode() is cleanly thread-safe).
enum StatSite {
  kSiteFlag = 0, kSiteBitTree, kSiteUintLength, kSiteUintMantissa, kSiteByte,
  kSiteCount,
};
#ifdef ARIXV_ENABLE_STATS
constexpr int kStatMaxCats = 16;
inline int g_stat_cat = -1;
inline int g_stat_site = kSiteFlag;
inline uint64_t g_stat_adaptive[kStatMaxCats][kSiteCount];
inline uint64_t g_stat_direct[kStatMaxCats];
#endif

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
// Compiles away for decoders and non-stats builds.
template <typename Coder>
struct StatSiteScope {
#ifdef ARIXV_ENABLE_STATS
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
#else
  explicit StatSiteScope(StatSite) {}
#endif
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
#ifdef ARIXV_ENABLE_STATS
    if (g_stat_cat >= 0) {
      ++g_stat_adaptive[g_stat_cat][g_stat_site];
    }
    if (cost_sink_ != nullptr) {
      const double p0 = static_cast<double>(p.value()) / kProbScale;
      *cost_sink_ -= std::log2(bit_for_encoder ? 1.0 - p0 : p0);
    }
#endif
    enc_.EncodeBit(p, bit_for_encoder ? 1 : 0);
    return bit_for_encoder;
  }

  uint32_t CodeBitsDirect(uint32_t value, int nbits) {
#ifdef ARIXV_ENABLE_STATS
    if (g_stat_cat >= 0) {
      g_stat_direct[g_stat_cat] += static_cast<uint64_t>(nbits);
    }
    if (cost_sink_ != nullptr) {
      *cost_sink_ += nbits;
    }
#endif
    for (int i = nbits - 1; i >= 0; --i) {
      enc_.EncodeBitDirect(static_cast<int>((value >> i) & 1u));
    }
    return value;
  }

  // One symbol of an adaptive kN-symbol CDF, on the rANS stream.
  template <int kN, int kRateCap>
  uint32_t CodeSymbol(SymbolCdf& t, uint32_t s) {
#ifdef ARIXV_ENABLE_STATS
    if (g_stat_cat >= 0) {
      ++g_stat_adaptive[g_stat_cat][g_stat_site];
    }
#endif
    using K = SymbolCdfConst<kN>;
    const uint32_t lo =
        s == 0 ? 0u : static_cast<uint16_t>(t.d[s] + K::kUniform[s]);
    uint32_t hi;
    if constexpr (kN < 16) {
      hi = static_cast<uint16_t>(t.d[s + 1] + K::kUniform[s + 1]);
    } else {
      hi = s + 1 < 16 ? static_cast<uint16_t>(t.d[s + 1] + K::kUniform[s + 1])
                      : kSymbolScale;
    }
    const uint32_t f = hi - lo;
#ifdef ARIXV_ENABLE_STATS
    if (cost_sink_ != nullptr) {
      *cost_sink_ -= std::log2(static_cast<double>(f) / kSymbolScale);
    }
#endif
    rans_.Put(lo, f);
    SymbolCdfUpdate<kN, kRateCap>(t, static_cast<int>(s));
    return s;
  }

  struct SymbolSession {
    BitEncoder& coder;
    template <int kN, int kRateCap>
    ARIXV_FORCE_INLINE uint32_t First(SymbolCdf& t, uint32_t s) {
      return coder.template CodeSymbol<kN, kRateCap>(t, s);
    }
    template <int kN, int kRateCap>
    ARIXV_FORCE_INLINE uint32_t Next(SymbolCdf& t, uint32_t s) {
      return coder.template CodeSymbol<kN, kRateCap>(t, s);
    }
  };
  ARIXV_FORCE_INLINE SymbolSession BeginSymbols() { return {*this}; }

  // Stats instrumentation (ARIXV_ENABLE_STATS + ARIXV_STATS=1): while set,
  // the entropy cost of every decision is accumulated into *sink. No-op when
  // stats are compiled out or on decoders so shared model code calls it
  // unconditionally.
  void SetCostSink(double* sink) {
#ifdef ARIXV_ENABLE_STATS
    cost_sink_ = sink;
#else
    (void)sink;
#endif
  }

  // Range-coder bytes, then the rANS words (see symbol_coder.h).
  void Flush() {
    enc_.Flush();
    rans_.Flush(enc_.out());
  }
  bool exhausted() const { return false; }

 private:
  RangeEncoder& enc_;
  RansEncoder rans_;
#ifdef ARIXV_ENABLE_STATS
  double* cost_sink_ = nullptr;
#endif
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

  struct SymbolSession {
    RansDecoder& rans;
    uint32_t x = 0;
    uint32_t last_f = 0;
    ARIXV_FORCE_INLINE explicit SymbolSession(RansDecoder& r) : rans(r) {}
    ARIXV_FORCE_INLINE ~SymbolSession() { rans.Commit(x, last_f); }

    template <int kN, int kRateCap>
    ARIXV_FORCE_INLINE uint32_t First(SymbolCdf& t, uint32_t /*s*/) {
      return static_cast<uint32_t>(
          rans.template DecodeFirst<kN, kRateCap>(t, x, last_f));
    }
    template <int kN, int kRateCap>
    ARIXV_FORCE_INLINE uint32_t Next(SymbolCdf& t, uint32_t /*s*/) {
      return static_cast<uint32_t>(
          rans.template DecodeNext<kN, kRateCap>(t, x, last_f));
    }
  };
  ARIXV_FORCE_INLINE SymbolSession BeginSymbols() {
    return SymbolSession(rans_);
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
      auto sym = coder.BeginSymbols();
      const uint32_t out = CodeFrom<0, Coder::kEncoding>(sym, value, 0, 0);
      return Coder::kEncoding ? value : out;
    }
  }

 private:
  template <int kPrefix>
  static constexpr int Chunk() { return kPrefix == 0 ? kFirstChunk : 4; }

  template <int kPrefix, bool kEncoding, typename Session>
  ARIXV_FORCE_INLINE uint32_t CodeFrom(Session& sym, uint32_t value,
                                       uint32_t prefix_value, uint32_t base) {
    constexpr int kChunk = Chunk<kPrefix>();
    constexpr int kShift = kBits - kPrefix - kChunk;
    const uint32_t want =
        kEncoding ? (value >> kShift) & ((1u << kChunk) - 1u) : 0u;
    const uint32_t s =
        kPrefix == 0
            ? sym.template First<1 << kChunk, kRateCap>(
                  tables[base + prefix_value], want)
            : sym.template Next<1 << kChunk, kRateCap>(
                  tables[base + prefix_value], want);
    const uint32_t next = (prefix_value << kChunk) | s;
    if constexpr (kPrefix + kChunk < kBits) {
      return CodeFrom<kPrefix + kChunk, kEncoding>(sym, value, next,
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

// Compile-time equivalent of SymbolCdfSetPrior (zero runtime construction).
template <int kN>
constexpr SymbolCdf MakeSymbolCdfPrior(std::array<uint32_t, kN> w,
                                       uint16_t count = 0) {
  using K = SymbolCdfConst<kN>;
  SymbolCdf t{};
  uint64_t total = 0;
  for (int i = 0; i < kN; ++i) {
    total += w[i];
  }
  if (total == 0) {
    t.d[0] = count;
    return t;
  }
  uint32_t freq[kN]{};
  const uint32_t spread = kSymbolScale - kN;
  uint32_t sum = 0;
  int largest = 0;
  for (int i = 0; i < kN; ++i) {
    freq[i] = 1 + static_cast<uint32_t>(
                      static_cast<uint64_t>(w[i]) * spread / total);
    sum += freq[i];
    if (freq[i] > freq[largest]) {
      largest = i;
    }
  }
  freq[largest] += kSymbolScale - sum;
  uint32_t c = 0;
  for (int i = 1; i < 16; ++i) {
    if (i <= kN) {
      c += freq[i - 1];
    }
    const uint32_t cdf = i < kN ? c : K::kUniform[i];
    t.d[i] = static_cast<uint16_t>(cdf - K::kUniform[i]);
  }
  t.d[0] = count;
  return t;
}

// Pure-math cold-start prior for a 6-bit uint length tree over valid bit
// widths [min_width, max_width] (0..32 for a general uint32_t).
constexpr BitTree<6> MakeUintLengthTreePrior(uint32_t min_width,
                                             uint32_t max_width) {
  BitTree<6> tree{};
  std::array<uint32_t, 4> top_w{};
  for (uint32_t top = 0; top < 4; ++top) {
    std::array<uint32_t, 16> leaf_w{};
    uint32_t valid = 0;
    for (uint32_t lo = 0; lo < 16; ++lo) {
      const uint32_t width = (top << 4) | lo;
      if (width >= min_width && width <= max_width) {
        leaf_w[lo] = 1;
        ++valid;
      }
    }
    top_w[top] = valid;
    if (valid > 0 && valid < 16) {
      tree.tables[1 + top] = MakeSymbolCdfPrior<16>(leaf_w);
    }
  }
  tree.tables[0] = MakeSymbolCdfPrior<4>(top_w);
  return tree;
}

inline constexpr SymbolCdf kUint32LengthRootPrior =
    MakeSymbolCdfPrior<4>({16, 16, 1, 0});
inline constexpr SymbolCdf kUint32LengthLeaf32Prior =
    MakeSymbolCdfPrior<16>({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});

struct UintModel {
  UintModel() {
    if constexpr (kUintLengthBits == 6) {
      length.tables[0] = kUint32LengthRootPrior;
      length.tables[3] = kUint32LengthLeaf32Prior;
    }
  }

  BitTree<kUintLengthBits> length{};
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
  return v == 0 ? 0u : static_cast<uint32_t>(32 - Clz32(v));
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
  if (nbits <= 1) {
    return nbits;
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

  // Cold path (once per leaf), kept out of the coding loop.
  template <typename LeafPrior>
  ARIXV_NOINLINE static void MakeLeaf(Arena& arena, ArenaPtr<SymbolCdf>& slot,
                                      uint32_t high,
                                      const LeafPrior& init_leaf) {
    slot.Make(arena);
    init_leaf(high, *slot);
  }

  struct NoLeafPrior {
    void operator()(uint32_t, SymbolCdf&) const {}
  };

  // init_leaf(high, leaf) seeds a leaf table when it is first created.
  template <typename Coder, typename LeafPrior = NoLeafPrior>
  uint32_t CodeValue(Coder& coder, Arena& arena, uint32_t value,
                     const LeafPrior& init_leaf = LeafPrior{}) {
    if constexpr (Coder::kRawResidual) {
      (void)arena;
      (void)init_leaf;
      return coder.CodeBitsDirect(value, kByteBits);
    } else {
#ifdef ARIXV_TRACE_DECISIONS
      if (Coder::kEncoding && g_tree_trace != nullptr) {
        g_tree_trace->push_back({this, kByteBits, value});
      }
#endif
      StatSiteScope<Coder> site(kSiteBitTree);
      auto sym = coder.BeginSymbols();
      const uint32_t high = sym.template First<16, kRateCap>(
          root, (value >> 4) & 15u);
      ArenaPtr<SymbolCdf>& low_table = leaf[high];
      if (!low_table) {
        MakeLeaf(arena, low_table, high, init_leaf);
      }
      const uint32_t low = sym.template Next<16, kRateCap>(
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

}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_PROB_H_
