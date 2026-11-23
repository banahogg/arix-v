// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// ARXP block codec (docs/format-arxp.md §3). Included by src/model.cpp inside
// namespace arixv::v1, after CodeModule: it drives the frozen ARX2 model
// through CodeModule's hook interface and adds nothing to the ARX2 path.
//
// One coder stream per block. Per member:
//   duplicate flag [+ distance to an identical earlier member], else a
//   structural flag (model-eligible) or the byte-model fallback, else the
//   module coded by the unchanged per-instruction model step, where before
//   every instruction (once a copy source exists, non-swapped targets only)
//   a flag says whether a copy of an instruction run from an earlier member
//   starts here. A copy codes rep-or-(distance, position), a length, and for
//   every ID word of the copied run a restoration choice (correspondence map
//   hit, base + running delta, or an explicit zigzag delta). Copied
//   instructions are then trained through the model: the encoder path runs
//   on the known words with a coder that only updates probabilities, so the
//   model state after a copy equals the state after coding it.
//
// The encoder follows a plan (an encoder-side choice); the decoder is
// self-contained and validates every reference.

namespace {

// ---------------------------------------------------------------------------
// Warm statistics (WARM): a structural member starts from a fresh model, then
// the allowlisted families are copied by value from the previous structural
// member's final state into destination-owned arena contexts, keyed by their
// grammar-derived table index. No tracker/ID/type/import state, operand
// caches, predictors, ID-slot handles, literal or string state, or arena
// pointers cross the boundary. A context the previous member never created
// stays uncreated, so it is built with its first-use priors exactly as in a
// standalone module.
// ---------------------------------------------------------------------------
namespace pack_transfer {

constexpr uint8_t kFamilyOpcode = 1;  // known_opcode, opcode trees, order-1
constexpr uint8_t kFamilyCount = 2;   // word-count models, optional tails
constexpr uint8_t kFamilyEnum = 4;    // value- and bit-enum operand models
constexpr uint8_t kFamilyAll = 7;

static_assert(sizeof(Prob) == sizeof(uint16_t),
              "pack transfer assumes the frozen packed Prob layout");

// Caps on carried adaptation ages (the packed Prob count, SymbolCdf::d[0],
// and the opcode order-1 promotion count). ARXP carries ages unchanged.
struct AgeCaps {
  uint16_t prob;
  uint16_t cdf;
  uint32_t order1;
};
constexpr AgeCaps kCarryAges = {0xffffu, 0xffffu, 0xffffffffu};

inline uint16_t ProbAge(const Prob& p) {
  return static_cast<uint16_t>(p.state >> kProbBits);
}

// One keyed walk over the allowlisted families. V provides:
//   bool Ctx(Arena&, ArenaPtr<T>& dst, const ArenaPtr<T>& src)
//   bool Ctx(std::unique_ptr<T>& dst, const std::unique_ptr<T>& src)
//   void P(Prob& dst, const Prob& src)       (probability value + age)
//   void C(SymbolCdf& dst, const SymbolCdf& src)  (CDF lanes + d[0] age)
//   void Order1(uint32_t& dst, uint32_t src)  (order-1 promotion count)
//   void Size(std::vector<Prob>& dst, const std::vector<Prob>& src)
template <class V, size_t kN>
void WalkCdfs(V& v, std::array<SymbolCdf, kN>& d,
              const std::array<SymbolCdf, kN>& s) {
  for (size_t i = 0; i < kN; ++i) v.C(d[i], s[i]);
}

template <class V, size_t kN>
void WalkProbs(V& v, std::array<Prob, kN>& d, const std::array<Prob, kN>& s) {
  for (size_t i = 0; i < kN; ++i) v.P(d[i], s[i]);
}

template <class V>
void WalkUint(V& v, Arena& arena, UintModel& d, const UintModel& s) {
  WalkCdfs(v, d.length.tables, s.length.tables);
  WalkProbs(v, d.short_rows, s.short_rows);
  if (v.Ctx(arena, d.long_rows, s.long_rows)) {
    for (size_t r = 0; r < s.long_rows->high.size(); ++r) {
      WalkProbs(v, d.long_rows->high[r], s.long_rows->high[r]);
    }
    WalkProbs(v, d.long_rows->rest, s.long_rows->rest);
  }
}

template <class V>
void WalkUintSlot(V& v, Arena& arena, ArenaPtr<UintModel>& d,
                  const ArenaPtr<UintModel>& s) {
  if (v.Ctx(arena, d, s)) WalkUint(v, arena, *d, *s);
}

template <class V, int kRateCap>
void WalkLazyLeafTree8(V& v, Arena& arena, LazyLeafTree8<kRateCap>& d,
                       const LazyLeafTree8<kRateCap>& s) {
  v.C(d.root, s.root);
  for (size_t i = 0; i < s.leaf.size(); ++i) {
    if (v.Ctx(arena, d.leaf[i], s.leaf[i])) v.C(*d.leaf[i], *s.leaf[i]);
  }
}

template <class V>
void WalkOpcodeTree(V& v, Arena& arena, OpcodeTree& d, const OpcodeTree& s) {
  v.C(d.root, s.root);
  WalkCdfs(v, d.mid, s.mid);
  for (size_t g = 0; g < s.leaf.size(); ++g) {
    if (v.Ctx(arena, d.leaf[g], s.leaf[g])) WalkCdfs(v, *d.leaf[g], *s.leaf[g]);
  }
}

template <class V>
void WalkOpcode(V& v, ModelState& dst, const ModelState& src) {
  Arena& a = dst.arena;
  WalkProbs(v, dst.known_opcode, src.known_opcode);
  for (size_t i = 0; i < src.opcode_index_trees.size(); ++i) {
    if (v.Ctx(a, dst.opcode_index_trees[i], src.opcode_index_trees[i])) {
      WalkOpcodeTree(v, a, *dst.opcode_index_trees[i],
                     *src.opcode_index_trees[i]);
    }
  }
  for (size_t i = 0; i < src.opcode_contexts.size(); ++i) {
    if (v.Ctx(a, dst.opcode_contexts[i], src.opcode_contexts[i])) {
      v.Order1(dst.opcode_contexts[i]->count, src.opcode_contexts[i]->count);
      WalkOpcodeTree(v, a, dst.opcode_contexts[i]->tree,
                     src.opcode_contexts[i]->tree);
    }
  }
  if (v.Ctx(a, dst.unknown_opcode_tree, src.unknown_opcode_tree)) {
    WalkCdfs(v, dst.unknown_opcode_tree->tables,
             src.unknown_opcode_tree->tables);
  }
}

template <class V>
void WalkCount(V& v, ModelState& dst, const ModelState& src) {
  Arena& a = dst.arena;
  v.P(dst.word_count_consistent, src.word_count_consistent);
  for (size_t i = 0; i < src.class_count_models.size(); ++i) {
    if (v.Ctx(a, dst.class_count_models[i], src.class_count_models[i])) {
      v.P(dst.class_count_models[i]->same_as_last,
          src.class_count_models[i]->same_as_last);
      WalkUint(v, a, dst.class_count_models[i]->extra,
               src.class_count_models[i]->extra);
    }
  }
  WalkUintSlot(v, a, dst.raw_known_count, src.raw_known_count);
  WalkUintSlot(v, a, dst.raw_unknown_count, src.raw_unknown_count);
  // Optional-tail presence lives in the per-instruction context next to
  // module-local caches. The destination context is created value-initialized
  // (as on first use) and only has_optional is carried: last_word_count,
  // ID/literal caches, ID-slot handles, and result-type choice stay fresh.
  for (size_t i = 0; i < src.inst_contexts.size(); ++i) {
    if (v.Ctx(a, dst.inst_contexts[i], src.inst_contexts[i])) {
      v.P(dst.inst_contexts[i]->group.has_optional,
          src.inst_contexts[i]->group.has_optional);
    }
  }
}

template <class V>
void WalkEnum(V& v, ModelState& dst, const ModelState& src) {
  Arena& a = dst.arena;
  for (size_t i = 0; i < src.value_enums.size(); ++i) {
    if (v.Ctx(a, dst.value_enums[i], src.value_enums[i])) {
      ValueEnumModel& d = *dst.value_enums[i];
      const ValueEnumModel& s = *src.value_enums[i];
      v.P(d.known, s.known);
      WalkLazyLeafTree8(v, a, d.index, s.index);
      WalkUint(v, a, d.escape, s.escape);
    }
  }
  for (size_t i = 0; i < src.bit_enums.size(); ++i) {
    if (v.Ctx(dst.bit_enums[i], src.bit_enums[i])) {
      BitEnumModel& d = *dst.bit_enums[i];
      const BitEnumModel& s = *src.bit_enums[i];
      v.Size(d.bits, s.bits);
      for (size_t b = 0; b < s.bits.size() && b < d.bits.size(); ++b) {
        v.P(d.bits[b], s.bits[b]);
      }
      v.P(d.unknown, s.unknown);
      WalkUint(v, a, d.residual, s.residual);
    }
  }
}

template <class V>
void Walk(V& v, ModelState& dst, const ModelState& src, uint8_t families) {
  if (families & kFamilyOpcode) WalkOpcode(v, dst, src);
  if (families & kFamilyCount) WalkCount(v, dst, src);
  if (families & kFamilyEnum) WalkEnum(v, dst, src);
}

// Copies values, applies the age caps, and creates destination-owned
// contexts for (only) the contexts the source created.
struct CopyVisitor {
  AgeCaps c;
  template <typename T>
  bool Ctx(Arena& arena, ArenaPtr<T>& d, const ArenaPtr<T>& s) {
    if (!s) return false;
    d.Make(arena);
    return true;
  }
  template <typename T>
  bool Ctx(std::unique_ptr<T>& d, const std::unique_ptr<T>& s) {
    if (!s) return false;
    d = std::make_unique<T>();
    return true;
  }
  void P(Prob& d, const Prob& s) {
    const uint16_t age = ProbAge(s);
    const uint16_t kept = age < c.prob ? age : c.prob;
    d.state = static_cast<uint16_t>((kept << kProbBits) |
                                    (s.state & Prob::kValueMask));
  }
  void C(SymbolCdf& d, const SymbolCdf& s) {
    d = s;  // lanes 1..15 are the distribution values
    d.d[0] = s.d[0] < c.cdf ? s.d[0] : c.cdf;  // lane 0 is the age
  }
  void Order1(uint32_t& d, uint32_t s) { d = s < c.order1 ? s : c.order1; }
  void Size(std::vector<Prob>& d, const std::vector<Prob>& s) {
    d.resize(s.size());
  }
};

// `dst` must be freshly constructed (no context created yet).
inline void Transfer(ModelState& dst, const ModelState& src, uint8_t families,
                     AgeCaps caps) {
  CopyVisitor v{caps};
  Walk(v, dst, src, families);
}

// The model for the next structural member of a block.
inline std::unique_ptr<ModelState> NextModel(
    const std::unique_ptr<ModelState>& prev, bool warm) {
  auto model = std::make_unique<ModelState>();
  if (prev && warm) Transfer(*model, *prev, kFamilyAll, kCarryAges);
  return model;
}

}  // namespace pack_transfer

namespace pack_codec {

using pack::Copy;
using pack::SharedMember;

// Probability-update-only coder with encoder semantics: identical model
// updates to BitEncoder, no output. Used for adapt-mode train-through of
// copied instructions; FAST_DECODE uses the dedicated TrackInstruction below
// so no second CodeInstruction / model-helper template tree is instantiated.
class TrainBits {
 public:
  static constexpr bool kEncoding = true;
  static constexpr bool kRawResidual = false;
  bool CodeBit(Prob& p, bool bit) {
    p.UpdateFromRaw(p.raw(), bit ? 1 : 0);
    return bit;
  }
  uint32_t CodeBitsDirect(uint32_t value, int) { return value; }
  template <int kN, int kRateCap>
  uint32_t CodeSymbol(SymbolCdf& t, uint32_t s) {
    SymbolCdfUpdate<kN, kRateCap>(t, static_cast<int>(s));
    return s;
  }
  struct SymbolSession {
    TrainBits& coder;
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
  void SetCostSink(double*) {}
  bool exhausted() const { return false; }
};

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
// Encoder cost arithmetic is integer fixed point (Q16: 1/65536 bit), so
// planning decisions, and therefore pack bytes, are identical on every
// compiler and host (no floating point, no libm).
using CostQ16 = int64_t;
constexpr int kCostShift = 16;

// floor(log2(x) * 2^16) for x >= 1 (x = 0 is treated as 1): normalise to a
// Q31 mantissa in [1, 2), then 16 rounds of squaring, each yielding one
// fractional bit. Exact integer arithmetic.
inline CostQ16 Log2Q16(uint64_t x) {
  if (x == 0) x = 1;
  int ip = 0;
  while ((x >> ip) > 1) ++ip;
  uint64_t m = ip >= 31 ? x >> (ip - 31) : x << (31 - ip);  // [2^31, 2^32)
  CostQ16 r = static_cast<CostQ16>(ip) << kCostShift;
  for (int b = kCostShift - 1; b >= 0; --b) {
    m = (m * m) >> 31;  // m < 2^32, so m * m < 2^64; result in [2^31, 2^33)
    if (m >= (uint64_t{1} << 32)) {
      m >>= 1;
      r |= CostQ16{1} << b;
    }
  }
  return r;
}

// TrainBits that also accumulates the entropy cost of every decision (Q16
// bits, as -log2 of the coded probability): the in-context literal cost the
// planner weighs copies against.
class CostBits {
 public:
  static constexpr bool kEncoding = true;
  static constexpr bool kRawResidual = false;
  CostQ16 total = 0;
  bool CodeBit(Prob& p, bool bit) {
    const uint32_t v = p.value();  // probability of a 0 bit, of kProbScale
    const uint32_t c = bit ? kProbScale - v : v;
    total += Log2Q16(kProbScale) - Log2Q16(c);
    p.UpdateFromRaw(p.raw(), bit ? 1 : 0);
    return bit;
  }
  uint32_t CodeBitsDirect(uint32_t value, int nbits) {
    total += static_cast<CostQ16>(nbits) << kCostShift;
    return value;
  }
  template <int kN, int kRateCap>
  uint32_t CodeSymbol(SymbolCdf& t, uint32_t s) {
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
    total += Log2Q16(kSymbolScale) - Log2Q16(hi - lo);
    SymbolCdfUpdate<kN, kRateCap>(t, static_cast<int>(s));
    return s;
  }
  struct SymbolSession {
    CostBits& coder;
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
  void SetCostSink(double*) {}
  bool exhausted() const { return false; }
};
#endif

// Encoder-style IO over native-order module words already in memory.
class TrainIO {
 public:
  explicit TrainIO(const uint32_t* words) : w_(words) {}
  bool SwappedHint() const { return false; }
  uint32_t HeaderWord(size_t i) const { return w_[i]; }
  uint32_t BodyWord(size_t i) const { return w_[5 + i]; }
  const uint32_t* BodyWordPtr(size_t i) const { return w_ + 5 + i; }
  uint32_t BoundHint() const { return w_[3]; }
  void EmitHeaderWord(uint32_t) {}
  void EmitBodyWord(uint32_t) {}
  void PatchBound(uint32_t) {}
  void PatchBodyWord(size_t, uint32_t) {}

 private:
  const uint32_t* w_;
};

inline uint32_t BitWidth(uint32_t v) { return v == 0 ? 0 : 32 - Clz32(v); }

// Adaptive Elias-gamma for v >= 1: unary bit length, then the first two
// mantissa bits adaptive per length, the rest direct.
struct GammaModel {
  Prob len[33];
  Prob mant[33][2];
};

template <class Bits>
uint32_t CodeGamma(Bits& bits, GammaModel& g, uint32_t v) {
  const uint32_t n = Bits::kEncoding ? BitWidth(v) : 0;
  uint32_t len = 1;
  while (len < 32 && bits.CodeBit(g.len[len], Bits::kEncoding && n > len)) {
    ++len;
  }
  uint32_t out = 1;
  for (int j = static_cast<int>(len) - 2; j >= 0; --j) {
    const int depth = static_cast<int>(len) - 2 - j;
    const bool want = Bits::kEncoding && ((v >> j) & 1u) != 0;
    const uint32_t bit =
        depth < 2 ? (bits.CodeBit(g.mant[len][depth], want) ? 1u : 0u)
                  : bits.CodeBitsDirect(want ? 1u : 0u, 1);
    out = (out << 1) | bit;
  }
  return Bits::kEncoding ? v : out;
}

inline uint32_t ZigZag(uint32_t d) {
  const int32_t s = static_cast<int32_t>(d);
  return (static_cast<uint32_t>(s) << 1) ^ static_cast<uint32_t>(s >> 31);
}
inline uint32_t UnZigZag(uint32_t z) { return (z >> 1) ^ (0u - (z & 1u)); }

struct SignalModels {
  Prob dup;
  Prob structural;
  Prob start[2];
  Prob rep;
  Prob map_hit[3];
  Prob delta_hit[3][2];
  GammaModel dup_dist, base_dist, len_rep, len_new, explicit_id[3];
};

struct Frame {
  uint32_t begin;   // body word index
  uint32_t nwords;
};

// A member usable as a copy base: structurally coded and not byte-swapped.
// Frames and ID roles are the ones the model itself used while coding it.
// Non-owning view: storage belongs to the block (own members) or to the
// shared members (read in place, never copied per block).
struct Base {
  bool candidate = false;
  bool valid = false;
  const uint32_t* words = nullptr;  // whole module, native order
  size_t nwords = 0;
  // Frame i spans [starts[i], starts[i + 1]) (body words); nframes + 1
  // entries.
  const uint32_t* starts = nullptr;
  uint32_t nframes = 0;
  const uint8_t* mask = nullptr;    // per body word: 0 literal, 1 def, 2 use
};

inline void MakeBase(const uint32_t* words, size_t nwords, bool swapped,
                     const uint32_t* starts, size_t nframes,
                     const uint8_t* mask, Base* b) {
  b->candidate = !swapped && nframes > 0;  // a frame-less base has no runs
  b->valid = b->candidate;
  b->words = words;
  b->nwords = nwords;
  b->starts = starts;
  b->nframes = static_cast<uint32_t>(nframes);
  b->mask = mask;
}

// Start offsets of contiguous frames (nframes + 1 entries), or empty when
// the frames do not tile from 0 (the member is then not a copy source).
inline std::vector<uint32_t> StartsOf(const std::vector<Frame>& f) {
  std::vector<uint32_t> s;
  if (f.empty() || f[0].begin != 0) return s;
  s.reserve(f.size() + 1);
  for (size_t i = 0; i < f.size(); ++i) {
    if (i && f[i].begin != f[i - 1].begin + f[i - 1].nwords) return {};
    s.push_back(f[i].begin);
  }
  s.push_back(f.back().begin + f.back().nwords);
  return s;
}

inline void SharedBase(const SharedMember& m, Base* b) {
  if (!m.valid) return;
  MakeBase(m.words, m.nwords, false, m.starts, m.nframes, m.mask, b);
}

// Base-ID -> target-ID correspondences, one table per base member, private
// to the block: open addressing, stamped with the target member so it is
// reused across targets without clearing. A table starts small and grows
// only with the entries a target inserts (at most one per copied ID word),
// so total capacity is bounded by the block's own targets, not by the size
// of the (possibly shared) bases they copy from.
class IdTable {
 public:
  bool Find(uint32_t b, uint32_t stamp, uint32_t* t, size_t* at) {
    if (e_.empty()) Grow(stamp);
    *at = Probe(b, stamp);
    const Entry& e = e_[*at];
    *t = e.val;
    return e.stamp == stamp;
  }
  // `at` from the immediately preceding Find with the same key and stamp.
  void Set(size_t at, uint32_t b, uint32_t stamp, uint32_t t) {
    Entry& e = e_[at];
    if (e.stamp != stamp) {
      if (stamp != live_stamp_) {
        live_stamp_ = stamp;
        live_ = 0;
      }
      ++live_;
    }
    e = Entry{b, stamp, t};
    if (2 * live_ > e_.size()) Grow(stamp);
  }

 private:
  struct Entry {
    uint32_t key, stamp, val;
  };
  size_t Probe(uint32_t b, uint32_t stamp) const {
    const size_t mask = e_.size() - 1;
    size_t i = static_cast<size_t>((b * 2654435761u) >> shift_);
    while (e_[i].stamp == stamp && e_[i].key != b) i = (i + 1) & mask;
    return i;
  }
  void Grow(uint32_t stamp) {
    std::vector<Entry> old;
    old.swap(e_);
    const size_t cap = old.empty() ? 8 : old.size() * 2;
    unsigned bits = 0;
    while ((size_t{1} << bits) < cap) ++bits;
    e_.assign(cap, Entry{0, 0, 0});
    shift_ = 32 - bits;
    live_ = 0;
    live_stamp_ = stamp;
    for (const Entry& o : old) {
      if (o.stamp != stamp) continue;  // other targets' entries are dead
      e_[Probe(o.key, stamp)] = o;
      ++live_;
    }
  }
  std::vector<Entry> e_;
  unsigned shift_ = 32;
  size_t live_ = 0;
  uint32_t live_stamp_ = 0;
};

// Per (target, base) pair: the running ID delta. One table per block,
// indexed by base and stamped with the target (as IdTable), so a target
// copying from a few bases costs O(1) setup, not O(members before it).
struct CopyMap {
  uint32_t stamp = 0;  // target + 1 that owns `delta`; else delta is 0
  uint32_t delta = 0;
};

// FAST_DECODE (no-adapt) string state update: mirrors CodeString with a
// no-op coder, updating recent_string_pool, StringModel history/order0, and
// returning the consumed word span without instantiating CodeString.
ARIXV_NOINLINE uint32_t TrackString(ModelState& model,
                                    const uint32_t* hint_words,
                                    uint32_t remaining) {
  uint32_t span = 0;
  bool found_nul = false;
  while (span < remaining && !found_nul) {
    const uint32_t word = hint_words[span++];
    if (WordHasNulByte(word)) {
      found_nul = true;
    }
  }
  if (!found_nul) span = remaining;
  std::string raw = RawStringBytes(hint_words, span);
  const uint32_t out_span = static_cast<uint32_t>((raw.size() + 3u) / 4u);
  const int rank = FindRecentString(model, raw);
  if (rank >= 0 && rank < 64) {
    PushStringHistory(*model.strings, raw);
    PromoteRecentString(model, static_cast<uint32_t>(rank));
    return out_span == 0 ? 1u : out_span;
  }
  StringModel& sm = *model.strings;
  const bool canonical = IsCanonicalStringBytes(raw);
  const int dict = canonical ? FindGrammarString(raw) : -1;
  if (dict >= 0 || !canonical) {
    PushStringHistory(sm, raw);
  } else {
    uint8_t prev = 0;
    size_t match = 0;
    if (remaining > 0) (void)sm.Table();
    const size_t limit = static_cast<size_t>(remaining) * 4u;
    for (size_t i = 0; i < limit; ++i) {
      const uint8_t b = static_cast<uint8_t>(raw[i]);
      if (match == 0 && i > 0) match = sm.Lookup();
      if (match != 0 && sm.At(match) == b) {
        ++match;
      } else {
        match = 0;
        Arena& arena = sm.bytes.arena;
        ArenaPtr<ByteTree>& tree = sm.bytes.tree[prev];
        if (!tree) {
          tree.Make(arena);
          tree->root = sm.order0.root;
        }
        const uint32_t high = (b >> 4) & 15u;
        ArenaPtr<SymbolCdf>& leaf0 = sm.order0.leaf[high];
        SymbolCdf& order0_leaf =
            leaf0 ? *leaf0 : MakeStringOrder0Leaf(arena, leaf0, high);
        ArenaPtr<SymbolCdf>& leaf = tree->leaf[high];
        if (!leaf) {
          leaf.Make(arena);
          *leaf = order0_leaf;
        }
        SymbolCdfUpdate<16, kByteSymbolRateCap>(sm.order0.root,
                                                static_cast<int>(high));
        SymbolCdfUpdate<16, kByteSymbolRateCap>(order0_leaf,
                                                static_cast<int>(b & 15u));
      }
      sm.Push(b);
      prev = b;
      if (b == 0) break;
    }
  }
  TouchString(model, std::move(raw));
  return out_span == 0 ? 1u : out_span;
}

// FAST_DECODE (no-adapt) copied-instruction replay: advances tracker, ID/
// literal/string/order-1-opcode state and records ID roles identically to
// CodeInstruction with a no-op coder, while skipping all probability updates
// and dead encoder searches (BuildDeferredResultTypeCandidates, FindEnumerant,
// CodeUint, etc.) and avoiding a second CodeInstruction template tree.
ARIXV_NOINLINE void TrackInstruction(ModelState& model, const uint32_t* target,
                                     size_t body_words, size_t& body_pos,
                                     InstructionCarry& c, uint8_t* roles) {
  const uint32_t* const body = target + 5;
  const uint32_t word0 = body[body_pos];
  const uint16_t opcode = static_cast<uint16_t>(word0 & 0xffffu);
  const uint32_t count_hint = (word0 >> 16) & 0xffffu;
  const InstructionDesc* desc = FindInstruction(opcode);
  uint16_t inst_index = NumCoreInstructions();
  if (desc != nullptr) {
    const int found = FindInstructionIndex(opcode);
    assert(found >= 0);
    inst_index = static_cast<uint16_t>(found);
    const uint16_t* prior =
        GetOpcodeSectionPrior(static_cast<uint8_t>(model.tracker.section()));
    const uint16_t prev_key = PreviousOpcodeKey(model.tracker);
    ArenaPtr<OpcodeCtx>& order1_slot = model.opcode_contexts[prev_key];
    if (!order1_slot) {
      order1_slot.Make(model.arena);
      order1_slot->tree.SetPrior(prior);
    }
    if (order1_slot->count < kOpcodeOrder1MinCount) {
      order1_slot->tree.Train(model.arena, inst_index, prior);
      ++order1_slot->count;
    }
    model.EnsureInstContext(inst_index);
  }

  const size_t remaining_body = body_words - body_pos;
  bool opaque = true;
  bool deferred_word_count = false;
  bool fixed_layout = false;
  uint32_t declared_count = count_hint;
  if (desc != nullptr) {
    const bool use_grammar_implied =
        HasInstructionFlag(desc, kInstGrammarImpliedWordCount);
    const bool use_ext_inst = HasInstructionFlag(desc, kInstExtInst);
    const bool use_optional_tail = HasInstructionFlag(desc, kInstOptionalTail);
    const bool single_optional =
        use_optional_tail && desc->min_word_count == desc->num_operands;
    bool consistent = false;
    if (desc->fixed_word_count) {
      consistent = (count_hint == desc->min_word_count);
    } else if (use_grammar_implied) {
      consistent = ValidateGrammarImplied(desc, body + body_pos, count_hint,
                                          remaining_body, model.tracker);
    } else if (use_ext_inst) {
      consistent = ValidateExtInstConsistent(desc, body + body_pos, count_hint,
                                             remaining_body, model.tracker);
    } else if (use_optional_tail) {
      if (count_hint < desc->min_word_count) {
        consistent = false;
      } else if (count_hint == desc->min_word_count) {
        consistent = true;
      } else if (single_optional) {
        const OperandSlotDesc& tail =
            GetOperandSlots(*desc)[desc->num_operands - 1];
        consistent = IsFixedOneWordKind(tail.kind)
                         ? (count_hint == desc->min_word_count + 1u)
                         : ValidateGrammarImplied(desc, body + body_pos,
                                                  count_hint, remaining_body,
                                                  model.tracker);
      } else {
        consistent = true;
      }
    } else {
      consistent = (count_hint >= desc->min_word_count);
    }
    if (consistent) {
      opaque = false;
      if (desc->fixed_word_count) {
        fixed_layout = true;
        declared_count = desc->min_word_count;
      } else if (use_grammar_implied || use_ext_inst) {
        deferred_word_count = true;
        declared_count = 0;
      } else if (use_optional_tail) {
        if (count_hint == desc->min_word_count) {
          fixed_layout = true;
          declared_count = desc->min_word_count;
        } else if (single_optional) {
          const OperandSlotDesc& tail =
              GetOperandSlots(*desc)[desc->num_operands - 1];
          if (IsFixedOneWordKind(tail.kind)) {
            fixed_layout = true;
            declared_count = desc->min_word_count + 1u;
          } else {
            deferred_word_count = true;
            declared_count = 0;
          }
        } else {
          model.inst_group(inst_index).last_word_count = count_hint;
        }
      } else {
        model.inst_group(inst_index).last_word_count = count_hint;
      }
    }
  }

  const bool patch_word0 = deferred_word_count;
  c.inst_words.clear();
  uint32_t operand_words =
      deferred_word_count
          ? static_cast<uint32_t>(std::min<size_t>(
                remaining_body > 0 ? remaining_body - 1 : 0, 0xfffeu))
          : (declared_count == 0
                 ? 0
                 : static_cast<uint32_t>(std::min<size_t>(
                       declared_count - 1,
                       remaining_body > 0 ? remaining_body - 1 : 0)));
  c.inst_words.push_back(word0);

  model.tracker.OnInstructionStart(opcode, desc, declared_count);
  model.cur_prev_res_id = model.tracker.in_block() ? c.prev_inst_result_id : 0u;
  model.cur_prev_res_list = c.prev_inst_result_list;
  c.prev_inst_result_id = 0;
  c.prev_inst_result_list = ModelState::kUnresolvedList;

  const OperandSlotDesc* fixed_slots =
      fixed_layout ? GetOperandSlots(*desc) : nullptr;
  if (!fixed_layout) {
    c.cursor.Begin(desc,
                   deferred_word_count ? operand_words + 1u : declared_count,
                   opaque);
  }

  uint32_t consumed = 0;
  uint32_t current_result_type = 0;
  const ExtInstSetDesc* current_ext_set = nullptr;
  bool has_last_enum = false;
  uint32_t last_enum = 0;
  const bool defer_result_type =
      desc != nullptr && !opaque && desc->has_result_type &&
      !HasInstructionFlag(desc, kInstConstantNumber) &&
      !HasInstructionFlag(desc, kInstVariable);
  bool result_type_deferred = false;
  uint32_t result_type_hint = 0;
  uint32_t first_id_operand = 0;
  uint32_t op0_type = 0;
  int num_id_operands = 0;

  while (consumed < operand_words) {
    const uint32_t remaining = operand_words - consumed;
    OperandCursor::Field cursor_field;
    OperandCursor::Field field;
    if (fixed_layout) {
      const OperandSlotDesc& slot = fixed_slots[consumed];
      const OperandKindDesc& kind = GetOperandKind(slot.kind);
      field = {OperandCursor::FieldType::kOperand, slot.kind, kind.category,
               static_cast<uint8_t>(consumed < 15 ? consumed : 15), false,
               false, remaining};
    } else {
      cursor_field = c.cursor.Next();
      field = cursor_field;
      if (field.type == OperandCursor::FieldType::kEnd ||
          (deferred_word_count &&
           field.type == OperandCursor::FieldType::kOpaqueWord)) {
        if (deferred_word_count) break;
        field.type = OperandCursor::FieldType::kOpaqueWord;
      }
    }

    const uint32_t hint0 = body[body_pos + 1 + consumed];
    uint32_t span = 1;

    if (field.type != OperandCursor::FieldType::kOpaqueWord) {
      switch (field.category) {
        case OperandCategory::kIdResult:
          roles[body_pos + 1 + consumed] = 1;
          model.tracker.OnResultId(hint0);
          model.prev_result_id = hint0;
          c.prev_inst_result_id = hint0;
          break;
        case OperandCategory::kIdResultType:
          roles[body_pos + 1 + consumed] = 2;
          if (defer_result_type && !result_type_deferred) {
            result_type_deferred = true;
            result_type_hint = hint0;
          } else {
            model.tracker.OnResultTypeUse(hint0);
            current_result_type = hint0;
          }
          break;
        case OperandCategory::kIdRef:
        case OperandCategory::kIdScope:
        case OperandCategory::kIdMemorySemantics: {
          roles[body_pos + 1 + consumed] = 2;
          InstGroup& group = model.inst_group(inst_index);
          const size_t key = Slot16(field.slot);
          const uint8_t state = group.last_id_state[key];
          const bool have_last = (state & 1u) != 0;
          const uint32_t own_last = have_last ? group.last_id_list[key] : 0u;
          const uint32_t prev_op_type = model.last_id_operand_type;
          uint32_t chosen_list = own_last;
          if (num_id_operands == 0) {
            if (!have_last) {
              chosen_list = PrevResList(model);
            } else if ((state & 0x20u) != 0u) {
              const uint32_t prl = PrevResList(model);
              if (prl != 0u) chosen_list = prl;
            }
          } else if (!have_last) {
            chosen_list =
                InitialIdListForSlot(model, num_id_operands, op0_type);
          } else {
            switch ((state >> 1) & 7u) {
              case 0: chosen_list = own_last; break;
              case 1: chosen_list = model.cur_op0_list; break;
              case 2: chosen_list = model.cur_prev_op_list; break;
              case 3:
                chosen_list = ElemValuesList(model.tracker, prev_op_type);
                break;
              case 4: chosen_list = PrevResList(model); break;
              default: chosen_list = own_last; break;
            }
            if (chosen_list == 0u) chosen_list = own_last;
          }
          const RecencyList* typed = model.tracker.TypedList(chosen_list);
          const bool coded =
              typed != nullptr && typed->size() > 0 && typed->Find(hint0) >= 0;
          group.last_id[key] = hint0;
          const Tracker::IdUse use =
              model.tracker.OnIdUse(hint0, /*touch_recent_ids=*/!coded);
          model.last_id_operand_type = use.result_type;
          uint8_t next_state = static_cast<uint8_t>(state | 1u);
          if (num_id_operands == 0) {
            if (!have_last) {
              next_state = static_cast<uint8_t>((state & ~0x30u) | 0x11u);
            } else if (use.type_list != 0u && model.cur_prev_res_id != 0u &&
                       (use.type_list != chosen_list ||
                        ((state >> 4) & 3u) == 1u)) {
              next_state =
                  UpdateOp0ListSource(model, own_last, use.type_list, state);
            }
            model.cur_op0_list = use.type_list;
            first_id_operand = hint0;
            op0_type = use.result_type;
          } else if (use.type_list != 0u &&
                     (!have_last || use.type_list != chosen_list)) {
            next_state = UpdateGt0ListSource(model, own_last, prev_op_type,
                                             use.type_list, state);
          }
          model.cur_prev_op_list = use.type_list;
          group.last_id_state[key] = next_state;
          group.last_id_list[key] = use.type_list;
          ++num_id_operands;
          if (desc && desc->opcode == 12 && field.slot == 2) {
            current_ext_set = model.tracker.ExtInstSetOf(hint0);
            c.cursor.SetExtInstSet(current_ext_set);
          }
          break;
        }
        case OperandCategory::kValueEnum:
          has_last_enum = true;
          last_enum = hint0;
          break;
        case OperandCategory::kLiteralInteger:
          if (has_last_enum) {
            EnumLiteralCache& cache =
                EnsureContext(model.arena, model.enum_literals);
            const uint64_t cache_key =
                (uint64_t{1} << 63) |
                (static_cast<uint64_t>(inst_index) << 36) |
                (static_cast<uint64_t>(Slot16(field.slot)) << 32) | last_enum;
            const uint32_t mix =
                static_cast<uint32_t>(cache_key ^ (cache_key >> 32)) *
                0x9E3779B1u;
            EnumLiteralCache::Entry& entry =
                cache.entries[mix >> (32 - EnumLiteralCache::kBits)];
            const uint32_t prev = entry.key == cache_key ? entry.last : 0u;
            entry.key = cache_key;
            entry.stride = hint0 - prev;
            entry.last = hint0;
          } else {
            InstGroup& group = model.inst_group(inst_index);
            const size_t key = Slot16(field.slot);
            group.last_literal_valid[key] = 1;
            group.last_literal[key] = hint0;
          }
          break;
        case OperandCategory::kLiteralContextDependentNumber: {
          if (deferred_word_count &&
              HasInstructionFlag(desc, kInstConstantNumber) &&
              model.tracker.TypeWidth(current_result_type) == 0) {
            span = 1;
          } else {
            span = SpanForField(field, desc, current_result_type, model.tracker,
                                c.inst_words.data(),
                                static_cast<uint32_t>(c.inst_words.size()),
                                remaining);
          }
          span = std::max<uint32_t>(1, std::min(span, remaining));
          if (span == 1 && current_result_type != 0) {
            const Tracker::IdInfo* ti = model.tracker.Find(current_result_type);
            if (ti != nullptr && ti->type_class == Tracker::TypeClass::kInt) {
              IntConstCache& cache =
                  EnsureContext(model.arena, model.int_const_cache);
              size_t slot = IntConstCache::kSize;
              for (size_t i = 0; i < IntConstCache::kSize; ++i) {
                if (cache.type_id[i] == current_result_type) {
                  slot = i;
                  break;
                }
              }
              if (slot == IntConstCache::kSize) {
                slot = cache.next_evict++ & (IntConstCache::kSize - 1);
                cache.type_id[slot] = current_result_type;
              }
              cache.last[slot] = hint0;
            }
          }
          break;
        }
        case OperandCategory::kLiteralExtInstInteger:
          if (deferred_word_count && HasInstructionFlag(desc, kInstExtInst)) {
            const InstructionDesc* ext_inst =
                current_ext_set ? FindExtInstruction(*current_ext_set, hint0)
                                : nullptr;
            if (!ext_inst || !ext_inst->fixed_word_count) {
              model.inst_group(inst_index).last_word_count = count_hint;
              uint32_t tgt =
                  count_hint > 0
                      ? static_cast<uint32_t>(std::min<size_t>(
                            count_hint - 1, remaining_body - 1))
                      : 0u;
              if (tgt < consumed + 1) tgt = consumed + 1;
              operand_words = tgt;
              c.cursor.SetRemaining(operand_words - consumed);
              deferred_word_count = false;
            }
          }
          break;
        case OperandCategory::kLiteralString:
          span = TrackString(model, body + body_pos + 1 + consumed, remaining);
          if (span > remaining) span = remaining;
          break;
        case OperandCategory::kBitEnum:
        case OperandCategory::kLiteralSpecConstantOpInteger:
        case OperandCategory::kComposite:
        case OperandCategory::kUnknown:
          break;
      }
    }

    for (uint32_t i = 0; i < span; ++i) {
      c.inst_words.push_back(body[body_pos + 1 + consumed + i]);
    }
    if (!fixed_layout) {
      c.cursor.Consume(cursor_field, span, hint0);
    }
    consumed += span;
  }

  if (patch_word0) {
    const uint32_t final_count =
        static_cast<uint32_t>(c.inst_words.size()) & 0xffffu;
    c.inst_words.data()[0] = (final_count << 16) | opcode;
  }
  if (result_type_deferred && c.inst_words.size() > 1) {
    model.tracker.OnResultTypeUse(result_type_hint);
  }
  c.prev_inst_result_list = model.tracker.OnInstructionEnd(
      c.inst_words.data(), static_cast<uint32_t>(c.inst_words.size()),
      inst_index, num_id_operands > 0 ? first_id_operand : 0);
  body_pos += 1 + consumed;
}

// CodeModule hook (src/model.cpp NoModuleHook interface), shared by encoder
// (Bits::kEncoding) and decoder: consumes copied runs before instructions
// and records frames and ID roles for later copies from this member.
template <class Bits>
struct Hook {
  static constexpr bool kActive = true;
  static constexpr bool kRecordRoles = true;
  void Role(size_t body_word, uint8_t role) { roles[body_word] = role; }
  SignalModels* sig = nullptr;
  std::vector<Base>* bases = nullptr;  // indexed by member; [0, k) usable
  uint32_t k = 0;
  bool signal = false;  // copy signalling enabled (false: plain coding)
  bool copies_allowed = false;
  bool any_candidate = false;
  bool adapt = true;  // train copied instructions' probabilities
  // Target words (native): encoder input, or the decoder's output buffer.
  const uint32_t* target = nullptr;
  // Encoder plan for this member.
  const Copy* plan = nullptr;
  size_t plan_count = 0;
  size_t next = 0;
  // State.
  uint32_t inst_index = 0;
  int prev_copied = 0;
  bool rep_valid = false;
  uint32_t rep_base = 0;
  uint32_t rep_pos = 0;
  std::vector<Frame> frames;
  std::vector<uint8_t> roles;  // per target body word, filled by the model
  std::vector<CopyMap>* maps = nullptr;  // the block's table, per base
  std::vector<IdTable>* ids = nullptr;  // the block's tables, per base
  // Optional per-literal-instruction cost recording (planner cost pass).
  const int64_t* cost_total = nullptr;
  int64_t cost_last = 0;
  std::vector<int64_t> costs;
  std::vector<uint32_t> restored;

  void Begin(bool swapped) {
    copies_allowed = signal && !swapped && any_candidate;
    if (cost_total) cost_last = *cost_total;
  }

  void AfterInstruction(size_t start, size_t end) {
    frames.push_back({static_cast<uint32_t>(start),
                      static_cast<uint32_t>(end - start)});
    if (cost_total) {
      costs.push_back(*cost_total - cost_last);
      cost_last = *cost_total;
    }
    ++inst_index;
    prev_copied = 0;
  }

  uint32_t RestoreId(Bits& bits, uint32_t base, CopyMap& cm, int role,
                     uint32_t b, uint32_t t_enc) {
    uint32_t mapped = 0;
    size_t at = 0;
    IdTable& tab = (*ids)[base];
    const bool has = tab.Find(b, k + 1, &mapped, &at);
    uint32_t t;
    if (has && bits.CodeBit(sig->map_hit[role],
                            Bits::kEncoding && mapped == t_enc)) {
      t = mapped;
    } else if (bits.CodeBit(sig->delta_hit[role][has ? 1 : 0],
                            Bits::kEncoding && b + cm.delta == t_enc)) {
      t = b + cm.delta;
    } else {
      const uint32_t z = CodeGamma(
          bits, sig->explicit_id[role],
          Bits::kEncoding ? ZigZag(t_enc - (b + cm.delta)) : 0u);
      t = b + cm.delta + UnZigZag(z);
    }
    tab.Set(at, b, k + 1, t);
    cm.delta = t - b;
    return t;
  }

  // 0: code the next instruction literally; 1: a copy was coded and its
  // instructions trained; -1: invalid stream (or, encoding, plan).
  template <class IO>
  ARIXV_NOINLINE int BeforeInstruction(ModelState& model, Bits& bits, IO& io,
                                       size_t body_words, size_t& body_pos,
                                       InstructionCarry& c) {
    if (!copies_allowed) return 0;
    const Copy* want = nullptr;
    if constexpr (Bits::kEncoding) {
      if (next < plan_count && plan[next].at == inst_index) want = &plan[next];
      if (next < plan_count && plan[next].at < inst_index) return -1;
    }
    if (!bits.CodeBit(sig->start[prev_copied], want != nullptr)) return 0;
    std::vector<Base>& bs = *bases;
    const bool rep_ok = rep_valid && rep_pos < bs[rep_base].nframes;
    bool is_rep = false;
    if (rep_ok) {
      is_rep = bits.CodeBit(sig->rep, Bits::kEncoding && want &&
                                          want->base == rep_base &&
                                          want->pos == rep_pos);
    }
    uint32_t base, pos;
    if (is_rep) {
      base = rep_base;
      pos = rep_pos;
    } else {
      if constexpr (Bits::kEncoding) {
        if (want->base >= k || !bs[want->base].valid) return -1;
      }
      const uint32_t d =
          CodeGamma(bits, sig->base_dist, Bits::kEncoding ? k - want->base : 0);
      if (d == 0 || d > k) return -1;
      base = k - d;
      if (!bs[base].valid || bs[base].nframes == 0) return -1;
      const uint32_t n = bs[base].nframes;
      const int nb = static_cast<int>(BitWidth(n - 1));
      pos = bits.CodeBitsDirect(Bits::kEncoding ? want->pos : 0u, nb);
      if (pos >= n) return -1;
    }
    const Base& B = bs[base];
    const uint32_t n = B.nframes;
    const uint32_t len = CodeGamma(bits, is_rep ? sig->len_rep : sig->len_new,
                                   Bits::kEncoding ? want->len : 0u);
    if (len == 0 || len > n - pos) return -1;
    TrainBits train;
    TrainIO tio(target);
    CopyMap& cm = (*maps)[base];
    if (cm.stamp != k + 1) cm = CopyMap{k + 1, 0};
    for (uint32_t j = 0; j < len; ++j) {
      const uint32_t fb = B.starts[pos + j], fe = B.starts[pos + j + 1];
      if (fe < fb || fe > B.nwords - 5) return -1;  // base frame out of range
      const Frame f{fb, fe - fb};
      if (f.nwords > body_words - body_pos) return -1;  // overruns the module
      restored.resize(f.nwords);
      for (uint32_t wi = 0; wi < f.nwords; ++wi) {
        const uint32_t bw = B.words[5 + f.begin + wi];
        const uint8_t role = B.mask[f.begin + wi];
        if (role > 2) return -1;  // roles index 3-entry signal models
        uint32_t tw = 0;
        if constexpr (Bits::kEncoding) {
          tw = target[5 + body_pos + wi];
          if (role == 0 && tw != bw) return -1;  // plan does not match
        }
        restored[wi] =
            role == 0 ? bw : RestoreId(bits, base, cm, role, bw, tw);
      }
      for (uint32_t wi = 0; wi < f.nwords; ++wi) io.EmitBodyWord(restored[wi]);
      const size_t start = body_pos;
      if (adapt) {
        CodeInstruction(model, train, tio, body_words, body_pos, c, *this);
      } else {
        TrackInstruction(model, target, body_words, body_pos, c, roles.data());
      }
      // The copied instruction must frame identically in the target.
      if (body_pos != start + f.nwords) return -1;
      frames.push_back({static_cast<uint32_t>(start), f.nwords});
      ++inst_index;
    }
    if constexpr (Bits::kEncoding) {
      if (want->len != len || want->pos != pos || want->base != base) {
        return -1;
      }
      ++next;
    }
    rep_valid = true;
    rep_base = base;
    rep_pos = pos + len;
    prev_copied = 1;
    return 1;
  }
};

// One module through the production CodeModule with the block hook.
template <class Bits, class IO>
arixv_status CodeModuleStructural(ModelState& model, Bits& bits, IO& io,
                                  size_t word_count, Hook<Bits>& hook) {
  hook.roles.assign(word_count - 5, 0);
  return CodeModule(model, bits, io, word_count, hook);
}

}  // namespace pack_codec
}  // namespace

namespace pack {

bool HostIsLittleEndian() {
  const uint32_t one = 1;
  uint8_t b;
  std::memcpy(&b, &one, 1);
  return b == 1;
}

arixv_status DecodeBlock(const uint8_t* payload, size_t payload_size,
                         uint8_t codec_flags, const uint64_t* sizes,
                         uint32_t count, const SharedMember* shared,
                         size_t ns_in, std::vector<DecodedMember>* out) {
  using namespace pack_codec;
  out->clear();
  if (!HostIsLittleEndian() || count == 0 || count > kMaxModules ||
      ns_in > kMaxModules) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }
  const uint32_t ns = static_cast<uint32_t>(ns_in);
  const bool warm = (codec_flags & kCodecWarm) != 0;
  const bool adapt = (codec_flags & kCodecFastDecode) == 0;
  RangeDecoder range(payload, payload_size);
  BitDecoder bits(range);
  SignalModels sig;
  std::vector<Base> bases(size_t{ns} + count);
  std::vector<IdTable> ids(bases.size());
  std::vector<CopyMap> maps(bases.size());
  for (uint32_t j = 0; j < ns; ++j) SharedBase(shared[j], &bases[j]);
  bool any_candidate = false;  // any copy source among members [0, g)
  for (uint32_t j = 0; j < ns; ++j) any_candidate |= bases[j].candidate;
  std::unique_ptr<ModelState> prev;
  out->resize(count);  // never reallocated: bases point into members
  std::vector<DecodedMember>& mods = *out;
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t g = ns + k;
    const size_t size = static_cast<size_t>(sizes[k]);
    DecodedMember& m = mods[k];
    if (g > 0 && bits.CodeBit(sig.dup, false)) {
      const uint32_t d = CodeGamma(bits, sig.dup_dist, 0u);
      if (d == 0 || d > g) return ARIXV_STATUS_BAD_CONTAINER;
      const uint32_t j = g - d;
      const uint8_t* src = j < ns ? shared[j].bytes : mods[j - ns].data();
      const size_t n = j < ns ? shared[j].size : mods[j - ns].size();
      if (n != size) return ARIXV_STATUS_BAD_CONTAINER;
      m.bytes.assign(src, src + n);
      continue;
    }
    if (!bits.CodeBit(sig.structural, false)) {
      ByteModel bm;
      std::vector<uint8_t>& o = m.bytes;
      o.reserve(CappedDecodeReserve(payload_size, size));
      CodeBytePayload(bits, bm, nullptr, size,
                      [&o](uint8_t b) { o.push_back(b); });
      if (bits.exhausted()) return ARIXV_STATUS_TRUNCATED;
      if (o.size() != size) return ARIXV_STATUS_BAD_CONTAINER;
      continue;
    }
    if (size < 20 || size % 4 != 0) return ARIXV_STATUS_BAD_CONTAINER;
    auto model = pack_transfer::NextModel(prev, warm);
    model->tracker.Reserve(CappedDecodeReserve(payload_size, size) / 4);
    m.structural = true;
    m.words.assign(size / 4, 0u);
    DecodeIO io(reinterpret_cast<uint8_t*>(m.words.data()), size);
    Hook<BitDecoder> hook;
    hook.ids = &ids;
    hook.maps = &maps;
    hook.sig = &sig;
    hook.bases = &bases;
    hook.k = g;
    hook.signal = true;
    hook.adapt = adapt;
    hook.any_candidate = any_candidate;
    hook.target = m.words.data();
    const arixv_status st =
        CodeModuleStructural(*model, bits, io, size / 4, hook);
    if (st != ARIXV_STATUS_OK) return st;
    if (!io.Complete()) return ARIXV_STATUS_TRUNCATED;
    const bool swapped = m.words[0] == kSpirvMagicSwapped;
    m.starts = StartsOf(hook.frames);
    m.mask = std::move(hook.roles);
    MakeBase(m.words.data(), m.words.size(), swapped, m.starts.data(),
             m.starts.empty() ? 0 : m.starts.size() - 1, m.mask.data(),
             &bases[g]);
    m.valid = bases[g].valid;
    if (!m.valid) {
      m.starts.clear();
      m.mask.clear();
    }
    any_candidate |= bases[g].candidate;
    prev = std::move(model);
  }
  if (!bits.ConsumedExactly()) return ARIXV_STATUS_BAD_CONTAINER;
  return ARIXV_STATUS_OK;
}

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
int64_t Log2Q16(uint64_t x) { return pack_codec::Log2Q16(x); }

bool ModuleIdRoles(const uint8_t* data, size_t size,
                   std::vector<FrameSpan>* frames, std::vector<uint8_t>* mask) {
  using namespace pack_codec;
  frames->clear();
  mask->clear();
  if (!IsSpirvModelEligible(data, size)) return false;
  const auto model = std::make_unique<ModelState>();
  model->tracker.Reserve(size / 4);
  TrainBits bits;
  EncodeIO io(data, size);
  Hook<TrainBits> hook;  // no signalling: every instruction literal
  if (CodeModuleStructural(*model, bits, io, size / 4, hook) !=
      ARIXV_STATUS_OK) {
    return false;
  }
  for (const Frame& f : hook.frames) frames->push_back({f.begin, f.nwords});
  *mask = std::move(hook.roles);
  return true;
}

bool EncodeBlock(const ModuleView* modules, size_t count, const Plan& plan,
                 uint8_t codec_flags, const SharedMember* shared, size_t ns,
                 std::vector<uint8_t>* payload) {
  using namespace pack_codec;
  payload->clear();
  if (!HostIsLittleEndian() || count == 0 || count > kMaxModules ||
      ns > kMaxModules) {
    return false;
  }
  const bool warm = (codec_flags & kCodecWarm) != 0;
  const bool adapt = (codec_flags & kCodecFastDecode) == 0;
  RangeEncoder range(*payload);
  BitEncoder bits(range);
  SignalModels sig;
  // Global member space: shared members [0, ns), then this block's.
  std::vector<Base> bases(ns + count);
  std::vector<std::vector<uint32_t>> own_starts(count);
  std::vector<std::vector<uint8_t>> own_masks(count);
  std::vector<IdTable> ids(bases.size());
  std::vector<CopyMap> maps(bases.size());
  for (size_t j = 0; j < ns; ++j) SharedBase(shared[j], &bases[j]);
  bool any_candidate = false;  // any copy source among members [0, g)
  for (size_t j = 0; j < ns; ++j) any_candidate |= bases[j].candidate;
  std::vector<std::vector<uint32_t>> all_words(count);
  std::unique_ptr<ModelState> prev;
  auto bytes_of = [&](size_t g, const uint8_t** d, size_t* n) {
    if (g < ns) {
      *d = shared[g].bytes;
      *n = shared[g].size;
    } else {
      *d = modules[g - ns].data;
      *n = modules[g - ns].size;
    }
  };
  for (size_t k = 0; k < count; ++k) {
    const size_t g = ns + k;
    const ModuleView& m = modules[k];
    const std::vector<Copy>* copies =
        k < plan.size() ? &plan[k] : nullptr;
    size_t dup_of = g;
    for (size_t j = 0; j < g; ++j) {
      const uint8_t* d;
      size_t n;
      bytes_of(j, &d, &n);
      if (n == m.size && (n == 0 || std::memcmp(d, m.data, n) == 0)) {
        dup_of = j;
        break;
      }
    }
    if (g > 0 && bits.CodeBit(sig.dup, dup_of != g)) {
      CodeGamma(bits, sig.dup_dist, static_cast<uint32_t>(g - dup_of));
      continue;  // bases[g] stays invalid
    }
    const bool eligible = IsSpirvModelEligible(m.data, m.size);
    if (!bits.CodeBit(sig.structural, eligible)) {
      ByteModel bm;
      CodeBytePayload(bits, bm, m.data, m.size, [](uint8_t) {});
      continue;
    }
    auto model = pack_transfer::NextModel(prev, warm);
    model->tracker.Reserve(m.size / 4);
    std::vector<uint32_t>& words = all_words[k];
    words.resize(m.size / 4);
    std::memcpy(words.data(), m.data, words.size() * 4);
    EncodeIO io(m.data, m.size);
    Hook<BitEncoder> hook;
    hook.ids = &ids;
    hook.maps = &maps;
    hook.sig = &sig;
    hook.bases = &bases;
    hook.k = static_cast<uint32_t>(g);
    hook.signal = true;
    hook.adapt = adapt;
    hook.any_candidate = any_candidate;
    hook.target = words.data();
    hook.plan = copies ? copies->data() : nullptr;
    hook.plan_count = copies ? copies->size() : 0;
    const bool swapped = io.SwappedHint();
    if (swapped && hook.plan_count) return false;
    if (CodeModuleStructural(*model, bits, io, m.size / 4, hook) !=
        ARIXV_STATUS_OK) {
      return false;
    }
    if (hook.next != hook.plan_count) return false;  // plan copies unused
    own_masks[k] = std::move(hook.roles);
    own_starts[k] = StartsOf(hook.frames);
    MakeBase(words.data(), words.size(), swapped, own_starts[k].data(),
             own_starts[k].empty() ? 0 : own_starts[k].size() - 1,
             own_masks[k].data(), &bases[g]);
    any_candidate |= bases[g].candidate;
    prev = std::move(model);
  }
  bits.Flush();
  return true;
}
#endif  // ARIXV_ENCODER

}  // namespace pack

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
namespace {
namespace pack_codec {

// Adaptive binary cost estimate (KT-style counts), in Q16 bits:
// -log2((c + 0.4) / (n + 0.8)) computed exactly as
// log2(5n + 4) - log2(5c + 2).
struct Bin {
  uint32_t n0 = 0, n1 = 0;
  CostQ16 Cost(bool bit) const {
    const uint64_t c = bit ? n1 : n0;
    return Log2Q16(5 * (uint64_t{n0} + n1) + 4) - Log2Q16(5 * c + 2);
  }
  CostQ16 Code(bool bit) {
    const CostQ16 c = Cost(bit);
    (bit ? n1 : n0)++;
    return c;
  }
};

// Elias-gamma length of n >= 1, in Q16 bits.
inline CostQ16 GammaLen(uint32_t n) {
  return static_cast<CostQ16>(2 * (BitWidth(n) - 1) + 1) << kCostShift;
}

// Planner search limits: candidates per position (most recent first) and
// the minimum new-copy length (= the hashed n-gram length).
constexpr uint32_t kPlanDepth = 8;
constexpr uint32_t kPlanMinLen = 3;

struct PlanModels {
  Bin start[2];
  Bin rep;
  Bin map_hit[3];
  Bin delta_hit[3][2];
};

struct PlanMod {
  uint32_t member = 0;
  bool base_ok = false;   // usable as a copy source
  bool target_ok = false; // parsed (eligible, not swapped)
  std::vector<uint32_t> words;  // native order
  std::vector<Frame> frames;
  std::vector<uint8_t> mask;    // per body word
  std::vector<CostQ16> bits;    // per instruction in-context literal cost
  std::vector<uint64_t> tok;    // per instruction abstract-token hash
  CostQ16 log2_bound = CostQ16{1} << kCostShift;
  const uint32_t* Inst(size_t i) const { return words.data() + 5 + frames[i].begin; }
  const uint8_t* Mask(size_t i) const { return mask.data() + frames[i].begin; }
};

inline uint64_t Mix64(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

// Abstract equality: same length, same roles, same non-ID words.
bool SameAbs(const PlanMod& a, size_t i, const PlanMod& b, size_t j) {
  if (a.tok[i] != b.tok[j]) return false;
  const Frame& fa = a.frames[i];
  const Frame& fb = b.frames[j];
  if (fa.nwords != fb.nwords) return false;
  const uint32_t* wa = a.Inst(i);
  const uint32_t* wb = b.Inst(j);
  const uint8_t* ma = a.Mask(i);
  const uint8_t* mb = b.Mask(j);
  for (uint32_t w = 0; w < fa.nwords; ++w) {
    if (ma[w] != mb[w]) return false;
    if (ma[w] == 0 && wa[w] != wb[w]) return false;
  }
  return true;
}

// Frames, roles and per-instruction literal costs of a module coded alone.
bool CostPass(const pack::ModuleView& m, PlanMod* pm) {
  if (!IsSpirvModelEligible(m.data, m.size)) return false;
  const auto model = std::make_unique<ModelState>();
  model->tracker.Reserve(m.size / 4);
  CostBits bits;
  EncodeIO io(m.data, m.size);
  Hook<CostBits> hook;
  hook.signal = false;
  hook.cost_total = &bits.total;
  if (CodeModuleStructural(*model, bits, io, m.size / 4, hook) !=
      ARIXV_STATUS_OK) {
    return false;
  }
  pm->frames = std::move(hook.frames);
  pm->mask = std::move(hook.roles);
  pm->bits = std::move(hook.costs);
  return pm->bits.size() == pm->frames.size();
}

// Greedy parser: tokens are instructions with ID words replaced by their
// roles; candidates come from a kPlanMinLen-instruction hash chain over
// earlier unique members (most recent kPlanDepth). A copy is taken when its
// estimated saving (in-context literal bits plus saved start flags) exceeds
// its estimated signalling and ID-restoration cost.
class Planner {
 public:
  explicit Planner(std::vector<PlanMod>& mods) : mods_(mods) {}

  void AddToIndex(uint32_t ui) {
    const PlanMod& m = mods_[ui];
    if (!m.base_ok) return;
    const size_t n = m.tok.size();
    for (size_t p = 0; p + kPlanMinLen <= n; ++p) {
      index_[Key(m, p)].push_back({ui, static_cast<uint32_t>(p)});
    }
  }

  void Parse(uint32_t k, std::vector<Copy>* out) {
    const PlanMod& T = mods_[k];
    const size_t n = T.tok.size();
    maps_.assign(k, {});
    deltas_.assign(k, 0);
    size_t pos = 0;
    int prev_copy = 0;
    bool have_rep = false;
    uint32_t rep_m = 0, rep_p = 0;
    struct Cand {
      uint32_t m, p;
      bool is_rep;
    };
    std::vector<Cand> cands;
    while (pos < n) {
      const CostQ16 flag1 = md_.start[prev_copy].Cost(true);
      const CostQ16 flag0 = md_.start[prev_copy].Cost(false);
      cands.clear();
      rep_possible_ = have_rep && rep_p < mods_[rep_m].tok.size();
      if (rep_possible_) cands.push_back({rep_m, rep_p, true});
      if (pos + kPlanMinLen <= n && k > 0) {
        const auto it = index_.find(Key(T, pos));
        if (it != index_.end()) {
          const auto& lst = it->second;
          size_t taken = 0;
          for (size_t i = lst.size(); i-- > 0 && taken < kPlanDepth;) {
            ++taken;
            if (rep_possible_ && lst[i].first == rep_m &&
                lst[i].second == rep_p) {
              continue;
            }
            cands.push_back({lst[i].first, lst[i].second, false});
          }
        }
      }
      CostQ16 best_net = 0;
      uint32_t best_len = 0;
      Cand best{};
      bool any = false;
      for (const Cand& c : cands) {
        uint32_t len = 0;
        const CostQ16 net =
            Evaluate(T, pos, k, c.m, c.p, c.is_rep, flag1, flag0, &len);
        if (len && (!any || net > best_net)) {
          any = true;
          best_net = net;
          best_len = len;
          best = c;
        }
      }
      if (any && best_net > 0) {
        md_.start[prev_copy].Code(true);
        if (rep_possible_) md_.rep.Code(best.is_rep);
        Commit(T, pos, best.m, best.p, best_len);
        out->push_back({static_cast<uint32_t>(pos), mods_[best.m].member,
                        best.p, best_len});
        pos += best_len;
        have_rep = true;
        rep_m = best.m;
        rep_p = best.p + best_len;
        prev_copy = 1;
      } else {
        md_.start[prev_copy].Code(false);
        ++pos;
        prev_copy = 0;
      }
    }
  }

 private:
  uint64_t Key(const PlanMod& m, size_t p) const {
    uint64_t h = 0;
    for (uint32_t j = 0; j < kPlanMinLen; ++j) h = Mix64(h ^ m.tok[p + j]);
    return h;
  }

  CostQ16 Evaluate(const PlanMod& T, size_t pos, uint32_t k, uint32_t m,
                   uint32_t p, bool is_rep, CostQ16 flag1, CostQ16 flag0,
                   uint32_t* out_len) {
    const PlanMod& B = mods_[m];
    const size_t limit = std::min(T.tok.size() - pos, B.tok.size() - p);
    const auto& mp = maps_[m];
    uint32_t delta = deltas_[m];
    overlay_.clear();
    CostQ16 sig = flag1;
    uint32_t min_len;
    if (is_rep) {
      sig += md_.rep.Cost(true);
      min_len = 1;
    } else {
      if (k > 0 && rep_possible_) sig += md_.rep.Cost(false);
      sig += GammaLen(k - m) + Log2Q16(std::max<size_t>(B.tok.size(), 1));
      min_len = kPlanMinLen;
    }
    CostQ16 benefit = 0, idb = 0, best = 0;
    uint32_t best_len = 0;
    for (size_t j = 0; j < limit; ++j) {
      if (!SameAbs(T, pos + j, B, p + j)) break;
      benefit += T.bits[pos + j] + flag0;
      const Frame& f = B.frames[p + j];
      const uint32_t* bw = B.Inst(p + j);
      const uint32_t* tw = T.Inst(pos + j);
      const uint8_t* mk = B.Mask(p + j);
      for (uint32_t wi = 0; wi < f.nwords; ++wi) {
        const uint8_t role = mk[wi];
        if (!role) continue;
        const uint32_t b = bw[wi], t = tw[wi];
        bool has = false;
        uint32_t cur = 0;
        const auto ov = overlay_.find(b);
        if (ov != overlay_.end()) {
          has = true;
          cur = ov->second;
        } else {
          const auto it = mp.find(b);
          if (it != mp.end()) {
            has = true;
            cur = it->second;
          }
        }
        if (has && cur == t) {
          idb += md_.map_hit[role].Cost(true);
        } else {
          if (has) idb += md_.map_hit[role].Cost(false);
          const bool hit2 = b + delta == t;
          idb += md_.delta_hit[role][has ? 1 : 0].Cost(hit2);
          if (!hit2) idb += T.log2_bound;
        }
        overlay_[b] = t;
        delta = t - b;
      }
      const uint32_t len = static_cast<uint32_t>(j + 1);
      if (len >= min_len) {
        const CostQ16 lbits =
            is_rep ? GammaLen(len) : GammaLen(len - kPlanMinLen + 1);
        const CostQ16 net = benefit - idb - sig - lbits;
        if (net > best) {
          best = net;
          best_len = len;
        }
      }
    }
    *out_len = best_len;
    return best;
  }

  void Commit(const PlanMod& T, size_t pos, uint32_t m, uint32_t p,
              uint32_t len) {
    const PlanMod& B = mods_[m];
    auto& mp = maps_[m];
    for (uint32_t j = 0; j < len; ++j) {
      const Frame& f = B.frames[p + j];
      const uint32_t* bw = B.Inst(p + j);
      const uint32_t* tw = T.Inst(pos + j);
      const uint8_t* mk = B.Mask(p + j);
      for (uint32_t wi = 0; wi < f.nwords; ++wi) {
        const uint8_t role = mk[wi];
        if (!role) continue;
        const uint32_t b = bw[wi], t = tw[wi];
        const auto it = mp.find(b);
        const bool has = it != mp.end();
        const bool map_hit = has && it->second == t;
        if (has) md_.map_hit[role].Code(map_hit);
        if (!map_hit) {
          md_.delta_hit[role][has ? 1 : 0].Code(b + deltas_[m] == t);
        }
        mp[b] = t;
        deltas_[m] = t - b;
      }
    }
  }

  std::vector<PlanMod>& mods_;
  PlanModels md_;
  bool rep_possible_ = false;
  std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>
      index_;
  std::vector<std::unordered_map<uint32_t, uint32_t>> maps_;
  std::vector<uint32_t> deltas_;
  std::unordered_map<uint32_t, uint32_t> overlay_;
};

}  // namespace pack_codec
}  // namespace

namespace pack {

void PlanBlock(const ModuleView* modules, size_t count,
               const SharedMember* shared, size_t ns, Plan* plan) {
  using namespace pack_codec;
  plan->assign(count, {});
  if (!HostIsLittleEndian()) return;
  auto tokenize = [](PlanMod& pm) {
    pm.tok.resize(pm.frames.size());
    for (size_t i = 0; i < pm.frames.size(); ++i) {
      uint64_t th = Mix64(pm.frames[i].nwords);
      const uint32_t* w = pm.Inst(i);
      const uint8_t* mk = pm.Mask(i);
      for (uint32_t j = 0; j < pm.frames[i].nwords; ++j) {
        th = Mix64(th ^ (mk[j] ? 0x100000000ull * mk[j] : w[j]));
      }
      pm.tok[i] = th;
    }
  };
  // Unique members in global order (shared first; exact duplicates are
  // coded as such). Shared members are copy sources only.
  std::vector<PlanMod> mods;
  std::unordered_map<uint64_t, std::vector<size_t>> by_hash;
  std::vector<ModuleView> all(ns + count);
  for (size_t j = 0; j < ns; ++j) all[j] = {shared[j].bytes, shared[j].size};
  for (size_t k = 0; k < count; ++k) all[ns + k] = modules[k];
  for (size_t k = 0; k < ns + count; ++k) {
    const ModuleView& m = all[k];
    uint64_t h = Mix64(m.size);
    for (size_t i = 0; i + 8 <= m.size; i += 8) {
      uint64_t w;
      std::memcpy(&w, m.data + i, 8);
      h = Mix64(h ^ w);
    }
    for (size_t i = m.size & ~size_t{7}; i < m.size; ++i) h = Mix64(h ^ m.data[i]);
    auto& bucket = by_hash[h];
    bool dup = false;
    for (size_t j : bucket) {
      if (all[j].size == m.size &&
          (m.size == 0 || std::memcmp(all[j].data, m.data, m.size) == 0)) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    bucket.push_back(k);
    PlanMod pm;
    pm.member = static_cast<uint32_t>(k);
    if (k < ns) {
      // Shared: exactly the decoder's base view; no literal costs needed.
      const SharedMember& s = shared[k];
      if (s.valid) {
        pm.words.assign(s.words, s.words + s.nwords);
        pm.frames.resize(s.nframes);
        for (size_t i = 0; i < s.nframes; ++i) {
          pm.frames[i] = {s.starts[i], s.starts[i + 1] - s.starts[i]};
        }
        pm.mask.assign(s.mask, s.mask + (s.nwords - 5));
        pm.base_ok = !pm.frames.empty();
        tokenize(pm);
      }
    } else if (CostPass(m, &pm)) {
      pm.words.resize(m.size / 4);
      std::memcpy(pm.words.data(), m.data, pm.words.size() * 4);
      const bool swapped = pm.words[0] == kSpirvMagicSwapped;
      pm.base_ok = pm.target_ok = !swapped && !pm.frames.empty();
      pm.log2_bound = pack_codec::Log2Q16(std::max<uint32_t>(pm.words[3], 2));
      tokenize(pm);
    } else {
      pm.frames.clear();
      pm.mask.clear();
      pm.bits.clear();
    }
    mods.push_back(std::move(pm));
  }
  Planner planner(mods);
  for (uint32_t ui = 0; ui < mods.size(); ++ui) {
    if (mods[ui].target_ok) {
      planner.Parse(ui, &(*plan)[mods[ui].member - ns]);
    }
    planner.AddToIndex(ui);
  }
}
}  // namespace pack
#endif  // ARIXV_ENCODER
