// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "model.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <type_traits>
#ifdef ARIXV_MEMSTATS
#include <map>
#endif
#include <unordered_map>
#include <vector>

#include "grammar_tables.h"
#include "platform.h"
#include "prob.h"
#include "range_coder.h"
#include "tracker.h"
#include "tuning.h"

namespace arixv {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kSpirvMagicSwapped = 0x03022307u;
#ifndef ARIXV_KOPCODE_ORDER1_MIN_COUNT
#define ARIXV_KOPCODE_ORDER1_MIN_COUNT 4
#endif
constexpr uint32_t kOpcodeOrder1MinCount = ARIXV_KOPCODE_ORDER1_MIN_COUNT;
constexpr size_t kInlineInstructionWords = 256;
constexpr size_t kInlineOperandWords = 8;

template <size_t kInlineWords>
class WordBuffer {
 public:
  void clear() {
    size_ = 0;
    spill_.clear();
  }

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  uint32_t front() const { return data()[0]; }
  uint32_t back() const { return data()[size_ - 1]; }

  const uint32_t* data() const {
    return spill_.empty() ? inline_words_.data() : spill_.data();
  }

  uint32_t* data() {
    return spill_.empty() ? inline_words_.data() : spill_.data();
  }

  uint32_t operator[](size_t index) const { return data()[index]; }

  void push_back(uint32_t word) {
    if (spill_.empty() && size_ < inline_words_.size()) {
      inline_words_[size_++] = word;
      return;
    }
    if (spill_.empty()) {
      spill_.assign(inline_words_.begin(), inline_words_.end());
    }
    spill_.push_back(word);
    size_ = spill_.size();
  }

  // Shrink-only: growing would expose unwritten words.
  void resize(size_t size) {
    assert(size <= size_);
    if (!spill_.empty()) {
      spill_.resize(size);
    }
    size_ = size;
  }

 private:
  std::array<uint32_t, kInlineWords> inline_words_{};
  std::vector<uint32_t> spill_;
  size_t size_ = 0;
};

// Keeps the decode-only direct-word path out of the operand switch while
// retaining a compile-time split for encoding. The hot call operator is
// explicitly inlined on the supported GCC/Clang toolchains.
template <class Bits>
class SingleWordStager {
 public:
  SingleWordStager(WordBuffer<kInlineOperandWords>& coded_words,
                   uint32_t& direct_word, bool& has_direct_word)
      : coded_words_(coded_words),
        direct_word_(direct_word),
        has_direct_word_(has_direct_word) {}

  ARIXV_FORCE_INLINE void operator()(uint32_t word) {
    if constexpr (!Bits::kEncoding) {
      direct_word_ = word;
      has_direct_word_ = true;
    } else {
      coded_words_.push_back(word);
    }
  }

 private:
  WordBuffer<kInlineOperandWords>& coded_words_;
  uint32_t& direct_word_;
  bool& has_direct_word_;
};

uint32_t LoadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void StoreLe32(uint32_t v, uint8_t* out) {
  out[0] = static_cast<uint8_t>(v & 0xffu);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  out[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
  out[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

uint32_t ByteSwap32(uint32_t v) {
  return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
         ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

bool IsCanonicalVersion(uint32_t word) {
  const uint32_t byte0 = word & 0xffu;
  const uint32_t minor = (word >> 8) & 0xffu;
  const uint32_t major = (word >> 16) & 0xffu;
  const uint32_t byte3 = (word >> 24) & 0xffu;
  return byte0 == 0 && byte3 == 0 && major < 16 && minor < 16;
}

// A fresh Prob on the spec-valid side (bit 1) of a validity flag.
inline Prob ValidFlag() { return Prob::WithValue(kValidFlagPrior); }
// The same for flags whose spec-valid side is bit 0.
inline Prob ValidFlagZero() {
  return Prob::WithValue(static_cast<uint16_t>(kProbScale - kValidFlagPrior));
}

struct CountModel {
  Prob same_as_last;
  UintModel extra;
};

// Copies a generated cold-start prior (GetPriorCdf pool index) into t.
inline void LoadPrior(SymbolCdf& t, uint16_t pool_index) {
  std::memcpy(t.d, GetPriorCdf(pool_index), sizeof(t.d));
}

// Opcode-index tree, symbol-for-symbol identical to BitTree<10> (chunks of
// 2, 4, 4 bits: 1 root, 4 mid, 64 leaf tables) but with the 64 leaf tables —
// 2 KB of BitTree<10>'s 2.2 KB — allocated lazily in four 16-table groups,
// one per top-chunk value. A context only reaches the leaves under the top
// values it actually sees. A fresh group starts from the section prior when
// the tree has one (GetOpcodeSectionPrior: instructions the module layout
// allows in that section), else uniform like the eager tree's untouched
// tables.
struct OpcodeTree {
  static_assert(BitTree<10>::kFirstChunk == 2 && BitTree<10>::NumTables() == 69,
                "OpcodeTree mirrors BitTree<10>'s 2/4/4 chunk layout");
  using LeafGroup = std::array<SymbolCdf, 4>;
  SymbolCdf root;
  std::array<SymbolCdf, 4> mid;
  std::array<ArenaPtr<LeafGroup>, 16> leaf;  // by (top, mid >> 2)
  // Seeds root and mid from a section prior (GetOpcodeSectionPrior); the
  // caller passes the same prior to CodeValue, which seeds leaf groups as
  // they are created (not stored here: it would cost 16 bytes per tree).
  void SetPrior(const uint16_t* p) {
    LoadPrior(root, p[0]);
    for (uint32_t i = 0; i < 4; ++i) LoadPrior(mid[i], p[1 + i]);
  }

  template <typename Coder>
  uint32_t CodeValue(Coder& coder, Arena& arena, uint32_t value,
                     const uint16_t* prior = nullptr) {
    if constexpr (Coder::kRawResidual) {
      (void)arena;
      return coder.CodeBitsDirect(value, 10);
    } else {
#ifdef ARIXV_TRACE_DECISIONS
      if (Coder::kEncoding && g_tree_trace != nullptr) {
        g_tree_trace->push_back({this, 10, value});
      }
#endif
      StatSiteScope<Coder> site(kSiteBitTree);
      const uint32_t top = coder.template CodeSymbol<4, kSymbolRateCap>(
          root, (value >> 8) & 3u);
      const uint32_t m = coder.template CodeSymbol<16, kSymbolRateCap>(
          mid[top], (value >> 4) & 15u);
      const uint32_t low = coder.template CodeSymbol<16, kSymbolRateCap>(
          Leaf(arena, top, m, prior), value & 15u);
      return Coder::kEncoding ? value : ((top << 8) | (m << 4) | low);
    }
  }

  // Fast-shadow update along value's path, matching CodeValue's warm-up
  // schedule so the order-1 tree adapts quickly during shadow training and
  // continues its warm-up count seamlessly when promoted.
  void Train(Arena& arena, uint32_t value, const uint16_t* prior = nullptr) {
    const uint32_t top = (value >> 8) & 3u;
    const uint32_t m = (value >> 4) & 15u;
    SymbolCdfUpdate<4, kSymbolRateCap>(root, static_cast<int>(top));
    SymbolCdfUpdate<16, kSymbolRateCap>(mid[top], static_cast<int>(m));
    SymbolCdfUpdate<16, kSymbolRateCap>(Leaf(arena, top, m, prior),
                                       static_cast<int>(value & 15u));
  }

 private:
  SymbolCdf& Leaf(Arena& arena, uint32_t top, uint32_t m,
                  const uint16_t* prior) {
    ArenaPtr<LeafGroup>& group = leaf[(top << 2) | (m >> 2)];
    if (!group) {
      MakeGroup(arena, group, top, m, prior);
    }
    return (*group)[m & 3u];
  }

  // Cold path (once per group per tree), kept out of the coding loop.
  ARIXV_NOINLINE static void MakeGroup(Arena& arena, ArenaPtr<LeafGroup>& group,
                                       uint32_t top, uint32_t m,
                                       const uint16_t* prior) {
    group.Make(arena);
    if (prior != nullptr) {
      for (uint32_t i = 0; i < 4; ++i) {
        LoadPrior((*group)[i], prior[5 + ((top << 4) | (m & ~3u) | i)]);
      }
    }
  }
};

// Order-1 opcode context (keyed by the previous instruction), created on
// first use with its tree: the count and tree used to sit in an eager
// 876-entry table, half of whose bytes were tree pointers.
struct OpcodeCtx {
  uint32_t count;
  OpcodeTree tree;
};

struct ValueEnumModel {
  Prob known = ValidFlag();  // a grammar enumerant
  LazyLeafTree8<kSymbolRateCap> index;
  UintModel escape;
};

struct BitEnumModel {
  std::vector<Prob> bits;
  Prob unknown = ValidFlagZero();  // bits outside the grammar's enumerants
  UintModel residual;
};

struct ExtInstNumberModel {
  Prob known = ValidFlag();  // an instruction of the set's grammar
  LazyLeafTree8<kSymbolRateCap> index;
  UintModel escape;
};

// Grammar cold-start prior for an 8-bit index tree: a valid index is one of
// the grammar's n entries, so the tree starts uniform over [0, n) (other
// values keep the minimum frequency) instead of uniform over all 256.
struct IndexLeafPrior {
  uint32_t n;
  void operator()(uint32_t high, SymbolCdf& leaf) const {
    if (high == (n >> 4) && (n & 15u) != 0) {
      LoadPrior(leaf, GetIndexLeafPrior(n & 15u));
    }
  }
};

template <typename T>
T& EnsureIndexContext(Arena& arena, ArenaPtr<T>& slot, uint32_t n) {
  if (!slot) {
    slot.Make(arena);
    LoadPrior(slot->index.root, GetIndexRootPrior(n));
  }
  return *slot;
}

UintModel& EnsureUint(Arena& arena, ArenaPtr<UintModel>& model) {
  if (!model) {
    model.Make(arena);
  }
  return *model;
}

ARIXV_NOINLINE UintModel& MakeUintWithPrior(Arena& arena,
                                            ArenaPtr<UintModel>& model,
                                            const BitTree<6>& prior) {
  model.Make(arena);
  if constexpr (kUintLengthBits == 6) {
    model->length = prior;
  }
  return *model;
}

UintModel& EnsureUintWithPrior(Arena& arena, ArenaPtr<UintModel>& model,
                               const BitTree<6>& prior) {
  return model ? *model : MakeUintWithPrior(arena, model, prior);
}

// Lazily created contexts are built (as T{}) on first use. A model lives for
// exactly one encode or decode, so a context, once made, is never stale.
template <typename T>
T& EnsureContext(Arena& arena, ArenaPtr<T>& slot) {
  if (!slot) {
    slot.Make(arena);
  }
  return *slot;
}

// For the few context types that own heap storage (BitEnumModel).
template <typename T>
T& EnsureContext(std::unique_ptr<T>& slot) {
  if (!slot) {
    slot = std::make_unique<T>();
  }
  return *slot;
}

// ARIXV_STATS=1: entropy cost per field category, accumulated process-wide
// across Encode calls and dumped to stderr at exit (bench/diagnostics only).
enum StatCat {
  kStatHeader = 0, kStatOpcode, kStatWordCount, kStatResultId,
  kStatResultType, kStatIdRef, kStatValueEnum, kStatBitEnum, kStatLitInt,
  kStatLitCdn, kStatExtNum, kStatString, kStatSpecOp, kStatOpaque, kStatBound,
  kStatCount,
};
constexpr const char* kStatNames[kStatCount] = {
    "header", "opcode", "word_count", "result_id", "result_type", "id_ref",
    "value_enum", "bit_enum", "literal_int", "literal_cdn", "ext_inst_num",
    "string", "spec_op", "opaque_words", "bound",
};
double g_stat_bits[kStatCount];
void DumpStats();
bool StatsEnabled() {
  static const bool enabled = [] {
    const bool e = std::getenv("ARIXV_STATS") != nullptr;
    if (e) std::atexit(DumpStats);
    return e;
  }();
  return enabled;
}
void DumpStats() {
  double total = 0;
  for (double b : g_stat_bits) total += b;
  std::fprintf(stderr, "ARIXV_STATS (encoder entropy, corpus-wide):\n");
  for (int i = 0; i < kStatCount; ++i) {
    std::fprintf(stderr, "  %-14s %12.0f bytes (%5.1f%%)\n", kStatNames[i],
                 g_stat_bits[i] / 8, total > 0 ? 100 * g_stat_bits[i] / total : 0);
  }
  std::fprintf(stderr, "  %-14s %12.0f bytes\n", "TOTAL", total / 8);

  static_assert(kStatCount <= kStatMaxCats, "stat category table too small");
  constexpr const char* kSiteNames[kSiteCount] = {
      "flag", "bit_tree", "uint_len", "uint_mant", "byte"};
  uint64_t site_total[kSiteCount] = {};
  uint64_t adaptive_total = 0;
  uint64_t direct_total = 0;
  std::fprintf(stderr,
               "ARIXV_STATS decisions (adaptive by primitive, direct bits):\n");
  std::fprintf(stderr, "  %-14s", "category");
  for (const char* n : kSiteNames) std::fprintf(stderr, " %11s", n);
  std::fprintf(stderr, " %11s %11s\n", "adaptive", "direct");
  for (int i = 0; i < kStatCount; ++i) {
    uint64_t row = 0;
    std::fprintf(stderr, "  %-14s", kStatNames[i]);
    for (int s = 0; s < kSiteCount; ++s) {
      std::fprintf(stderr, " %11llu",
                   static_cast<unsigned long long>(g_stat_adaptive[i][s]));
      row += g_stat_adaptive[i][s];
      site_total[s] += g_stat_adaptive[i][s];
    }
    adaptive_total += row;
    direct_total += g_stat_direct[i];
    std::fprintf(stderr, " %11llu %11llu\n",
                 static_cast<unsigned long long>(row),
                 static_cast<unsigned long long>(g_stat_direct[i]));
  }
  std::fprintf(stderr, "  %-14s", "TOTAL");
  for (uint64_t t : site_total) {
    std::fprintf(stderr, " %11llu", static_cast<unsigned long long>(t));
  }
  std::fprintf(stderr, " %11llu %11llu\n",
               static_cast<unsigned long long>(adaptive_total),
               static_cast<unsigned long long>(direct_total));
}

// Per-instruction caches: the last ID and last literal seen in each of the
// 16 operand slots (each guarded by a valid flag) and the last word count.
// InstGroup{} is the first-touch state.
// ID-operand hit and rank models for one (instruction, operand slot),
// created on the slot's first ID. Sharing them per operand position across
// instructions cost 2.4% of the corpus; starting a private model from the
// shared one after a few uses was worse than starting it fresh.
struct IdSlotModel {
  Prob in_typed;
  Prob in_list;
  BitTree<kRankTreeBits> typed_rank;
  BitTree<kRankTreeBits> rank;
};

struct InstGroup {
  // 1 + index into ModelState::id_slots (0 = none yet); see IdSlotModel.
  // Indices, not pointers: 32 bytes per instruction instead of 128.
  uint16_t id_slot[16];
  uint32_t last_id[16];
  uint32_t last_id_list[16];  // last_id's type's recency list (valid with it)
  uint32_t last_literal[16];
  // Per-slot ID history state:
  //   bit 0     = last_id / last_id_list valid
  //   bits 1-3  = adaptive typed-list source mode for id_idx > 0
  //               (0 = own_last, 1 = op0_list, 2 = prev_op_list,
  //                3 = elem(prev_op)_list, 4 = prev_res_list)
  //   bits 4-5  = 2-bit hysteresis counter for id_idx == 0 (0..2, init 1;
  //               uses prev_res_list when == 2, own_last otherwise)
  uint8_t last_id_state[16];
  uint8_t last_literal_valid[16];
  uint32_t last_word_count;
  // Presence flag for kInstOptionalTail instructions (sits in the alignment
  // padding before BitTree<3>, adding zero bytes to InstGroup).
  Prob has_optional = ValidFlagZero();
  // Which operand-derived candidate the deferred Result Type matched (see
  // CodeDeferredResultType): 0-6, or 7 for none.
  BitTree<3> result_type_choice;
};

// Everything kept per known instruction, created on its first use.
struct InstContext {
  InstGroup group;
};

// Reverses the low 23 bits (IEEE-754 float32 mantissa) so the most-significant
// mantissa bits — the ones nonzero in simple fractions — sit in the low bit
// positions where UintModel codes small values.
inline uint32_t ReverseBits23(uint32_t v) {
  v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
  v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
  v = ((v & 0x0f0f0f0fu) << 4) | ((v >> 4) & 0x0f0f0f0fu);
  v = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
  v = (v << 16) | (v >> 16);
  return v >> 9;
}

// Per-(instruction, slot, preceding-value-enum) literal history for stride
// prediction (e.g. OpDecorate/OpMemberDecorate Offset, Location, Binding).
// Direct-mapped and fixed-size so hostile input cannot grow memory.
struct EnumLiteralCache {
  static constexpr uint32_t kBits = 5;
  static constexpr size_t kSize = size_t{1} << kBits;
  struct Entry {
    uint64_t key;  // bit 63 set when occupied; 0 = empty
    uint32_t last;
    uint32_t stride;
  };
  Entry entries[kSize];
  Prob stride_match[9];  // indexed by Slot8(slot)
};

// Per-type last integer constant value, for delta coding of 1-word integer
// LiteralContextDependentNumber operands.
struct IntConstCache {
  static constexpr size_t kSize = 8;
  uint32_t type_id[kSize];
  uint32_t last[kSize];
  uint32_t next_evict;
};

// Literal-string model, all learned per module from grammar-derived starts:
// - a whole-string dictionary of grammar names (extensions, ext-inst sets);
// - an order-2 match model over every string seen so far, primed with the
//   grammar's enumerant names and the dictionary (GetStringPrimer());
// - order-1 bytes whose contexts start as a copy of a running order-0
//   model rather than uniform, so a module's first strings still code well.
struct StringMatchTable {
  // Virtual history position of the byte that followed the context last
  // time (primer first, then this module's strings); 0 = none.
  std::array<uint32_t, 1u << kStringMatchBits> pos;
};

// Must match the generator's table construction.
inline uint32_t StringMatchHash(uint8_t a, uint8_t b) {
  const uint32_t key = (static_cast<uint32_t>(a) << 8) | b;
  return (key * 0x9E3779B1u) >> (32 - kStringMatchBits);
}

struct StringModel {
  explicit StringModel(size_t classes) : dict_flag(classes) {
    LoadPrior(order0.root, GetStringOrder0RootPrior());
  }

  // Order-1 byte trees (bytes.tree[prev]) and the order-0 tree they start
  // from; both allocate leaves in bytes.arena.
  ByteModel bytes;
  ByteTree order0{};

  std::vector<Prob> dict_flag;  // per instruction class
  BitTree<8> dict_index;

  std::array<Prob, 16> match_flag{};  // per current match length (capped)
  ArenaPtr<StringMatchTable> match;   // copied from the primed table on first use
  std::vector<uint8_t> history;       // this module's string bytes

  uint8_t At(uint32_t v) const {
    const std::string_view primer = GetStringPrimer();
    return v < primer.size() ? static_cast<uint8_t>(primer[v])
                             : history[v - primer.size()];
  }
  uint32_t End() const {
    return static_cast<uint32_t>(GetStringPrimer().size() + history.size());
  }
  StringMatchTable& Table() {
    if (!match) {
      // Private copy of the generated primed table (constant data, so no
      // shared or process-wide state).
      match.Make(bytes.arena);
      int bits = 0;
      const uint32_t* primed = GetPrimedStringMatchTable(&bits);
      std::memcpy(match->pos.data(), primed, sizeof(match->pos));
    }
    return *match;
  }
  // Earlier position that followed the two bytes before End() (verified,
  // since the table is hashed); 0 if none.
  uint32_t Lookup() {
    const uint32_t v = End();
    if (v < 2) {
      return 0;
    }
    const uint8_t c0 = At(v - 2);
    const uint8_t c1 = At(v - 1);
    const uint32_t slot = Table().pos[StringMatchHash(c0, c1)];
    return slot >= 2 && slot < v && At(slot - 2) == c0 && At(slot - 1) == c1
               ? slot
               : 0;
  }
  // Appends b at End(), pointing its context's table slot at it.
  void Push(uint8_t b) {
    const uint32_t v = End();
    if (v >= 2) {
      Table().pos[StringMatchHash(At(v - 2), At(v - 1))] = v;
    }
    history.push_back(b);
  }
};

struct ModelState {
  // Owns every lazily created context (ArenaPtr members below); declared
  // first so it is destroyed last.
  Arena arena;

  ModelState()
      : opcode_index_trees(
            static_cast<size_t>(Section::kCount) *
            (static_cast<size_t>(NumInstructionClasses()) + 1)),
        opcode_contexts(NumCoreInstructions() + 1),
        inst_contexts(NumCoreInstructions()),
        class_count_models(static_cast<size_t>(NumInstructionClasses()) + 1),
        type_in_list(std::make_unique<std::vector<Prob>>(
            static_cast<size_t>(NumInstructionClasses()) + 1)),
        type_rank(std::make_unique<std::vector<BitTree<kRankTreeBits>>>(
            static_cast<size_t>(NumInstructionClasses()) + 1)),
        type_escape(static_cast<size_t>(NumInstructionClasses()) + 1),
        value_enum_ordinal(NumOperandKinds()),
        bit_enums(NumOperandKinds()),
        integer_values(
            (static_cast<size_t>(NumInstructionClasses()) + 1) * 9),
        ext_inst_nums(NumExtInstSets()),
        strings(std::make_unique<StringModel>(
            static_cast<size_t>(NumInstructionClasses()) + 1)) {
    size_t value_enum_kinds = 0;
    for (uint16_t k = 0; k < NumOperandKinds(); ++k) {
      if (GetOperandKind(k).category == OperandCategory::kValueEnum) {
        value_enum_ordinal[k] = static_cast<uint16_t>(value_enum_kinds++);
      }
    }
    value_enums.resize(value_enum_kinds * 16);
    id_slots.reserve(32);  // the corpus averages ~37 slots per module
    // Grammar cold-start priors for the header (see docs/development/ratio.md
    // item 7): versions the grammar defines, registered tool IDs, and the
    // spec-valid side of the header flags.
    LoadPrior(version_major.tables[0], GetVersionMajorPrior());
    LoadPrior(version_minor.tables[0], GetVersionMinorPrior());
    LoadPrior(generator_id.root, GetIndexRootPrior(NumRegisteredGenerators()));
    version_canonical = ValidFlag();
    schema_zero = ValidFlag();
    bound_matches = ValidFlag();
    var_sc_matches = ValidFlag();
    string_canonical = ValidFlag();
    known_opcode.fill(ValidFlag());
  }

  // Must run before any inst_group access for inst_index;
  // CodeModule calls it once per instruction with a known descriptor.
  void EnsureInstContext(uint16_t inst_index) {
    (void)EnsureContext(arena, inst_contexts[inst_index]);
  }
  InstGroup& inst_group(uint16_t inst_index) {
    return inst_contexts[inst_index]->group;
  }

  // The (instruction, slot) ID model behind an InstGroup::id_slot entry,
  // created on first use. A module has far fewer than 65535 such slots
  // (~875 instructions x 16 slots); past that, slots share the last model.
  IdSlotModel& IdSlot(uint16_t& handle) {
    if (handle == 0) {
      if (id_slots.size() < 0xffffu) {
        id_slots.emplace_back();
      }
      handle = static_cast<uint16_t>(id_slots.size());
    }
    return id_slots[handle - 1u];
  }
  std::vector<IdSlotModel> id_slots;

  Tracker tracker;
  // Result Type of the ID operand CodeIdRef just coded (from its OnIdUse).
  uint32_t last_id_operand_type = 0;
  // Per-instruction ID operand tracking for CodeIdRef.
  // kUnresolvedList is distinct from 0 (none), 1..N (typed lists), and
  // Tracker::kRecentTypesList (~0u).
  static constexpr uint32_t kUnresolvedList = ~uint32_t{1};
  uint32_t cur_op0_list = 0;
  uint32_t cur_prev_op_list = 0;
  uint32_t cur_prev_res_id = 0;
  uint32_t cur_prev_res_list = kUnresolvedList;
  Prob swapped_flag;
  Prob version_canonical;
  BitTree<4> version_major;
  BitTree<4> version_minor;
  ByteModel generator_bytes;
  Prob generator_registered = ValidFlag();
  LazyLeafTree8<kSymbolRateCap> generator_id{};
  ArenaPtr<UintModel> generator_id_escape;  // unregistered tools only
  Prob schema_zero;
  Prob bound_matches;
  Prob var_sc_matches;

  std::array<Prob, static_cast<size_t>(Section::kCount) * 2> known_opcode{};
  std::vector<ArenaPtr<OpcodeTree>> opcode_index_trees;
  ArenaPtr<BitTree<16>> unknown_opcode_tree;
  std::vector<ArenaPtr<OpcodeCtx>> opcode_contexts;
  // Per-instruction contexts (operand caches + word-count model), created
  // zeroed on an instruction's first use: a module uses a few dozen of the
  // ~875 core instructions. One pointer table serves both; they are always
  // created together.
  std::vector<ArenaPtr<InstContext>> inst_contexts;
  // Word-count models are shared per instruction class (ClassBucket): one per
  // instruction was too sparse to learn — sharing compresses the corpus
  // 0.83% smaller and touches ~7 KB less per decode. The last word count
  // itself stays per instruction (InstGroup::last_word_count).
  Prob word_count_consistent = ValidFlag();
  std::vector<ArenaPtr<CountModel>> class_count_models;
  // Models on rare paths (corpus-measured: raw counts 1/1080 files, exact
  // bound mispredictions 27/1080, opaque words 4/1080) are allocated on
  // first use so typical decodes never pay for their construction.
  ArenaPtr<UintModel> raw_known_count;
  ArenaPtr<UintModel> raw_unknown_count;
  ArenaPtr<UintModel> bound_value;
  std::unique_ptr<std::array<ByteModel, 4>> operand_bytes;

  uint32_t prev_result_id = 0;
  std::array<Prob, 2> result_next{};
  Prob result_in_run;
  BitTree<kRankTreeBits> result_run_rank;
  Prob result_in_fwd;
  BitTree<kRankTreeBits> result_fwd_rank;
  std::array<UintModel, 4> result_delta;

  std::unique_ptr<std::vector<Prob>> type_in_list;
  std::unique_ptr<std::vector<BitTree<kRankTreeBits>>> type_rank;
  // Escape models allocate per slot on first escape: every module escapes
  // somewhere, but through only a handful of the 27/29 slots.
  std::vector<ArenaPtr<UintModel>> type_escape;

  // ID operands are first looked up in the recency list of the type the same
  // (instruction, slot) took last time (see CodeIdRef); hit and rank models
  // are per (instruction, slot) (InstGroup::id_slot).
  std::array<std::array<ArenaPtr<UintModel>, 9>, 3> id_escape{};

  // Value-enum contexts are indexed by (value-enum ordinal, slot): only 56 of
  // the 91 operand kinds are value enums, so indexing by raw kind wasted
  // ~40% of a pointer table zeroed every decode.
  std::vector<uint16_t> value_enum_ordinal;  // per operand kind
  std::vector<ArenaPtr<ValueEnumModel>> value_enums;
  std::vector<std::unique_ptr<BitEnumModel>> bit_enums;

  std::vector<ArenaPtr<UintModel>> integer_values;
  ArenaPtr<EnumLiteralCache> enum_literals;
  ArenaPtr<UintModel> context_number;
  ArenaPtr<UintModel> context_int;
  ArenaPtr<IntConstCache> int_const_cache;
  Prob float_zero;
  Prob float_sign;
  Prob float_mant_zero;
  ArenaPtr<UintModel> float_exp;
  ArenaPtr<UintModel> float_mant;
  Prob context_high_signext;
  ArenaPtr<UintModel> spec_constant_op;  // rare: 16/1080 files

  std::vector<ArenaPtr<ExtInstNumberModel>> ext_inst_nums;
  ArenaPtr<UintModel> ext_inst_unknown;  // rare: 4/1080 files

  Prob string_match;
  BitTree<6> string_rank;
  Prob string_canonical;
  std::unique_ptr<StringModel> strings;
  std::vector<std::string> recent_strings;

};

class EncodeIO {
 public:
  EncodeIO(const uint8_t* data, size_t size) {
    words_.resize(size / 4);
    for (size_t i = 0; i < words_.size(); ++i) {
      words_[i] = LoadLe32(data + i * 4);
    }
    swapped_ = !words_.empty() && words_[0] == kSpirvMagicSwapped;
    if (swapped_) {
      for (uint32_t& word : words_) {
        word = ByteSwap32(word);
      }
    }
  }

  bool SwappedHint() const { return swapped_; }
  uint32_t HeaderWord(size_t index) const { return words_[index]; }
  uint32_t BodyWord(size_t index) const { return words_[5 + index]; }
  const uint32_t* BodyWordPtr(size_t index) const {
    return words_.data() + 5 + index;
  }
  uint32_t BoundHint() const { return words_[3]; }
  void EmitHeaderWord(uint32_t word) {
    if (emitted_ != 3) {
      assert(word == words_[emitted_]);
    }
    ++emitted_;
  }
  void EmitBodyWord(uint32_t word) { assert(word == words_[emitted_++]); }
  void PatchBound(uint32_t word) { assert(word == words_[3]); }
  void PatchBodyWord(size_t index, uint32_t word) {
    assert(word == words_[5 + index]);
    (void)index;
    (void)word;
  }

 private:
  std::vector<uint32_t> words_;
  bool swapped_ = false;
  size_t emitted_ = 0;
};

class DecodeIO {
 public:
  DecodeIO(std::vector<uint8_t>& out, size_t expected_size) : out_(out) {
    // The module header has already validated this size.  Allocate it once so
    // the hot path can write each decoded word directly instead of growing the
    // vector one byte at a time.
    out_.resize(expected_size);
    data_ = out_.data();
  }

  bool SwappedHint() const { return false; }
  uint32_t HeaderWord(size_t) const { return 0; }
  uint32_t BodyWord(size_t) const { return 0; }
  const uint32_t* BodyWordPtr(size_t) const { return nullptr; }
  uint32_t BoundHint() const { return 0; }

  void SetSwapped(bool swapped) { swapped_ = swapped; }

  void EmitHeaderWord(uint32_t word) { Append(word); }
  ARIXV_FORCE_INLINE void EmitBodyWord(uint32_t word) { Append(word); }

  bool Complete() const { return !overflowed_ && write_pos_ == out_.size(); }

  void PatchBound(uint32_t word) {
    if (out_.size() < 16) {
      return;
    }
    if (swapped_) {
      word = ByteSwap32(word);
    }
    StoreLe32(word, data_ + 12);
  }

  // Overwrites a body word already emitted (a deferred Result Type).
  void PatchBodyWord(size_t index, uint32_t word) {
    const size_t at = (5 + index) * 4;
    if (at > write_pos_ || write_pos_ - at < 4) {
      return;  // never written (overflowed); the decode fails anyway
    }
    if (swapped_) {
      word = ByteSwap32(word);
    }
    StoreLe32(word, data_ + at);
  }

 private:
  ARIXV_FORCE_INLINE void Append(uint32_t word) {
    if (write_pos_ > out_.size() || out_.size() - write_pos_ < 4) {
      overflowed_ = true;
      return;
    }
    if (swapped_) {
      word = ByteSwap32(word);
    }
    StoreLe32(word, data_ + write_pos_);
    write_pos_ += 4;
  }

  std::vector<uint8_t>& out_;
  uint8_t* data_ = nullptr;
  size_t write_pos_ = 0;
  bool overflowed_ = false;
  bool swapped_ = false;
};

// A packed decision stream with no entropy coding. Keeping the BitEncoder /
// BitDecoder-shaped interface lets this exercise the exact same structural
// traversal as M2 while removing range arithmetic and adaptive updates from
// the hot path.
class RawResidualEncoder {
 public:
  static constexpr bool kEncoding = true;
  static constexpr bool kRawResidual = true;

  explicit RawResidualEncoder(std::vector<uint8_t>& out) : out_(out) {}

  bool CodeBit(Prob&, bool bit) {
    CodeBitsDirect(bit ? 1u : 0u, 1);
    return bit;
  }

  uint32_t CodeBitsDirect(uint32_t value, int nbits) {
    const int original_nbits = nbits;
    while (nbits > 0) {
      if (bit_offset_ == 0) {
        out_.push_back(0);
      }
      const int available = 8 - bit_offset_;
      const int take = std::min(nbits, available);
      const int source_shift = nbits - take;
      const uint32_t mask = (1u << take) - 1u;
      out_.back() |= static_cast<uint8_t>(
          ((value >> source_shift) & mask) << (available - take));
      bit_offset_ = (bit_offset_ + take) & 7;
      nbits -= take;
    }
    if (original_nbits == 32) {
      return value;
    }
    return value & ((1u << original_nbits) - 1u);
  }

  uint32_t CodeUint32(uint32_t value) {
    uint32_t remaining = value;
    while (remaining >= 0x80) {
      CodeBitsDirect((remaining & 0x7fu) | 0x80u, 8);
      remaining >>= 7;
    }
    CodeBitsDirect(remaining, 8);
    return value;
  }

  void SetCostSink(double*) {}
  void Flush() {}
  bool exhausted() const { return false; }

 private:
  std::vector<uint8_t>& out_;
  int bit_offset_ = 0;
};

class RawResidualDecoder {
 public:
  static constexpr bool kEncoding = false;
  static constexpr bool kRawResidual = true;

  RawResidualDecoder(const uint8_t* data, size_t size)
      : data_(data), size_(size) {}

  bool CodeBit(Prob&, bool) {
    return CodeBitsDirect(0, 1) != 0;
  }

  uint32_t CodeBitsDirect(uint32_t, int nbits) {
    uint32_t value = 0;
    while (nbits > 0) {
      const size_t byte = bit_pos_ >> 3;
      if (byte >= size_) {
        exhausted_ = true;
        // nbits can still be 32 here (a full-width read that exhausts before
        // consuming any bits), where value is necessarily 0 and a 32-bit
        // shift would be undefined.
        value = nbits < 32 ? value << nbits : 0;
        break;
      }
      const int offset = static_cast<int>(bit_pos_ & 7u);
      const int available = 8 - offset;
      const int take = std::min(nbits, available);
      const int source_shift = available - take;
      const uint32_t mask = (1u << take) - 1u;
      value = (value << take) | ((data_[byte] >> source_shift) & mask);
      bit_pos_ += static_cast<size_t>(take);
      nbits -= take;
    }
    return value;
  }

  uint32_t CodeUint32(uint32_t) {
    uint32_t value = 0;
    for (int byte_index = 0; byte_index < 5; ++byte_index) {
      const uint32_t byte = CodeBitsDirect(0, 8);
      if (byte_index == 4 && (byte & 0xf0u) != 0) {
        malformed_ = true;
      }
      value |= (byte & 0x7fu) << (byte_index * 7);
      if ((byte & 0x80u) == 0) {
        return value;
      }
    }
    malformed_ = true;
    return value;
  }

  void SetCostSink(double*) {}
  bool exhausted() const { return exhausted_; }

  bool AtEndWithZeroPadding() const {
    if (exhausted_ || malformed_) {
      return false;
    }
    const size_t consumed_bytes = (bit_pos_ + 7u) >> 3;
    if (consumed_bytes != size_) {
      return false;
    }
    const size_t remaining = (8u - (bit_pos_ & 7u)) & 7u;
    if (remaining == 0) {
      return true;
    }
    const uint8_t padding_mask =
        static_cast<uint8_t>((1u << remaining) - 1u);
    return (data_[size_ - 1] & padding_mask) == 0;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t bit_pos_ = 0;
  bool exhausted_ = false;
  bool malformed_ = false;
};

template <class Bits>
uint32_t CodeDirect32(Bits& bits, uint32_t hint) {
  uint32_t value = 0;
  for (int shift = 24; shift >= 0; shift -= 8) {
    value = (value << 8) |
            bits.CodeBitsDirect((hint >> shift) & 0xffu, 8);
  }
  return Bits::kEncoding ? hint : value;
}

// Header word 2: the tool's own version (low 16 bits, two order-1 bytes),
// then its tool ID (high 16 bits) as an index into the registry's
// contiguous IDs, escaping for unregistered ones.
template <class Bits>
uint32_t CodeGenerator(Bits& bits, ModelState& model, uint32_t hint) {
  uint8_t prev = 0;
  uint32_t out = 0;
  for (int byte = 0; byte < 2; ++byte) {
    const uint8_t b = CodeByte(
        bits, model.generator_bytes, prev,
        static_cast<uint8_t>((hint >> (byte * 8)) & 0xffu));
    out |= static_cast<uint32_t>(b) << (byte * 8);
    prev = b;
  }
  const uint32_t registered = NumRegisteredGenerators();
  uint32_t id = hint >> 16;
  if (bits.CodeBit(model.generator_registered, id < registered)) {
    id = model.generator_id.CodeValue(bits, model.arena, id,
                                      IndexLeafPrior{registered});
  } else {
    id = CodeUint(bits, model.arena,
                  EnsureUint(model.arena, model.generator_id_escape), id) &
         0xffffu;
  }
  return Bits::kEncoding ? hint : (out | (id << 16));
}

template <class Bits>
uint32_t CodeGenericOperandWord(Bits& bits, ModelState& model,
                                std::array<uint8_t, 4>& prev_bytes,
                                uint32_t hint) {
  if constexpr (Bits::kRawResidual) {
    (void)model;
    (void)prev_bytes;
    return bits.CodeBitsDirect(hint, 32);
  }
  if (!model.operand_bytes) {
    // Created on first use, identically on both sides.
    model.operand_bytes = std::make_unique<std::array<ByteModel, 4>>();
  }
  uint32_t out = 0;
  for (int byte = 0; byte < 4; ++byte) {
    const uint8_t b =
        CodeByte(bits, (*model.operand_bytes)[byte], prev_bytes[byte],
                 static_cast<uint8_t>((hint >> (byte * 8)) & 0xffu));
    out |= static_cast<uint32_t>(b) << (byte * 8);
    prev_bytes[byte] = b;
  }
  return Bits::kEncoding ? hint : out;
}

uint32_t ZigZag32(uint32_t delta) {
  return (delta << 1) ^ (0u - (delta >> 31));
}

uint32_t UnZigZag32(uint32_t value) {
  return (value >> 1) ^ (0u - (value & 1u));
}

template <class Bits>
uint32_t CodeDelta(Bits& bits, Arena& arena, UintModel& model, uint32_t value,
                   uint32_t base) {
  const uint32_t hint = ZigZag32(value - base);
  const uint32_t coded = CodeUint(bits, arena, model, hint);
  return Bits::kEncoding ? value : base + UnZigZag32(coded);
}

uint8_t ResultClassBucket(const InstructionDesc* desc) {
  return desc ? desc->result_bucket : 3;
}

size_t ClassBucket(const InstructionDesc* desc) {
  return desc ? desc->instruction_class : NumInstructionClasses();
}

int IdCategoryBucket(OperandCategory category) {
  if (category == OperandCategory::kIdScope) {
    return 1;
  }
  if (category == OperandCategory::kIdMemorySemantics) {
    return 2;
  }
  return 0;
}

uint32_t Slot8(uint8_t slot) {
  return slot < 8 ? slot : 8;
}

size_t Slot16(uint8_t slot) {
  return slot < 15 ? slot : 15;
}

// The previous instruction's core-table index, remembered by the tracker
// (this used to binary-search the opcode table on every instruction: ~3.8%
// of decode instructions).
uint16_t PreviousOpcodeKey(const Tracker& tracker) {
  const uint16_t sentinel = NumCoreInstructions();
  if (tracker.prev_opcode_class() == 0xFF) {
    return sentinel;
  }
  const uint16_t index = tracker.prev_instruction_index();
  return index < sentinel ? index : sentinel;
}

bool IsSingleBit(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

uint32_t WordFromBytes(const std::string& bytes, size_t word_index) {
  uint32_t word = 0;
  for (int i = 0; i < 4; ++i) {
    const size_t index = word_index * 4 + static_cast<size_t>(i);
    const uint8_t b =
        index < bytes.size() ? static_cast<uint8_t>(bytes[index]) : 0;
    word |= static_cast<uint32_t>(b) << (i * 8);
  }
  return word;
}

std::string RawStringBytes(const uint32_t* words, uint32_t span) {
  std::string out;
  out.reserve(static_cast<size_t>(span) * 4);
  for (uint32_t i = 0; i < span; ++i) {
    const uint32_t word = words[i];
    for (int byte = 0; byte < 4; ++byte) {
      out.push_back(static_cast<char>((word >> (byte * 8)) & 0xffu));
    }
  }
  return out;
}

bool IsCanonicalStringBytes(const std::string& bytes) {
  bool seen_nul = false;
  for (unsigned char ch : bytes) {
    if (seen_nul && ch != 0) {
      return false;
    }
    if (ch == 0) {
      seen_nul = true;
    }
  }
  return seen_nul;
}

int FindRecentString(const ModelState& model, const std::string& raw) {
  for (size_t i = 0; i < model.recent_strings.size(); ++i) {
    if (model.recent_strings[i] == raw) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void TouchString(ModelState& model, const std::string& raw) {
  const int found = FindRecentString(model, raw);
  if (found == 0) {
    return;
  }
  if (found > 0) {
    // Move-to-front by rotating the [0, found] prefix; this moves the existing
    // strings rather than copying `raw` in and shifting the whole vector.
    std::rotate(model.recent_strings.begin(),
                model.recent_strings.begin() + found,
                model.recent_strings.begin() + found + 1);
    return;
  }
  model.recent_strings.insert(model.recent_strings.begin(), raw);
  if (model.recent_strings.size() > 64) {
    model.recent_strings.pop_back();
  }
}

template <class Bits>
uint32_t CodeRankedId(Bits& bits, Prob& flag, BitTree<kRankTreeBits>& tree,
                      Arena& arena, ArenaPtr<UintModel>& escape,
                      const RecencyList& list, uint32_t hint, uint32_t base) {
  const int rank = Bits::kEncoding ? list.Find(hint) : -1;
  const bool in_list = rank >= 0 && rank < (1 << kRankTreeBits);
  if (bits.CodeBit(flag, in_list)) {
    const uint32_t coded_rank =
        tree.CodeValue(bits, in_list ? static_cast<uint32_t>(rank) : 0);
    if (!Bits::kEncoding && coded_rank < static_cast<uint32_t>(list.size())) {
      return list.at(static_cast<int>(coded_rank));
    }
    return Bits::kEncoding ? hint : 0;
  }
  if constexpr (Bits::kRawResidual) {
    const uint32_t delta = Bits::kEncoding ? ZigZag32(hint - base) : 0;
    const uint32_t coded = bits.CodeUint32(delta);
    return Bits::kEncoding ? hint : base + UnZigZag32(coded);
  }
  return CodeDelta(bits, arena, EnsureUint(arena, escape), hint, base);
}


template <class Bits>
uint32_t CodeResultId(Bits& bits, ModelState& model,
                      const InstructionDesc* desc, uint32_t hint) {
  // Compilers skip IDs they already used (constants and types created while
  // a function body is built), so predict the smallest never-seen ID above
  // the previous result rather than just previous + 1.
  const Tracker& tracker = model.tracker;
  uint32_t predicted = model.prev_result_id + 1;
  for (int step = 0; step < kResultScanCap; ++step) {
    const uint32_t candidate = model.prev_result_id + 1 + static_cast<uint32_t>(step);
    if (candidate == 0) {
      break;  // wrapped
    }
    if (tracker.Find(candidate) == nullptr) {
      predicted = candidate;
      break;
    }
  }
  const bool sequential = Bits::kEncoding && hint == predicted;
  const size_t ctx = tracker.in_function() ? 1u : 0u;
  uint32_t id = predicted;
  if (!bits.CodeBit(model.result_next[ctx], sequential)) {
    // Then: continues one of the recent runs (interleaved ID allocation).
    const RecencyList& runs = tracker.result_runs();
    const int run = Bits::kEncoding ? runs.Find(hint - 1) : -1;
    const RecencyList& fwd = tracker.forward_refs();
    if (bits.CodeBit(model.result_in_run, run >= 0)) {
      const uint32_t coded_run = model.result_run_rank.CodeValue(
          bits, run >= 0 ? static_cast<uint32_t>(run) : 0u);
      if (!Bits::kEncoding) {
        id = coded_run < static_cast<uint32_t>(runs.size())
                 ? runs.at(static_cast<int>(coded_run)) + 1
                 : 0;
      } else {
        id = hint;
      }
    } else {
      const int rank = Bits::kEncoding ? fwd.Find(hint) : -1;
      const bool in_fwd = rank >= 0 && rank < (1 << kRankTreeBits);
      if (bits.CodeBit(model.result_in_fwd, in_fwd)) {
        const uint32_t coded_rank =
            model.result_fwd_rank.CodeValue(bits,
                                            in_fwd ? static_cast<uint32_t>(rank)
                                                   : 0);
        if (!Bits::kEncoding && coded_rank < static_cast<uint32_t>(fwd.size())) {
          id = fwd.at(static_cast<int>(coded_rank));
        } else {
          id = Bits::kEncoding ? hint : 0;
        }
      } else {
        id = CodeDelta(bits, model.arena,
                       model.result_delta[ResultClassBucket(desc)], hint,
                       predicted);
      }
    }
  }
  model.tracker.OnResultId(id);
  model.prev_result_id = id;
  return Bits::kEncoding ? hint : id;
}

template <class Bits>
uint32_t CodeResultType(Bits& bits, ModelState& model,
                        const InstructionDesc* desc, uint32_t hint) {
  const size_t cls = ClassBucket(desc);
  uint32_t id = CodeRankedId(bits, (*model.type_in_list)[cls],
                             (*model.type_rank)[cls],
                             model.arena, model.type_escape[cls],
                             model.tracker.recent_types(), hint,
                             model.prev_result_id);
  model.tracker.OnResultTypeUse(id);
  return Bits::kEncoding ? hint : id;
}

// Result Type coded after the instruction's operands. Candidates combine each
// ID operand's own Result Type and element type (or ElementTypeOf(id) for
// type-ID operands like OpFunction's function type) with pure SPIR-V OpType*
// type-algebra derivations (OpFunctionParameter, OpAccessChain pointer walk,
// OpCompositeExtract literal walk, OpVectorShuffle vector size,
// OpCompositeConstruct/OpConstantComposite vector/matrix/array/struct, scalar
// and vector conversions, comparisons bool/bvec, OpVectorTimesMatrix /
// OpMatrixTimesMatrix, and image/sampled-image types), deduplicated and built
// lazily up to `limit` on decode. A miss (choice 7, or any index past the
// candidates) falls back to ranked recent-types coding.
constexpr int kMaxResultTypeCandidates = 7;

// Non-template so the type-algebra candidate builder is compiled once instead
// of once per coder template (keeps CodeModule within GCC's inlining budget).
int BuildDeferredResultTypeCandidates(
    const Tracker& tracker, const uint32_t* id_operands,
    const uint32_t* operand_types, int num_ids, int total_ids,
    const uint32_t* inst_words, uint32_t word_count,
    uint32_t first_literal_word, uint32_t num_literal_ints, uint32_t* cand,
    int limit) {
  int count = 0;
  const auto add = [cand, &count, limit](uint32_t type) {
    if (type == 0 || count >= limit) {
      return;
    }
    for (int i = 0; i < count; ++i) {
      if (cand[i] == type) {
        return;
      }
    }
    cand[count++] = type;
  };

  if (num_ids == 0) {
    add(tracker.CurrentFunctionParamType());
    add(tracker.bool_type_id());
    return count;
  }

  const Tracker::IdInfo* type0_info = nullptr;
  bool type0_fetched = false;
  const auto get_type0 = [&]() -> const Tracker::IdInfo* {
    if (!type0_fetched) {
      type0_fetched = true;
      if (operand_types[0] != 0) {
        const Tracker::IdInfo* info = tracker.Find(operand_types[0]);
        if (info && info->kind == Tracker::IdKind::kType) {
          type0_info = info;
        }
      }
    }
    return type0_info;
  };

  // Shape-specific derivations with literal indices (OpCompositeExtract and
  // OpVectorShuffle) run first so their exact result hits candidate 0.
  if (num_literal_ints >= 1) {
    if (total_ids == 1 && first_literal_word + num_literal_ints <= word_count) {
      const Tracker::IdInfo* t0 = get_type0();
      if (t0 != nullptr) {
        if (num_literal_ints == 1) {
          if (t0->type_class == Tracker::TypeClass::kStruct) {
            add(tracker.StructMemberType(*t0, inst_words[first_literal_word]));
          } else if (t0->type_class == Tracker::TypeClass::kVector ||
                     t0->type_class == Tracker::TypeClass::kMatrix ||
                     t0->type_class == Tracker::TypeClass::kArray ||
                     t0->type_class == Tracker::TypeClass::kRuntimeArray) {
            add(t0->element_type);
          }
        } else {
          add(tracker.WalkCompositeByLiterals(
              operand_types[0], inst_words + first_literal_word,
              num_literal_ints));
        }
      }
    } else if (total_ids == 2 && num_literal_ints >= 2) {
      const Tracker::IdInfo* t0 = get_type0();
      if (t0 != nullptr && t0->type_class == Tracker::TypeClass::kVector) {
        add(tracker.VectorTypeOf(t0->element_type, num_literal_ints));
      }
    }
    if (count >= limit) {
      return count;
    }
  }

  // Operand 0: its Result Type first (keeps the decode choice-0 fast path
  // hash-lookup-free for arithmetic/value ops), then access-chain pointer walk
  // (when operand 0 is a pointer with ID indices) and element type (or
  // ElementTypeOf(id) when operand 0 is itself a type ID, e.g. OpFunction).
  if (operand_types[0] != 0) {
    add(operand_types[0]);
    if (count >= limit) {
      return count;
    }
    const Tracker::IdInfo* t0 = get_type0();
    if (t0 != nullptr) {
      if (total_ids >= 2 && num_literal_ints == 0 &&
          t0->type_class == Tracker::TypeClass::kPointer &&
          word_count == static_cast<uint32_t>(total_ids) + 3u) {
        const uint32_t leaf = tracker.WalkCompositeByIds(
            t0->element_type, inst_words + 4, word_count - 4u);
        if (leaf != 0) {
          add(tracker.PointerTypeOf(t0->width, leaf));
          if (count >= limit) {
            return count;
          }
        }
      }
      add(t0->element_type);
      if (count >= limit) {
        return count;
      }
      // Image / SampledImage result types.
      if (t0->type_class == Tracker::TypeClass::kSampledImage) {
        const uint32_t samp = tracker.ElementTypeOf(t0->element_type);
        if (samp != 0) {
          add(tracker.VectorTypeOf(samp, 4));
          add(samp);
        }
      } else if (t0->type_class == Tracker::TypeClass::kImage) {
        if (total_ids == 2 && num_literal_ints == 0) {
          add(Tracker::SampledImageTypeOf(*t0));
        }
        const uint32_t samp = t0->element_type;
        if (samp != 0) {
          add(tracker.VectorTypeOf(samp, 4));
        }
      } else if (total_ids == 2 && num_ids >= 2 && num_literal_ints == 0 &&
                 operand_types[1] != 0 &&
                 (t0->type_class == Tracker::TypeClass::kVector ||
                  t0->type_class == Tracker::TypeClass::kMatrix)) {
        // OpVectorTimesMatrix / OpMatrixTimesMatrix.
        const Tracker::IdInfo* t1 = tracker.Find(operand_types[1]);
        if (t1 && t1->kind == Tracker::IdKind::kType &&
            t1->type_class == Tracker::TypeClass::kMatrix) {
          if (t0->type_class == Tracker::TypeClass::kVector) {
            add(tracker.VectorTypeOf(t0->element_type, t1->width));
          } else {
            add(tracker.MatrixTypeOf(t0->element_type, t1->width));
          }
        }
      } else if (total_ids == 1 && num_literal_ints == 0) {
        // Unary scalar / vector conversions (OpBitcast, OpConvert*, Op*Convert).
        const Tracker::IdInfo* base = nullptr;
        uint32_t vec_len = 0;
        if (t0->type_class == Tracker::TypeClass::kFloat ||
            t0->type_class == Tracker::TypeClass::kInt) {
          base = t0;
        } else if (t0->type_class == Tracker::TypeClass::kVector) {
          const Tracker::IdInfo* elem = tracker.Find(t0->element_type);
          if (elem && elem->kind == Tracker::IdKind::kType &&
              (elem->type_class == Tracker::TypeClass::kFloat ||
               elem->type_class == Tracker::TypeClass::kInt)) {
            base = elem;
            vec_len = t0->width;
          }
        }
        if (base != nullptr) {
          const auto add_peer = [&](uint32_t scalar_tid) {
            if (scalar_tid == 0) {
              return;
            }
            add(vec_len == 0 ? scalar_tid
                             : tracker.VectorTypeOf(scalar_tid, vec_len));
          };
          const uint32_t w = base->width;
          if (base->type_class == Tracker::TypeClass::kInt) {
            const int is_signed = base->aux_flag != 0 ? 1 : 0;
            add_peer(tracker.ScalarTypeOf(2, w));
            add_peer(tracker.ScalarTypeOf(1 - is_signed, w));
            if (w == 32) {
              add_peer(tracker.ScalarTypeOf(is_signed, 16));
              add_peer(tracker.ScalarTypeOf(is_signed, 64));
            } else {
              add_peer(tracker.ScalarTypeOf(is_signed, 32));
            }
          } else {
            add_peer(tracker.ScalarTypeOf(1, w));
            add_peer(tracker.ScalarTypeOf(0, w));
            if (w == 32) {
              add_peer(tracker.ScalarTypeOf(2, 16));
              add_peer(tracker.ScalarTypeOf(2, 64));
            } else {
              add_peer(tracker.ScalarTypeOf(2, 32));
            }
          }
        }
      }
    }
  } else {
    add(tracker.ElementTypeOf(id_operands[0]));
  }

  // Remaining ID operands (i >= 1).
  for (int i = 1; i < num_ids && count < limit; ++i) {
    const uint32_t type = operand_types[i];
    if (type != 0) {
      add(type);
      if (count < limit) {
        add(tracker.ElementTypeOf(type));
      }
    } else {
      add(tracker.ElementTypeOf(id_operands[i]));
    }
  }

  // Composite construction (vector, vector concat, matrix, array, struct).
  if (count < limit && num_literal_ints == 0) {
    if (total_ids >= 2) {
      const Tracker::IdInfo* t0 = get_type0();
      if (t0 != nullptr) {
        const uint32_t type0 = operand_types[0];
        const uint32_t n = static_cast<uint32_t>(total_ids);
        const Tracker::TypeClass tc = t0->type_class;
        if (tc == Tracker::TypeClass::kFloat ||
            tc == Tracker::TypeClass::kInt ||
            tc == Tracker::TypeClass::kBool) {
          add(tracker.VectorTypeOf(type0, n));
          if (total_ids == num_ids) {
            uint32_t total_comp = 1;
            bool has_vec = false;
            bool ok = true;
            for (int i = 1; i < num_ids && ok; ++i) {
              const uint32_t ti = operand_types[i];
              if (ti == type0) {
                ++total_comp;
              } else {
                const Tracker::IdInfo* inf = tracker.Find(ti);
                if (inf && inf->kind == Tracker::IdKind::kType &&
                    inf->type_class == Tracker::TypeClass::kVector &&
                    inf->element_type == type0) {
                  total_comp += inf->width;
                  has_vec = true;
                } else {
                  ok = false;
                }
              }
            }
            if (ok && has_vec) {
              add(tracker.VectorTypeOf(type0, total_comp));
            }
          }
          add(tracker.ArrayTypeOf(type0, n));
        } else if (tc == Tracker::TypeClass::kVector) {
          add(tracker.MatrixTypeOf(type0, n));
          if (total_ids == num_ids) {
            const uint32_t elem = t0->element_type;
            uint32_t total_comp = t0->width;
            bool ok = elem != 0 && total_comp > 0;
            for (int i = 1; i < num_ids && ok; ++i) {
              const uint32_t ti = operand_types[i];
              if (ti == elem) {
                ++total_comp;
              } else {
                const Tracker::IdInfo* inf = tracker.Find(ti);
                if (inf && inf->kind == Tracker::IdKind::kType &&
                    inf->type_class == Tracker::TypeClass::kVector &&
                    inf->element_type == elem) {
                  total_comp += inf->width;
                } else {
                  ok = false;
                }
              }
            }
            if (ok && total_comp != t0->width) {
              add(tracker.VectorTypeOf(elem, total_comp));
            }
          }
          add(tracker.ArrayTypeOf(type0, n));
        } else {
          add(tracker.ArrayTypeOf(type0, n));
        }
      }
    }
    if (count < limit && total_ids >= 1 && total_ids == num_ids &&
        operand_types[0] != 0) {
      add(tracker.StructTypeOf(operand_types, static_cast<uint32_t>(num_ids)));
    }
  }

  // Comparisons and relational ops -> matching bvecN or scalar bool.
  if (count < limit) {
    const uint32_t bool_tid = tracker.bool_type_id();
    if (bool_tid != 0) {
      const Tracker::IdInfo* t0 = get_type0();
      if (t0 != nullptr && t0->type_class == Tracker::TypeClass::kVector) {
        add(tracker.VectorTypeOf(bool_tid, t0->width));
        add(bool_tid);
      } else {
        add(bool_tid);
      }
    }
  }
  return count;
}

template <class Bits>
uint32_t CodeDeferredResultType(Bits& bits, ModelState& model,
                                const InstructionDesc* desc,
                                uint16_t inst_index,
                                const uint32_t* id_operands,
                                const uint32_t* operand_types,
                                int num_ids, int total_ids,
                                const uint32_t* inst_words, uint32_t word_count,
                                uint32_t first_literal_word,
                                uint32_t num_literal_ints, uint32_t hint) {
  uint32_t cand[kMaxResultTypeCandidates];
  uint32_t choice = kMaxResultTypeCandidates;
  if constexpr (Bits::kEncoding) {
    if (num_ids > 0 && num_literal_ints == 0 && operand_types[0] != 0 &&
        operand_types[0] == hint) {
      cand[0] = hint;
      choice = 0;
    } else {
      const int count = BuildDeferredResultTypeCandidates(
          model.tracker, id_operands, operand_types, num_ids, total_ids,
          inst_words, word_count, first_literal_word, num_literal_ints, cand,
          kMaxResultTypeCandidates);
      for (int i = 0; i < count; ++i) {
        if (cand[i] == hint) {
          choice = static_cast<uint32_t>(i);
          break;
        }
      }
    }
  }
  // (A "same choice as last time" flag in front of this symbol ran ~1% fewer
  // instructions but was slower in wall time and cost 0.25% ratio.)
  choice = model.inst_group(inst_index).result_type_choice.CodeValue(bits,
                                                                   choice);
  if (choice < static_cast<uint32_t>(kMaxResultTypeCandidates)) {
    if constexpr (Bits::kEncoding) {
      model.tracker.OnResultTypeUse(cand[choice]);
      return hint;
    } else {
      // Fast path: when there are no literal indices and operand 0 has a known
      // Result Type, candidate 0 is always operand_types[0] (no hash lookup).
      if (choice == 0 && num_ids > 0 && num_literal_ints == 0 &&
          operand_types[0] != 0) {
        model.tracker.OnResultTypeUse(operand_types[0]);
        return operand_types[0];
      }
      if (BuildDeferredResultTypeCandidates(
              model.tracker, id_operands, operand_types, num_ids, total_ids,
              inst_words, word_count, first_literal_word, num_literal_ints,
              cand, static_cast<int>(choice) + 1) > static_cast<int>(choice)) {
        model.tracker.OnResultTypeUse(cand[choice]);
        return cand[choice];
      }
    }
  }
  return CodeResultType(bits, model, desc, hint);
}

// Recency list of values of type_id's element type (pointee, component,
// element, return type), or 0 if none.
ARIXV_NOINLINE uint32_t ElemValuesList(const Tracker& tracker,
                                       uint32_t type_id) {
  if (type_id == 0) {
    return 0u;
  }
  const uint32_t elem_type = tracker.ElementTypeOf(type_id);
  if (elem_type == 0) {
    return 0u;
  }
  const Tracker::IdInfo* elem_info = tracker.Find(elem_type);
  return elem_info != nullptr ? elem_info->values_list : 0u;
}

ARIXV_NOINLINE uint32_t PrevResListSlow(ModelState& model) {
  const Tracker::IdInfo* pr = model.tracker.Find(model.cur_prev_res_id);
  model.cur_prev_res_list = pr != nullptr ? pr->type_list : 0u;
  return model.cur_prev_res_list;
}

// Lazily resolves the typed recency list of the immediately preceding
// instruction's result ID (within a basic block), caching it for the
// current instruction.
inline uint32_t PrevResList(ModelState& model) {
  if (model.cur_prev_res_id == 0u) {
    return 0u;
  }
  return model.cur_prev_res_list != ModelState::kUnresolvedList
             ? model.cur_prev_res_list
             : PrevResListSlow(model);
}

// Cold-start typed-list guess on a slot's very first occurrence in a module
// (when no per-slot history exists yet), purely from structural SPIR-V type
// relationships of earlier ID operands in the same instruction or the
// preceding in-block result.
ARIXV_NOINLINE uint32_t InitialIdListForSlot(ModelState& model, int id_idx,
                                             uint32_t op0_type) {
  if (id_idx >= 2) {
    return model.cur_prev_op_list;
  }
  if (op0_type == 0) {
    return PrevResList(model);
  }
  const Tracker::IdInfo* ti = model.tracker.Find(op0_type);
  if (ti == nullptr) {
    return 0u;
  }
  switch (ti->type_class) {
    case Tracker::TypeClass::kInt:
    case Tracker::TypeClass::kFloat:
    case Tracker::TypeClass::kVector:
    case Tracker::TypeClass::kMatrix:
      return model.cur_op0_list;
    case Tracker::TypeClass::kPointer:
      return ElemValuesList(model.tracker, op0_type);
    default:
      return 0u;
  }
}

// Cold path: updates bits 4-5 (2-bit hysteresis counter in 0..2) for
// id_idx == 0 to choose between own_last (cnt < 2) and prev_res_list (cnt == 2)
// without touching bits 1-3 (which may be shared on variadic slots).
ARIXV_NOINLINE uint8_t UpdateOp0ListSource(ModelState& model, uint32_t own_last,
                                           uint32_t actual_list,
                                           uint8_t state) {
  const uint32_t prl = PrevResList(model);
  uint32_t cnt = (state >> 4) & 3u;
  if (prl != 0u && prl != own_last) {
    if (actual_list == prl && cnt < 2u) {
      ++cnt;
    } else if (actual_list == own_last && cnt > 0u) {
      --cnt;
    }
  }
  return static_cast<uint8_t>((state & ~0x30u) | (cnt << 4) | 1u);
}

// Cold path: updates bits 1-3 (adaptive list-source mode) for id_idx > 0 when
// the actual ID's list differed from the chosen list (or on cold start),
// preserving bits 4-5 for variadic slots shared with id_idx == 0.
ARIXV_NOINLINE uint8_t UpdateGt0ListSource(ModelState& model, uint32_t own_last,
                                           uint32_t prev_op_type,
                                           uint32_t actual_list,
                                           uint8_t state) {
  int winner = -1;
  if (model.cur_prev_op_list == actual_list) {
    winner = 2;
  } else if (model.cur_op0_list == actual_list) {
    winner = 1;
  } else if (ElemValuesList(model.tracker, prev_op_type) == actual_list) {
    winner = 3;
  } else if (own_last == actual_list) {
    winner = 0;
  } else if (PrevResList(model) == actual_list) {
    winner = 4;
  }
  if (winner >= 0) {
    return static_cast<uint8_t>((state & ~0x0eu) | (winner << 1) | 1u);
  }
  return static_cast<uint8_t>(state | 1u);
}

template <class Bits>
uint32_t CodeIdRef(Bits& bits, ModelState& model, uint16_t inst_index,
                   const OperandCursor::Field& field, uint32_t hint,
                   int id_idx, uint32_t op0_type) {
  const int bucket = IdCategoryBucket(field.category);
  const uint32_t slot = Slot8(field.slot);
  InstGroup& group = model.inst_group(inst_index);
  const size_t key = Slot16(field.slot);
  IdSlotModel& own = model.IdSlot(group.id_slot[key]);
  const uint8_t state = group.last_id_state[key];
  const bool have_last = (state & 1u) != 0;
  const uint32_t base = have_last ? group.last_id[key] : model.prev_result_id;
  const uint32_t own_last = have_last ? group.last_id_list[key] : 0u;
  const uint32_t prev_op_type = model.last_id_operand_type;
  // First try an adaptively chosen typed recency list: for id_idx == 0 either
  // the slot's own last type list or the preceding in-block result's type list;
  // for later ID operands of the same instruction, own_last, op0's list,
  // prev_op's list, elem(prev_op)'s list (e.g. pointer -> pointee), or
  // prev_res_list. On a miss, fall back to the global recency list below.
  uint32_t chosen_list = own_last;
  if (id_idx == 0) {
    if (!have_last) {
      chosen_list = PrevResList(model);
    } else if ((state & 0x20u) != 0u) {
      const uint32_t prl = PrevResList(model);
      if (prl != 0u) {
        chosen_list = prl;
      }
    }
  } else {
    if (!have_last) {
      chosen_list = InitialIdListForSlot(model, id_idx, op0_type);
    } else {
      switch ((state >> 1) & 7u) {
        case 0:
          chosen_list = own_last;
          break;
        case 1:
          chosen_list = model.cur_op0_list;
          break;
        case 2:
          chosen_list = model.cur_prev_op_list;
          break;
        case 3:
          chosen_list = ElemValuesList(model.tracker, prev_op_type);
          break;
        case 4:
          chosen_list = PrevResList(model);
          break;
        default:
          chosen_list = own_last;
          break;
      }
      if (chosen_list == 0u) {
        chosen_list = own_last;
      }
    }
  }
  uint32_t id = 0;
  bool coded = false;
  const RecencyList* typed = model.tracker.TypedList(chosen_list);
  if (typed != nullptr && typed->size() > 0) {
    const int rank = Bits::kEncoding ? typed->Find(hint) : -1;
    if (bits.CodeBit(own.in_typed, rank >= 0)) {
      const uint32_t coded_rank = own.typed_rank.CodeValue(
          bits, rank >= 0 ? static_cast<uint32_t>(rank) : 0u);
      id = Bits::kEncoding ? hint
           : coded_rank < static_cast<uint32_t>(typed->size())
               ? typed->at(static_cast<int>(coded_rank))
               : 0u;
      coded = true;
    }
  }
  if (!coded) {
    id = CodeRankedId(bits, own.in_list, own.rank, model.arena,
                      model.id_escape[bucket][slot],
                      model.tracker.recent_ids(), hint, base);
  }
  group.last_id[key] = id;
  const Tracker::IdUse use = model.tracker.OnIdUse(id, /*touch_recent_ids=*/!coded);
  model.last_id_operand_type = use.result_type;
  uint8_t next_state = static_cast<uint8_t>(state | 1u);
  if (id_idx == 0) {
    if (!have_last) {
      next_state = static_cast<uint8_t>((state & ~0x30u) | 0x11u);
    } else if (use.type_list != 0u && model.cur_prev_res_id != 0u &&
               (use.type_list != chosen_list || ((state >> 4) & 3u) == 1u)) {
      next_state = UpdateOp0ListSource(model, own_last, use.type_list, state);
    }
    model.cur_op0_list = use.type_list;
  } else {
    if (use.type_list != 0u && (!have_last || use.type_list != chosen_list)) {
      next_state = UpdateGt0ListSource(model, own_last, prev_op_type,
                                       use.type_list, state);
    }
  }
  model.cur_prev_op_list = use.type_list;
  group.last_id_state[key] = next_state;
  group.last_id_list[key] = use.type_list;
  return Bits::kEncoding ? hint : id;
}

template <class Bits>
uint32_t CodeValueEnum(Bits& bits, ModelState& model,
                       const OperandCursor::Field& field, uint32_t hint) {
  const OperandKindDesc& kind = GetOperandKind(field.kind);
  const EnumerantDesc* enumerant =
      Bits::kEncoding ? FindEnumerant(kind, hint) : nullptr;
  uint32_t index = 0;
  bool known = false;
  if (enumerant && kind.num_enumerants <= 255) {
    for (uint16_t i = 0; i < kind.num_enumerants; ++i) {
      if (&GetEnumerant(kind, i) == enumerant) {
        index = i;
        known = true;
        break;
      }
    }
  }
  if constexpr (Bits::kRawResidual) {
    if (bits.CodeBit(model.schema_zero, known)) {
      index = bits.CodeBitsDirect(index, 8);
      if (!Bits::kEncoding && index < kind.num_enumerants) {
        return GetEnumerant(kind, static_cast<uint16_t>(index)).value;
      }
      return Bits::kEncoding ? hint : 0;
    }
    return bits.CodeUint32(hint);
  }
  // field.kind is a value-enum kind here (the cursor yields kValueEnum only
  // for those), so its ordinal is valid.
  const size_t key =
      static_cast<size_t>(model.value_enum_ordinal[field.kind]) * 16 +
      Slot16(field.slot);
  ValueEnumModel& ctx = EnsureIndexContext(model.arena, model.value_enums[key],
                                           kind.num_enumerants);
  if (bits.CodeBit(ctx.known, known)) {
    index = ctx.index.CodeValue(bits, model.arena, index,
                                IndexLeafPrior{kind.num_enumerants});
    if (!Bits::kEncoding && index < kind.num_enumerants) {
      return GetEnumerant(kind, static_cast<uint16_t>(index)).value;
    }
    return Bits::kEncoding ? hint : 0;
  }
  return CodeUint(bits, model.arena, ctx.escape, hint);
}

template <class Bits>
uint32_t CodeBitEnum(Bits& bits, ModelState& model,
                     const OperandCursor::Field& field, uint32_t hint) {
  const OperandKindDesc& kind = GetOperandKind(field.kind);
  if constexpr (Bits::kRawResidual) {
    uint32_t known_mask = 0;
    uint32_t value = 0;
    for (uint16_t i = 0; i < kind.num_enumerants; ++i) {
      const EnumerantDesc& enumerant = GetEnumerant(kind, i);
      if (!IsSingleBit(enumerant.value)) {
        continue;
      }
      known_mask |= enumerant.value;
      const bool present = Bits::kEncoding && ((hint & enumerant.value) != 0);
      if (bits.CodeBit(model.schema_zero, present)) {
        value |= enumerant.value;
      }
    }
    const uint32_t residual_hint = hint & ~known_mask;
    if (bits.CodeBit(model.schema_zero,
                     Bits::kEncoding && residual_hint != 0)) {
      value |= bits.CodeUint32(residual_hint);
    }
    return Bits::kEncoding ? hint : value;
  }
  BitEnumModel& ctx =
      EnsureContext(model.bit_enums[field.kind]);
  if (ctx.bits.empty()) {
    ctx.bits.resize(kind.num_enumerants);
  }
  uint32_t known_mask = 0;
  uint32_t value = 0;
  for (uint16_t i = 0; i < kind.num_enumerants; ++i) {
    const EnumerantDesc& enumerant = GetEnumerant(kind, i);
    if (!IsSingleBit(enumerant.value)) {
      continue;
    }
    known_mask |= enumerant.value;
    const bool present = Bits::kEncoding && ((hint & enumerant.value) != 0);
    if (bits.CodeBit(ctx.bits[i], present)) {
      value |= enumerant.value;
    }
  }
  const uint32_t residual_hint = hint & ~known_mask;
  if (bits.CodeBit(ctx.unknown, Bits::kEncoding && residual_hint != 0)) {
    value |= CodeUint(bits, model.arena, ctx.residual, residual_hint);
  }
  return Bits::kEncoding ? hint : value;
}

template <class Bits>
ARIXV_NOINLINE uint32_t CodeLiteralInteger(Bits& bits, ModelState& model,
                                           const InstructionDesc* desc,
                                           const OperandCursor::Field& field,
                                           uint16_t inst_index,
                                           bool has_last_enum,
                                           uint32_t last_enum, uint32_t hint) {
  if constexpr (Bits::kRawResidual) {
    (void)desc;
    (void)has_last_enum;
    (void)last_enum;
    InstGroup& group = model.inst_group(inst_index);
    const size_t key = Slot16(field.slot);
    if (group.last_literal_valid[key] == 0) {
      group.last_literal[key] = 0;
      group.last_literal_valid[key] = 1;
    }
    const uint32_t base = group.last_literal[key];
    const uint32_t delta = Bits::kEncoding ? ZigZag32(hint - base) : 0;
    const uint32_t coded = bits.CodeUint32(delta);
    const uint32_t value = Bits::kEncoding ? hint : base + UnZigZag32(coded);
    group.last_literal[key] = value;
    return Bits::kEncoding ? hint : value;
  }
  const size_t cls = ClassBucket(desc);
  const uint32_t slot8 = Slot8(field.slot);
  if (has_last_enum) {
    EnumLiteralCache& cache = EnsureContext(model.arena, model.enum_literals);
    const uint64_t cache_key =
        (uint64_t{1} << 63) | (static_cast<uint64_t>(inst_index) << 36) |
        (static_cast<uint64_t>(Slot16(field.slot)) << 32) | last_enum;
    const uint32_t mix =
        static_cast<uint32_t>(cache_key ^ (cache_key >> 32)) * 0x9E3779B1u;
    EnumLiteralCache::Entry& entry =
        cache.entries[mix >> (32 - EnumLiteralCache::kBits)];
    const bool hit = entry.key == cache_key;
    const uint32_t prev = hit ? entry.last : 0u;
    const uint32_t stride = hit ? entry.stride : 0u;
    const uint32_t pred = prev + stride;
    uint32_t value = pred;
    if (!bits.CodeBit(cache.stride_match[slot8],
                      Bits::kEncoding && hint == pred)) {
      UintModel& ctx =
          EnsureContext(model.arena, model.integer_values[cls * 9 + slot8]);
      value = CodeDelta(bits, model.arena, ctx, hint, prev);
    }
    entry.key = cache_key;
    entry.stride = value - prev;
    entry.last = value;
    return Bits::kEncoding ? hint : value;
  }
  InstGroup& group = model.inst_group(inst_index);
  const size_t key = Slot16(field.slot);
  if (group.last_literal_valid[key] == 0) {
    group.last_literal[key] = 0;
    group.last_literal_valid[key] = 1;
  }
  const uint32_t base = group.last_literal[key];
  UintModel& ctx =
      EnsureContext(model.arena, model.integer_values[cls * 9 + slot8]);
  const uint32_t value = CodeDelta(bits, model.arena, ctx, hint, base);
  group.last_literal[key] = value;
  return Bits::kEncoding ? hint : value;
}

template <class Bits>
uint32_t CodeContextNumberRaw(Bits& bits, ModelState& model, uint32_t hint) {
  if constexpr (Bits::kRawResidual) {
    (void)model;
    return bits.CodeUint32(hint);
  }
  return CodeUint(bits, model.arena,
                  EnsureUint(model.arena, model.context_number), hint);
}

template <class Bits>
ARIXV_NOINLINE uint32_t CodeContextNumber(Bits& bits, ModelState& model,
                                          uint32_t current_result_type,
                                          uint32_t hint) {
  if constexpr (Bits::kRawResidual) {
    (void)model;
    (void)current_result_type;
    return bits.CodeUint32(hint);
  }
  const Tracker::IdInfo* type_info =
      current_result_type != 0 ? model.tracker.Find(current_result_type)
                               : nullptr;
  if (type_info != nullptr) {
    if (type_info->type_class == Tracker::TypeClass::kFloat &&
        type_info->width == 32) {
      if (bits.CodeBit(model.float_zero, Bits::kEncoding && hint == 0u)) {
        return 0u;
      }
      const bool neg =
          bits.CodeBit(model.float_sign, Bits::kEncoding && (hint >> 31) != 0);
      static constexpr BitTree<6> kFloatExpPrior =
          MakeUintLengthTreePrior(0, 9);
      static constexpr BitTree<6> kFloatMantPrior =
          MakeUintLengthTreePrior(1, 23);
      const uint32_t exp =
          CodeDelta(bits, model.arena,
                    EnsureUintWithPrior(model.arena, model.float_exp,
                                        kFloatExpPrior),
                    (hint >> 23) & 0xffu, 127u) &
          0xffu;
      const uint32_t rmant_hint = Bits::kEncoding ? ReverseBits23(hint) : 0u;
      const bool mant_zero = bits.CodeBit(model.float_mant_zero,
                                          Bits::kEncoding && rmant_hint == 0u);
      const uint32_t rmant =
          mant_zero ? 0u
                    : CodeUint(bits, model.arena,
                               EnsureUintWithPrior(model.arena,
                                                   model.float_mant,
                                                   kFloatMantPrior),
                               rmant_hint);
      const uint32_t value =
          (neg ? 0x80000000u : 0u) | (exp << 23) | ReverseBits23(rmant);
      return Bits::kEncoding ? hint : value;
    }
    if (type_info->type_class == Tracker::TypeClass::kInt) {
      IntConstCache& cache = EnsureContext(model.arena, model.int_const_cache);
      size_t slot = IntConstCache::kSize;
      for (size_t i = 0; i < IntConstCache::kSize; ++i) {
        if (cache.type_id[i] == current_result_type) {
          slot = i;
          break;
        }
      }
      const uint32_t base = slot < IntConstCache::kSize ? cache.last[slot] : 0u;
      const uint32_t value = CodeDelta(
          bits, model.arena, EnsureUint(model.arena, model.context_int), hint,
          base);
      if (slot == IntConstCache::kSize) {
        slot = cache.next_evict++ & (IntConstCache::kSize - 1);
        cache.type_id[slot] = current_result_type;
      }
      cache.last[slot] = value;
      return Bits::kEncoding ? hint : value;
    }
  }
  return CodeContextNumberRaw(bits, model, hint);
}

template <class Bits>
uint32_t CodeSpecConstantOp(Bits& bits, ModelState& model, uint32_t hint) {
  if constexpr (Bits::kRawResidual) {
    (void)model;
    return bits.CodeUint32(hint);
  }
  return CodeUint(bits, model.arena, EnsureUint(model.arena, model.spec_constant_op), hint);
}

template <class Bits>
uint32_t CodeExtInstNumber(Bits& bits, ModelState& model,
                           const ExtInstSetDesc* set, uint32_t hint) {
  if (!set) {
    if constexpr (Bits::kRawResidual) {
      return bits.CodeUint32(hint);
    }
    return CodeUint(bits, model.arena, EnsureUint(model.arena, model.ext_inst_unknown), hint);
  }
  const int set_index = FindExtInstSetIndex(set);
  if (set_index < 0) {
    if constexpr (Bits::kRawResidual) {
      return bits.CodeUint32(hint);
    }
    return CodeUint(bits, model.arena, EnsureUint(model.arena, model.ext_inst_unknown), hint);
  }
  const InstructionDesc* inst =
      Bits::kEncoding ? FindExtInstruction(*set, hint) : nullptr;
  uint32_t index = 0;
  bool known = false;
  if (inst) {
    for (uint16_t i = 0; i < set->num_instructions && i < 256; ++i) {
      if (&set->instructions[i] == inst) {
        index = i;
        known = true;
        break;
      }
    }
  }
  if constexpr (Bits::kRawResidual) {
    if (bits.CodeBit(model.schema_zero, known)) {
      index = bits.CodeBitsDirect(index, 8);
      if (!Bits::kEncoding && index < set->num_instructions) {
        return set->instructions[index].opcode;
      }
      return Bits::kEncoding ? hint : 0;
    }
    return bits.CodeUint32(hint);
  }
  ExtInstNumberModel& ctx = EnsureIndexContext(
      model.arena, model.ext_inst_nums[static_cast<size_t>(set_index)],
      set->num_instructions);
  if (bits.CodeBit(ctx.known, known)) {
    index = ctx.index.CodeValue(bits, model.arena, index,
                                IndexLeafPrior{set->num_instructions});
    if (!Bits::kEncoding && index < set->num_instructions) {
      return set->instructions[index].opcode;
    }
    return Bits::kEncoding ? hint : 0;
  }
  return CodeUint(bits, model.arena, ctx.escape, hint);
}

ARIXV_NOINLINE SymbolCdf& MakeStringOrder0Leaf(Arena& arena,
                                               ArenaPtr<SymbolCdf>& slot,
                                               uint32_t high) {
  slot.Make(arena);
  LoadPrior(*slot, GetStringOrder0LeafPrior(high));
  return *slot;
}

// One string byte under order-1 context `prev`. A context's tables start as
// a copy of the running order-0 model (counts included) instead of uniform.
template <class Bits>
uint8_t CodeStringByte(Bits& bits, StringModel& m, uint8_t prev, uint8_t b) {
  if constexpr (Bits::kRawResidual) {
    return static_cast<uint8_t>(bits.CodeBitsDirect(b, kByteBits));
  } else {
    StatSiteScope<Bits> site(kSiteByte);
    Arena& arena = m.bytes.arena;
    ArenaPtr<ByteTree>& tree = m.bytes.tree[prev];
    if (!tree) {
      tree.Make(arena);
      tree->root = m.order0.root;
    }
    const uint32_t high = bits.template CodeSymbol<16, kByteSymbolRateCap>(
        tree->root, (b >> 4) & 15u);
    ArenaPtr<SymbolCdf>& leaf0 = m.order0.leaf[high];
    SymbolCdf& order0_leaf =
        leaf0 ? *leaf0 : MakeStringOrder0Leaf(arena, leaf0, high);
    ArenaPtr<SymbolCdf>& leaf = tree->leaf[high];
    if (!leaf) {
      leaf.Make(arena);
      *leaf = order0_leaf;
    }
    const uint32_t low = bits.template CodeSymbol<16, kByteSymbolRateCap>(
        *leaf, b & 15u);
    SymbolCdfUpdate<16, kByteSymbolRateCap>(m.order0.root,
                                            static_cast<int>(high));
    SymbolCdfUpdate<16, kByteSymbolRateCap>(order0_leaf, static_cast<int>(low));
    return Bits::kEncoding ? b : static_cast<uint8_t>((high << 4) | low);
  }
}

// Adds a string coded another way (list hit, dictionary, word fallback) to
// the match history: its bytes through the first NUL.
void PushStringHistory(StringModel& m, const std::string& raw) {
  for (char ch : raw) {
    m.Push(static_cast<uint8_t>(ch));
    if (ch == '\0') {
      break;
    }
  }
}

int FindGrammarString(const std::string& raw) {
  // raw is canonical (NUL-terminated, zero padded); compare up to the NUL.
  const char* s = raw.c_str();
  int lo = 0;
  int hi = NumGrammarStrings();
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    const int c = std::strcmp(GetGrammarString(static_cast<uint16_t>(mid)), s);
    if (c == 0) {
      return mid;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return -1;
}

// Kept out of line: strings are a small share of decisions, and inlining
// this into CodeModule shifts the compiler's inlining of hotter paths.
template <class Bits, size_t kInlineWords>
ARIXV_NOINLINE void CodeString(Bits& bits, ModelState& model,
                               const InstructionDesc* desc,
                const uint32_t* hint_words,
                uint32_t remaining, uint32_t* span_out,
                WordBuffer<kInlineWords>& words) {
  std::string raw;
  if (Bits::kEncoding) {
    uint32_t span = 0;
    bool found_nul = false;
    while (span < remaining && !found_nul) {
      const uint32_t word = hint_words[span++];
      for (int byte = 0; byte < 4; ++byte) {
        if (((word >> (byte * 8)) & 0xffu) == 0) {
          found_nul = true;
          break;
        }
      }
    }
    if (!found_nul) {
      span = remaining;
    }
    raw = RawStringBytes(hint_words, span);
  }

  const int rank = Bits::kEncoding ? FindRecentString(model, raw) : -1;
  if (bits.CodeBit(model.string_match, rank >= 0 && rank < 64)) {
    const uint32_t coded_rank =
        model.string_rank.CodeValue(bits,
                                    rank >= 0 ? static_cast<uint32_t>(rank)
                                              : 0);
    if (!Bits::kEncoding &&
        coded_rank < static_cast<uint32_t>(model.recent_strings.size())) {
      raw = model.recent_strings[coded_rank];
    }
    PushStringHistory(*model.strings, raw);
  } else {
    StringModel& sm = *model.strings;
    const bool canonical = Bits::kEncoding && IsCanonicalStringBytes(raw);
    const int dict = canonical ? FindGrammarString(raw) : -1;
    if (bits.CodeBit(sm.dict_flag[ClassBucket(desc)], dict >= 0)) {
      const uint32_t index = sm.dict_index.CodeValue(
          bits, dict >= 0 ? static_cast<uint32_t>(dict) : 0u);
      if (!Bits::kEncoding) {
        // An out-of-range index (corrupt input) decodes as the empty string.
        raw = index < NumGrammarStrings()
                  ? std::string(GetGrammarString(static_cast<uint16_t>(index)))
                  : std::string();
        raw.push_back('\0');
        while ((raw.size() & 3u) != 0) {
          raw.push_back('\0');
        }
      }
      PushStringHistory(sm, raw);
    } else if (bits.CodeBit(model.string_canonical, canonical)) {
      const std::string source = raw;
      std::string coded;
      uint8_t prev = 0;
      // Match state: the history position predicted to repeat next (0 =
      // none) and how many predictions in a row have held.
      uint32_t match = 0;
      uint32_t match_len = 0;
      if (remaining > 0) {
        // Ensure the 4 KB match table is allocated before the first byte-tree
        // objects so it fills bytes.arena's 4 KB first block without stranding.
        (void)sm.Table();
      }
      while (coded.size() < static_cast<size_t>(remaining) * 4u) {
        if constexpr (!Bits::kEncoding) {
          if (bits.exhausted()) {
            break;
          }
        }
        const uint8_t hint =
            Bits::kEncoding ? static_cast<uint8_t>(source[coded.size()]) : 0;
        // Byte 0 follows the previous string's NUL; its order-2 context
        // (prev_last_char, '\0') crosses unrelated strings.
        if (match == 0 && !coded.empty()) {
          match = sm.Lookup();
          match_len = 0;
        }
        uint8_t b = 0;
        bool predicted = false;
        if (match != 0) {
          const uint8_t guess = sm.At(match);
          Prob& flag = sm.match_flag[match_len < 15 ? match_len : 15];
          predicted = bits.CodeBit(flag, hint == guess);
          b = guess;
        }
        if (predicted) {
          ++match;  // still < End(): it trails the byte being appended
          ++match_len;
        } else {
          match = 0;
          b = CodeStringByte(bits, sm, prev, hint);
        }
        sm.Push(b);
        coded.push_back(static_cast<char>(b));
        prev = b;
        if (b == 0) {
          break;
        }
      }
      while ((coded.size() & 3u) != 0) {
        coded.push_back('\0');
      }
      raw = coded;
    } else {
      raw.clear();
      std::array<uint8_t, 4> prev_bytes = {0, 0, 0, 0};
      bool found_nul = false;
      while (raw.size() / 4 < remaining && !found_nul) {
        if constexpr (!Bits::kEncoding) {
          if (bits.exhausted()) {
            break;
          }
        }
        const uint32_t hint = Bits::kEncoding
                                  ? hint_words[raw.size() / 4]
                                  : 0;
        const uint32_t word =
            CodeGenericOperandWord(bits, model, prev_bytes, hint);
        for (int byte = 0; byte < 4; ++byte) {
          const char ch = static_cast<char>((word >> (byte * 8)) & 0xffu);
          raw.push_back(ch);
          if (ch == '\0') {
            found_nul = true;
          }
        }
      }
      PushStringHistory(sm, raw);
    }
  }

  if (!Bits::kEncoding && raw.empty()) {
    raw.push_back('\0');
  }

  const uint32_t span =
      static_cast<uint32_t>((raw.size() + 3u) / 4u);
  *span_out = span == 0 ? 1 : span;
  words.clear();
  for (uint32_t i = 0; i < *span_out; ++i) {
    words.push_back(WordFromBytes(raw, i));
  }
  TouchString(model, raw);
}

template <class Bits>
uint16_t CodeOpcode(ModelState& model, Bits& bits, uint16_t hint,
                    const InstructionDesc** desc_out,
                    uint16_t* inst_index_out) {
  const Section section = model.tracker.section();
  const bool prev_known = model.tracker.prev_opcode_class() != 0xFF;
  const size_t known_ctx = static_cast<size_t>(section) * 2 +
                           (prev_known ? 1u : 0u);

  const InstructionDesc* hint_desc = Bits::kEncoding ? FindInstruction(hint)
                                                     : nullptr;
  const bool known = bits.CodeBit(model.known_opcode[known_ctx],
                                  hint_desc != nullptr);
  uint16_t opcode = hint;
  if (known) {
    if constexpr (Bits::kRawResidual) {
      uint32_t index = 0;
      if (Bits::kEncoding) {
        const int found = FindInstructionIndex(hint);
        assert(found >= 0);
        index = static_cast<uint32_t>(found);
      }
      index = bits.CodeBitsDirect(index, 10);
      if (index >= NumCoreInstructions()) {
        index = NumCoreInstructions() - 1;
      }
      const uint16_t inst_index = static_cast<uint16_t>(index);
      opcode = GetCoreInstruction(inst_index).opcode;
      *desc_out = &GetCoreInstruction(inst_index);
      *inst_index_out = inst_index;
      return opcode;
    }
    const uint16_t* prior =
        GetOpcodeSectionPrior(static_cast<uint8_t>(section));
    const uint16_t prev_key = PreviousOpcodeKey(model.tracker);
    ArenaPtr<OpcodeCtx>& order1_slot = model.opcode_contexts[prev_key];
    if (!order1_slot) {
      order1_slot.Make(model.arena);
      order1_slot->tree.SetPrior(prior);
    }
    OpcodeCtx& order1 = *order1_slot;
    uint32_t index = 0;
    if (Bits::kEncoding) {
      const int found = FindInstructionIndex(hint);
      assert(found >= 0);
      index = static_cast<uint32_t>(found);
    }
    if (order1.count < kOpcodeOrder1MinCount) {
      const uint8_t prev_class =
          prev_known ? model.tracker.prev_opcode_class()
                     : NumInstructionClasses();
      const size_t tree_ctx =
          static_cast<size_t>(section) *
              (static_cast<size_t>(NumInstructionClasses()) + 1) +
          prev_class;
      ArenaPtr<OpcodeTree>& tree_slot = model.opcode_index_trees[tree_ctx];
      if (!tree_slot) {
        tree_slot.Make(model.arena);
        tree_slot->SetPrior(prior);
      }
      index = tree_slot->CodeValue(bits, model.arena, index, prior);
      order1.tree.Train(model.arena, index, prior);
      ++order1.count;
    } else {
      index = order1.tree.CodeValue(bits, model.arena, index, prior);
    }
    if (index >= NumCoreInstructions()) {
      index = NumCoreInstructions() - 1;
    }
    const uint16_t inst_index = static_cast<uint16_t>(index);
    opcode = GetCoreInstruction(inst_index).opcode;
    *desc_out = &GetCoreInstruction(inst_index);
    *inst_index_out = inst_index;
  } else {
    if constexpr (Bits::kRawResidual) {
      opcode = static_cast<uint16_t>(bits.CodeBitsDirect(hint, 16));
      *desc_out = nullptr;
      *inst_index_out = NumCoreInstructions();
      return opcode;
    }
    opcode = static_cast<uint16_t>(
        EnsureContext(model.arena, model.unknown_opcode_tree)
            .CodeValue(bits, hint));
    *desc_out = nullptr;
    *inst_index_out = NumCoreInstructions();
  }
  return opcode;
}

bool WordHasNulByte(uint32_t word) {
  return (word & 0x000000ffu) == 0 || (word & 0x0000ff00u) == 0 ||
         (word & 0x00ff0000u) == 0 || (word & 0xff000000u) == 0;
}

bool IsFixedOneWordKind(uint16_t kind_index) {
  const OperandKindDesc& kind = GetOperandKind(kind_index);
  switch (kind.category) {
    case OperandCategory::kIdResult:
    case OperandCategory::kIdResultType:
    case OperandCategory::kIdRef:
    case OperandCategory::kIdScope:
    case OperandCategory::kIdMemorySemantics:
    case OperandCategory::kLiteralInteger:
    case OperandCategory::kLiteralExtInstInteger:
      return true;
    case OperandCategory::kValueEnum:
    case OperandCategory::kBitEnum: {
      for (uint16_t i = 0; i < kind.num_enumerants; ++i) {
        if (GetEnumerant(kind, i).num_params != 0) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

// Encoder-only exact check: verifies that walking `desc` over
// `inst_words[1..count_hint-1]` with OperandCursor using the same deferred
// budget as CodeModule reaches kEnd without degrading and consumes exactly
// `count_hint - 1` words (so a decoder running in deferred-word-count mode
// will stop at the exact same word).
ARIXV_NOINLINE bool ValidateGrammarImplied(const InstructionDesc* desc,
                                           const uint32_t* inst_words,
                                           uint32_t count_hint,
                                           size_t remaining_body,
                                           const Tracker& tracker) {
  if (!desc || !inst_words || count_hint < desc->min_word_count ||
      count_hint > remaining_body) {
    return false;
  }
  const uint32_t limit = count_hint - 1;
  const uint32_t budget =
      static_cast<uint32_t>(std::min<size_t>(remaining_body, 0xffffu));
  OperandCursor cursor;
  cursor.Begin(desc, budget, /*force_opaque=*/false);
  uint32_t pos = 0;
  uint32_t result_type = 0;
  while (pos < limit) {
    const OperandCursor::Field f = cursor.Next();
    if (f.type != OperandCursor::FieldType::kOperand) {
      return false;
    }
    const uint32_t w0 = inst_words[1 + pos];
    uint32_t span = 1;
    switch (f.category) {
      case OperandCategory::kIdResultType:
        result_type = w0;
        break;
      case OperandCategory::kLiteralString: {
        uint32_t s = 0;
        bool found_nul = false;
        while (pos + s < limit) {
          if (WordHasNulByte(inst_words[1 + pos + s])) {
            ++s;
            found_nul = true;
            break;
          }
          ++s;
        }
        if (!found_nul) {
          return false;
        }
        span = s;
        break;
      }
      case OperandCategory::kLiteralContextDependentNumber: {
        if (!HasInstructionFlag(desc, kInstConstantNumber)) {
          return false;
        }
        const uint32_t width = tracker.TypeWidth(result_type);
        if (width == 0) {
          return false;
        }
        span = width > 32 ? 2u : 1u;
        if (pos + span > limit) {
          return false;
        }
        break;
      }
      case OperandCategory::kLiteralSpecConstantOpInteger:
      case OperandCategory::kLiteralExtInstInteger:
      case OperandCategory::kComposite:
      case OperandCategory::kUnknown:
        return false;
      default:
        break;
    }
    cursor.Consume(f, span, w0);
    pos += span;
    if (cursor.degraded()) {
      return false;
    }
  }
  const OperandCursor::Field end_field = cursor.Next();
  return end_field.type == OperandCursor::FieldType::kEnd && !cursor.degraded();
}

// Encoder-only check for deferred OpExtInst: requires the full instruction to
// fit in `remaining_body` and, when the resolved extended instruction has a
// fixed word count, requires `count_hint` to match it exactly.
ARIXV_NOINLINE bool ValidateExtInstConsistent(const InstructionDesc* desc,
                                              const uint32_t* inst_words,
                                              uint32_t count_hint,
                                              size_t remaining_body,
                                              const Tracker& tracker) {
  if (!desc || !inst_words || count_hint < desc->min_word_count ||
      count_hint > remaining_body) {
    return false;
  }
  const Tracker::IdInfo* info = tracker.Find(inst_words[3]);
  const ExtInstSetDesc* ext_set = info ? info->ext_set : nullptr;
  if (!ext_set) {
    return true;
  }
  const InstructionDesc* ext_inst = FindExtInstruction(*ext_set, inst_words[4]);
  if (!ext_inst || !ext_inst->fixed_word_count) {
    return true;
  }
  return count_hint == desc->min_word_count + ext_inst->num_operands;
}

template <class Bits>
uint32_t CodeVarWordCount(ModelState& model, Bits& bits, uint16_t inst_index,
                          const InstructionDesc* desc, uint32_t hint,
                          uint32_t base_count) {
  uint32_t count = hint;
  // A fresh InstGroup starts with last_word_count = 0.
  const uint32_t last_count = model.inst_group(inst_index).last_word_count;
  if constexpr (Bits::kRawResidual) {
    if (bits.CodeBit(model.schema_zero,
                     Bits::kEncoding && hint == last_count)) {
      count = last_count;
    } else {
      const uint32_t extra = Bits::kEncoding ? hint - base_count : 0;
      count = (base_count + bits.CodeUint32(extra)) & 0xffffu;
    }
  } else {
    CountModel& count_model =
        EnsureContext(model.arena, model.class_count_models[ClassBucket(desc)]);
    if (bits.CodeBit(count_model.same_as_last,
                     Bits::kEncoding && hint == last_count)) {
      count = last_count;
    } else {
      const uint32_t extra_hint = Bits::kEncoding ? hint - base_count : 0;
      count = (base_count +
               CodeUint(bits, model.arena, count_model.extra, extra_hint)) &
              0xffffu;
    }
  }
  model.inst_group(inst_index).last_word_count = count;
  return count;
}

template <class Bits>
uint32_t CodeWordCount(ModelState& model, Bits& bits, uint16_t inst_index,
                       const InstructionDesc* desc, uint32_t hint,
                       const uint32_t* inst_words, size_t remaining_body,
                       bool* opaque, bool* deferred, bool* fixed_layout_out) {
  *opaque = true;
  *deferred = false;
  *fixed_layout_out = false;
  if (!desc) {
    if constexpr (Bits::kRawResidual) {
      return bits.CodeUint32(hint) & 0xffffu;
    }
    return CodeUint(bits, model.arena,
                    EnsureUint(model.arena, model.raw_unknown_count), hint) &
           0xffffu;
  }

  const bool use_grammar_implied =
      HasInstructionFlag(desc, kInstGrammarImpliedWordCount);
  const bool use_ext_inst = HasInstructionFlag(desc, kInstExtInst);
  const bool use_optional_tail = HasInstructionFlag(desc, kInstOptionalTail);
  const bool single_optional =
      use_optional_tail && desc->min_word_count == desc->num_operands;

  bool hint_consistent = false;
  if constexpr (Bits::kEncoding) {
    if (desc->fixed_word_count) {
      hint_consistent = (hint == desc->min_word_count);
    } else if (use_grammar_implied) {
      hint_consistent = ValidateGrammarImplied(desc, inst_words, hint,
                                               remaining_body, model.tracker);
    } else if (use_ext_inst) {
      hint_consistent = ValidateExtInstConsistent(
          desc, inst_words, hint, remaining_body, model.tracker);
    } else if (use_optional_tail) {
      if (hint < desc->min_word_count) {
        hint_consistent = false;
      } else if (hint == desc->min_word_count) {
        hint_consistent = true;
      } else if (single_optional) {
        const OperandSlotDesc& tail =
            GetOperandSlots(*desc)[desc->num_operands - 1];
        if (IsFixedOneWordKind(tail.kind)) {
          hint_consistent = (hint == desc->min_word_count + 1u);
        } else {
          hint_consistent = ValidateGrammarImplied(
              desc, inst_words, hint, remaining_body, model.tracker);
        }
      } else {
        hint_consistent = true;
      }
    } else {
      hint_consistent = (hint >= desc->min_word_count);
    }
  }

  bool consistent = false;
  if constexpr (Bits::kRawResidual) {
    consistent = bits.CodeBit(model.schema_zero, hint_consistent);
    if (!consistent) {
      return bits.CodeUint32(hint) & 0xffffu;
    }
  } else {
    consistent = bits.CodeBit(model.word_count_consistent, hint_consistent);
    if (!consistent) {
      return CodeUint(bits, model.arena,
                      EnsureUint(model.arena, model.raw_known_count), hint) &
             0xffffu;
    }
  }

  *opaque = false;
  if (desc->fixed_word_count) {
    *fixed_layout_out = true;
    return desc->min_word_count;
  }
  if (use_grammar_implied || use_ext_inst) {
    *deferred = true;
    return 0;
  }
  if (use_optional_tail) {
    Prob& opt_prob = Bits::kRawResidual ? model.schema_zero
                                        : model.inst_group(inst_index).has_optional;
    const bool has_optional =
        bits.CodeBit(opt_prob, Bits::kEncoding && hint > desc->min_word_count);
    if (!has_optional) {
      *fixed_layout_out = true;
      return desc->min_word_count;
    }
    if (single_optional) {
      const OperandSlotDesc& tail =
          GetOperandSlots(*desc)[desc->num_operands - 1];
      if (IsFixedOneWordKind(tail.kind)) {
        *fixed_layout_out = true;
        return desc->min_word_count + 1u;
      }
      *deferred = true;
      return 0;
    }
    return CodeVarWordCount(model, bits, inst_index, desc, hint,
                            desc->min_word_count + 1u);
  }
  return CodeVarWordCount(model, bits, inst_index, desc, hint,
                          desc->min_word_count);
}

template <class Bits, class IO>
Status CodeModule(ModelState& model, Bits& bits, IO& io, size_t word_count) {
  model.tracker.Reserve(word_count);
  // Stats tagging: encoder accumulates entropy per category; decoder no-op.
  const bool stats = StatsEnabled();
  const auto stat = [&bits, stats](StatCat cat) {
    bits.SetCostSink(stats ? &g_stat_bits[cat] : nullptr);
    if constexpr (Bits::kEncoding && !Bits::kRawResidual) {
      g_stat_cat = stats ? static_cast<int>(cat) : -1;
    }
  };
  stat(kStatHeader);
  const bool swapped = bits.CodeBit(model.swapped_flag, io.SwappedHint());
  if constexpr (!Bits::kEncoding) {
    io.SetSwapped(swapped);
  }

  io.EmitHeaderWord(kSpirvMagic);

  const uint32_t version_hint = io.HeaderWord(1);
  const bool version_canonical =
      bits.CodeBit(model.version_canonical,
                   Bits::kEncoding && IsCanonicalVersion(version_hint));
  uint32_t version = version_hint;
  if (version_canonical) {
    const uint32_t major_hint = (version_hint >> 16) & 0x0fu;
    const uint32_t minor_hint = (version_hint >> 8) & 0x0fu;
    const uint32_t major = model.version_major.CodeValue(bits, major_hint);
    const uint32_t minor = model.version_minor.CodeValue(bits, minor_hint);
    version = (major << 16) | (minor << 8);
  } else {
    version = CodeDirect32(bits, version_hint);
  }
  io.EmitHeaderWord(version);

  io.EmitHeaderWord(CodeGenerator(bits, model, io.HeaderWord(2)));
  io.EmitHeaderWord(0);

  const uint32_t schema_hint = io.HeaderWord(4);
  const bool schema_zero =
      bits.CodeBit(model.schema_zero, Bits::kEncoding && schema_hint == 0);
  const uint32_t schema = schema_zero ? 0 : CodeDirect32(bits, schema_hint);
  io.EmitHeaderWord(schema);

  const size_t body_words = word_count - 5;
  size_t body_pos = 0;
  uint32_t prev_inst_result_id = 0;
  WordBuffer<kInlineInstructionWords> inst_words;
  WordBuffer<kInlineOperandWords> coded_words;
  while (body_pos < body_words) {
    // word_count comes from the untrusted header. Once the coded stream is
    // exhausted, every further word is decoded from past-the-end zero bytes;
    // stop rather than synthesizing up to `body_words` filler words (which
    // would grow `out` to the attacker-declared size). The size check in the
    // caller then reports this truncated stream as kTruncated.
    if constexpr (!Bits::kEncoding) {
      if (bits.exhausted()) {
        break;
      }
    }
    const uint32_t word0_hint = io.BodyWord(body_pos);
    const uint16_t opcode_hint = static_cast<uint16_t>(word0_hint & 0xffffu);
    const uint32_t count_hint = (word0_hint >> 16) & 0xffffu;
    const InstructionDesc* desc = nullptr;
    uint16_t inst_index = NumCoreInstructions();
    stat(kStatOpcode);
    const uint16_t opcode =
        CodeOpcode(model, bits, opcode_hint, &desc, &inst_index);
    if (desc != nullptr) {
      model.EnsureInstContext(inst_index);
    }

    const size_t remaining_body = body_words - body_pos;
    bool opaque = true;
    bool deferred_word_count = false;
    bool fixed_layout = false;
    stat(kStatWordCount);
    const uint32_t declared_count = CodeWordCount(
        model, bits, inst_index, desc, count_hint, io.BodyWordPtr(body_pos),
        remaining_body, &opaque, &deferred_word_count, &fixed_layout);
    const bool patch_word0 = deferred_word_count;
    const uint32_t initial_word0 =
        patch_word0
            ? (Bits::kEncoding ? word0_hint : static_cast<uint32_t>(opcode))
            : ((declared_count << 16) | opcode);
    inst_words.clear();
    uint32_t operand_words =
        deferred_word_count
            ? static_cast<uint32_t>(std::min<size_t>(
                  remaining_body > 0 ? remaining_body - 1 : 0, 0xfffeu))
            : (declared_count == 0
                   ? 0
                   : static_cast<uint32_t>(std::min<size_t>(
                         declared_count - 1,
                         remaining_body > 0 ? remaining_body - 1 : 0)));
    inst_words.push_back(initial_word0);

    model.tracker.OnInstructionStart(opcode, desc, declared_count);
    model.cur_prev_res_id = model.tracker.in_block() ? prev_inst_result_id : 0u;
    model.cur_prev_res_list = ModelState::kUnresolvedList;
    prev_inst_result_id = 0;
    io.EmitBodyWord(initial_word0);

    std::array<uint8_t, 4> prev_bytes = {0, 0, 0, 0};
    OperandCursor cursor;
    // Fixed layouts have only required, one-word operands and no enumerant
    // parameters (including optional-tail instructions whose present prefix is
    // fixed-one-word). Their slot list therefore describes every decoded field
    // directly, avoiding the general cursor state machine on this common path.
    const OperandSlotDesc* fixed_slots =
        fixed_layout ? GetOperandSlots(*desc) : nullptr;
    if (!fixed_layout) {
      cursor.Begin(desc,
                   deferred_word_count ? operand_words + 1u : declared_count,
                   opaque);
    }
    uint32_t consumed = 0;
    uint32_t current_result_type = 0;
    const ExtInstSetDesc* current_ext_set = nullptr;
    bool has_last_enum = false;
    uint32_t last_enum = 0;
    // Result Type is coded after the operands unless operand parsing depends
    // on it (context-dependent literal widths) or OpVariable (whose
    // StorageClass operand is predicted from the pointer Result Type).
    const bool defer_result_type =
        desc != nullptr && !opaque && desc->has_result_type &&
        !HasInstructionFlag(desc, kInstConstantNumber) &&
        !HasInstructionFlag(desc, kInstVariable);
    bool result_type_deferred = false;
    size_t result_type_body_index = 0;
    uint32_t result_type_hint = 0;
    uint32_t id_operands[8];
    uint32_t id_operand_types[8];
    int num_id_operands = 0;
    int total_id_operands = 0;
    uint32_t first_literal_word = 0;
    uint32_t num_literal_ints = 0;
    bool literals_contiguous = true;
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
        cursor_field = field;
      } else {
        cursor_field = cursor.Next();
        field = cursor_field;
        if (field.type == OperandCursor::FieldType::kEnd ||
            (deferred_word_count &&
             field.type == OperandCursor::FieldType::kOpaqueWord)) {
          if (deferred_word_count) {
            break;
          }
          field.type = OperandCursor::FieldType::kOpaqueWord;
        }
      }

      const uint32_t hint0 = io.BodyWord(body_pos + 1 + consumed);
      // Nearly every operand category produces one word.  On decode, avoid
      // routing those words through coded_words only to copy them immediately
      // to inst_words and the output.  Multiword fields (notably strings and
      // context-dependent numbers) retain the staging buffer below.
      uint32_t direct_word = 0;
      bool has_direct_word = false;
      coded_words.clear();
      SingleWordStager<Bits> stage_single_word(coded_words, direct_word,
                                                has_direct_word);
      uint32_t consume_span = 1;

      if (field.type == OperandCursor::FieldType::kOpaqueWord) {
        stat(kStatOpaque);
        stage_single_word(
            CodeGenericOperandWord(bits, model, prev_bytes, hint0));
      } else {
        switch (field.category) {
          case OperandCategory::kIdResult: {
            stat(kStatResultId);
            const uint32_t rid = CodeResultId(bits, model, desc, hint0);
            prev_inst_result_id = rid;
            stage_single_word(rid);
            break;
          }
          case OperandCategory::kIdResultType: {
            if (defer_result_type && !result_type_deferred) {
              // The encoder stages the true word; the decoder a placeholder
              // patched after the operands.
              result_type_deferred = true;
              result_type_body_index = body_pos + 1 + consumed;
              result_type_hint = hint0;
              stage_single_word(Bits::kEncoding ? hint0 : 0u);
              break;
            }
            stat(kStatResultType);
            const uint32_t word = CodeResultType(bits, model, desc, hint0);
            stage_single_word(word);
            current_result_type = word;
            break;
          }
          case OperandCategory::kIdRef:
          case OperandCategory::kIdScope:
          case OperandCategory::kIdMemorySemantics: {
            stat(kStatIdRef);
            const uint32_t word = CodeIdRef(
                bits, model, inst_index, field, hint0, num_id_operands,
                num_id_operands > 0 ? id_operand_types[0] : 0u);
            stage_single_word(word);
            ++total_id_operands;
            if (num_id_operands < 8) {
              id_operand_types[num_id_operands] = model.last_id_operand_type;
              id_operands[num_id_operands++] = word;
            }
            if (desc && desc->opcode == 12 && field.slot == 2) {
              current_ext_set = model.tracker.ExtInstSetOf(word);
              cursor.SetExtInstSet(current_ext_set);
            }
            break;
          }
          case OperandCategory::kValueEnum: {
            stat(kStatValueEnum);
            uint32_t word;
            uint32_t pred_sc = 0;
            if (HasInstructionFlag(desc, kInstVariable) && field.slot == 2 &&
                model.tracker.PointerStorageClass(current_result_type,
                                                  &pred_sc) &&
                bits.CodeBit(model.var_sc_matches,
                             Bits::kEncoding && hint0 == pred_sc)) {
              word = pred_sc;
            } else {
              word = CodeValueEnum(bits, model, field, hint0);
            }
            stage_single_word(word);
            has_last_enum = true;
            last_enum = word;
            break;
          }
          case OperandCategory::kBitEnum:
            stat(kStatBitEnum);
            stage_single_word(CodeBitEnum(bits, model, field, hint0));
            break;
          case OperandCategory::kLiteralInteger:
            stat(kStatLitInt);
            if (num_literal_ints == 0) {
              first_literal_word = static_cast<uint32_t>(inst_words.size());
              num_literal_ints = 1;
            } else if (first_literal_word + num_literal_ints ==
                       static_cast<uint32_t>(inst_words.size())) {
              ++num_literal_ints;
            } else {
              literals_contiguous = false;
            }
            stage_single_word(CodeLiteralInteger(bits, model, desc, field,
                                                 inst_index, has_last_enum,
                                                 last_enum, hint0));
            break;
          case OperandCategory::kLiteralContextDependentNumber: {
            stat(kStatLitCdn);
            if (deferred_word_count &&
                HasInstructionFlag(desc, kInstConstantNumber) &&
                model.tracker.TypeWidth(current_result_type) == 0) {
              // Corrupt-stream guard: encoder only defers when TypeWidth > 0.
              consume_span = 1;
            } else {
              consume_span =
                  SpanForField(field, desc, current_result_type, model.tracker,
                               inst_words.data(),
                               static_cast<uint32_t>(inst_words.size()),
                               remaining);
            }
            const uint32_t actual_span = std::min(consume_span, remaining);
            if (actual_span == 1) {
              stage_single_word(CodeContextNumber(
                  bits, model, current_result_type, hint0));
            } else if (actual_span >= 2 &&
                model.tracker.TypeWidth(current_result_type) > 32) {
              const uint32_t low = CodeContextNumberRaw(bits, model, hint0);
              coded_words.push_back(low);
              const uint32_t high_hint =
                  Bits::kEncoding ? io.BodyWord(body_pos + 1 + consumed + 1)
                                  : 0;
              const uint32_t signext =
                  (low & 0x80000000u) ? 0xffffffffu : 0u;
              if (bits.CodeBit(model.context_high_signext,
                               Bits::kEncoding && high_hint == signext)) {
                coded_words.push_back(signext);
              } else {
                coded_words.push_back(
                    CodeContextNumberRaw(bits, model, high_hint));
              }
            } else {
              for (uint32_t i = 0; i < actual_span; ++i) {
                coded_words.push_back(CodeContextNumberRaw(
                    bits, model,
                    Bits::kEncoding ? io.BodyWord(body_pos + 1 + consumed + i)
                                    : 0));
              }
            }
            break;
          }
          case OperandCategory::kLiteralExtInstInteger: {
            stat(kStatExtNum);
            const uint32_t ext_num =
                CodeExtInstNumber(bits, model, current_ext_set, hint0);
            stage_single_word(ext_num);
            if (deferred_word_count && HasInstructionFlag(desc, kInstExtInst)) {
              const InstructionDesc* ext_inst =
                  current_ext_set
                      ? FindExtInstruction(*current_ext_set, ext_num)
                      : nullptr;
              if (!ext_inst || !ext_inst->fixed_word_count) {
                stat(kStatWordCount);
                const uint32_t ext_total = CodeVarWordCount(
                    model, bits, inst_index, desc, count_hint,
                    desc->min_word_count);
                uint32_t target =
                    ext_total > 0
                        ? static_cast<uint32_t>(std::min<size_t>(
                              ext_total - 1, remaining_body - 1))
                        : 0u;
                if (target < consumed + 1) {
                  target = consumed + 1;
                }
                operand_words = target;
                cursor.SetRemaining(operand_words - consumed);
                deferred_word_count = false;
              }
            }
            break;
          }
          case OperandCategory::kLiteralString: {
            stat(kStatString);
            const uint32_t* string_hint_ptr =
                Bits::kEncoding ? io.BodyWordPtr(body_pos + 1 + consumed)
                                : nullptr;
            CodeString(bits, model, desc, string_hint_ptr, remaining,
                       &consume_span, coded_words);
            if (consume_span > remaining) {
              consume_span = remaining;
            }
            break;
          }
          case OperandCategory::kLiteralSpecConstantOpInteger:
            stat(kStatSpecOp);
            stage_single_word(CodeSpecConstantOp(bits, model, hint0));
            break;
          case OperandCategory::kComposite:
          case OperandCategory::kUnknown:
            stat(kStatOpaque);
            stage_single_word(
                CodeGenericOperandWord(bits, model, prev_bytes, hint0));
            break;
        }
      }

      if (has_direct_word) {
        inst_words.push_back(direct_word);
        io.EmitBodyWord(direct_word);
        if (!fixed_layout) {
          cursor.Consume(cursor_field, 1, direct_word);
        }
        ++consumed;
        continue;
      }

      if (coded_words.empty()) {
        stat(kStatOpaque);
        coded_words.push_back(
            CodeGenericOperandWord(bits, model, prev_bytes, hint0));
      }
      if (coded_words.size() > remaining) {
        coded_words.resize(remaining);
      }
      consume_span = std::max<uint32_t>(
          1, std::min<uint32_t>(consume_span,
                                static_cast<uint32_t>(coded_words.size())));

      for (size_t i = 0; i < coded_words.size(); ++i) {
        const uint32_t word = coded_words[i];
        inst_words.push_back(word);
        io.EmitBodyWord(word);
      }
      if (!fixed_layout) {
        cursor.Consume(cursor_field, consume_span, coded_words.front());
      }
      consumed += static_cast<uint32_t>(coded_words.size());
    }

    if (patch_word0) {
      const uint32_t final_count =
          static_cast<uint32_t>(inst_words.size()) & 0xffffu;
      const uint32_t final_word0 = (final_count << 16) | opcode;
      inst_words.data()[0] = final_word0;
      io.PatchBodyWord(body_pos, final_word0);
    }
    if (result_type_deferred && inst_words.size() > 1) {
      stat(kStatResultType);
      const uint32_t type = CodeDeferredResultType(
          bits, model, desc, inst_index, id_operands, id_operand_types,
          num_id_operands, total_id_operands, inst_words.data(),
          static_cast<uint32_t>(inst_words.size()), first_literal_word,
          literals_contiguous ? num_literal_ints : 0u, result_type_hint);
      inst_words.data()[1] = type;
      io.PatchBodyWord(result_type_body_index, type);
    }
    model.tracker.OnInstructionEnd(inst_words.data(),
                                   static_cast<uint32_t>(inst_words.size()),
                                   inst_index,
                                   num_id_operands > 0 ? id_operands[0] : 0);
    body_pos += 1 + consumed;
  }

  stat(kStatBound);
  const uint32_t predicted_bound = model.tracker.max_seen_id() + 1;
  const uint32_t bound_hint = io.BoundHint();
  const bool bound_matches =
      bits.CodeBit(model.bound_matches,
                   Bits::kEncoding && bound_hint == predicted_bound);
  const uint32_t bound =
      bound_matches ? predicted_bound
                    : [&]() {
                        if constexpr (Bits::kRawResidual) {
                          return bits.CodeUint32(bound_hint);
                        }
                        return CodeUint(bits, model.arena, EnsureUint(model.arena, model.bound_value),
                                        bound_hint);
                      }();
  io.PatchBound(bound);

  if (bits.exhausted()) {
    if constexpr (std::is_same_v<Bits, BitDecoder>) {
      if (!bits.range_exhausted()) {
        return Status::kBadContainer;
      }
    }
    return Status::kTruncated;
  }
  return Status::kOk;
}

}  // namespace

bool PrimedStringMatchTableIsConsistent() {
  int bits = 0;
  const uint32_t* generated = GetPrimedStringMatchTable(&bits);
  if (bits != kStringMatchBits) {
    return false;
  }
  StringMatchTable rebuilt{};
  const std::string_view primer = GetStringPrimer();
  for (size_t v = 2; v < primer.size(); ++v) {
    rebuilt.pos[StringMatchHash(static_cast<uint8_t>(primer[v - 2]),
                                static_cast<uint8_t>(primer[v - 1]))] =
        static_cast<uint32_t>(v);
  }
  return std::memcmp(rebuilt.pos.data(), generated, sizeof(rebuilt.pos)) == 0;
}

bool IsSpirvModelEligible(const uint8_t* data, size_t size) {
  if (size < 20 || (size % 4) != 0) {
    return false;
  }
  const uint32_t magic = LoadLe32(data);
  return magic == kSpirvMagic || magic == kSpirvMagicSwapped;
}

void EncodeModule(const uint8_t* data, size_t size,
                  std::vector<uint8_t>& out_payload) {
  ModelState model;
  RangeEncoder range(out_payload);
  BitEncoder bits(range);
  EncodeIO io(data, size);
  const Status status = CodeModule(model, bits, io, size / 4);
  (void)status;
  g_stat_cat = -1;
  bits.Flush();
}

void EncodeModuleRawResidual(const uint8_t* data, size_t size,
                             std::vector<uint8_t>& out_payload) {
  ModelState model;
  RawResidualEncoder bits(out_payload);
  EncodeIO io(data, size);
  const Status status = CodeModule(model, bits, io, size / 4);
  (void)status;
  bits.Flush();
}

#ifdef ARIXV_MEMSTATS
// Diagnostics build only: per-member heap and inline sizes of the model,
// accumulated over decodes. "touched" excludes deliberately uninitialized
// arrays, which only fault in where written.
std::map<std::string, std::pair<uint64_t, uint64_t>> g_model_members;
uint64_t g_model_decodes = 0;
namespace {
template <typename V>
size_t VecBytes(const V& v) {
  return v.capacity() * sizeof(typename V::value_type);
}
void RecordModelMemory(const ModelState& m) {
  ++g_model_decodes;
  const auto add = [](const char* name, size_t bytes, bool touched) {
    auto& e = g_model_members[name];
    e.first += bytes;
    e.second += touched ? bytes : 0;
  };
  add("ModelState inline (sizeof)", sizeof(ModelState), true);
  add("opcode_index_trees ptrs", VecBytes(m.opcode_index_trees), true);
  add("opcode_contexts", VecBytes(m.opcode_contexts), true);
  add("inst_contexts ptrs", VecBytes(m.inst_contexts), true);
  add("type_in_list", VecBytes(*m.type_in_list), true);
  add("type_rank", VecBytes(*m.type_rank), true);
  add("type_escape ptrs", VecBytes(m.type_escape), true);
  add("value_enums ptrs", VecBytes(m.value_enums), true);
  add("bit_enums ptrs", VecBytes(m.bit_enums), true);
  add("integer_values ptrs", VecBytes(m.integer_values), true);
  add("ext_inst_nums ptrs", VecBytes(m.ext_inst_nums), true);
  add("strings StringModel", sizeof(StringModel), true);
  add("strings history", VecBytes(m.strings->history), true);
  add("operand_bytes ByteModels", m.operand_bytes ? sizeof(*m.operand_bytes) : 0,
      true);
  add("tracker ID map (stamps touched)", m.tracker.IdMapBytes(), false);
  add("tracker ID map stamps", m.tracker.IdMapBytes() / (2 * 4 + sizeof(Tracker::IdInfo)) * 4, true);
  size_t strings = VecBytes(m.recent_strings);
  for (const std::string& str : m.recent_strings) strings += str.capacity() > 15 ? str.capacity() + 1 : 0;
  add("recent_strings", strings, true);
}
}  // namespace
#endif

Status DecodeModule(const uint8_t* payload, size_t payload_size,
                    size_t expected_original_size, std::vector<uint8_t>& out) {
  if (expected_original_size < 20 || (expected_original_size % 4) != 0) {
    return Status::kBadContainer;
  }
  out.clear();
  // Heap-allocated: ModelState's inline models are too large for the stack.
  const auto model = std::make_unique<ModelState>();
  RangeDecoder range(payload, payload_size);
  BitDecoder bits(range);
  DecodeIO io(out, expected_original_size);
  const Status status =
      CodeModule(*model, bits, io, expected_original_size / 4);
#ifdef ARIXV_MEMSTATS
  RecordModelMemory(*model);
#endif
  if (status != Status::kOk) {
    out.clear();
    return status;
  }
  if (!io.Complete()) {
    out.clear();
    return Status::kTruncated;
  }
  if (!bits.ConsumedExactly()) {
    out.clear();
    return Status::kBadContainer;
  }
  return Status::kOk;
}

Status DecodeModuleRawResidual(const uint8_t* payload, size_t payload_size,
                               size_t expected_original_size,
                               std::vector<uint8_t>& out) {
  if (expected_original_size < 20 || (expected_original_size % 4) != 0) {
    return Status::kBadContainer;
  }
  out.clear();
  const auto model = std::make_unique<ModelState>();
  RawResidualDecoder bits(payload, payload_size);
  DecodeIO io(out, expected_original_size);
  const Status status =
      CodeModule(*model, bits, io, expected_original_size / 4);
  if (status != Status::kOk) {
    out.clear();
    return status;
  }
  if (!io.Complete()) {
    out.clear();
    return Status::kTruncated;
  }
  if (!bits.AtEndWithZeroPadding()) {
    out.clear();
    return Status::kBadContainer;
  }
  return Status::kOk;
}

}  // namespace arixv
