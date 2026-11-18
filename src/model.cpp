// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "model.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <type_traits>
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
constexpr uint32_t kOpcodeOrder1MinCount = 64;
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

struct CountModel {
  Prob consistent;
  Prob same_as_last;
  UintModel extra;
};

struct OpcodeCtx {
  uint32_t stamp = 0;
  uint32_t count = 0;
  std::unique_ptr<BitTree<10>> tree;
};

struct ValueEnumModel {
  Prob known;
  std::unique_ptr<BitTree<8>> index;
  UintModel escape;

  ValueEnumModel() : index(std::make_unique<BitTree<8>>()) {}
};

struct BitEnumModel {
  std::vector<Prob> bits;
  Prob unknown;
  UintModel residual;
};

struct ExtInstNumberModel {
  Prob known;
  std::unique_ptr<BitTree<8>> index;
  UintModel escape;

  ExtInstNumberModel() : index(std::make_unique<BitTree<8>>()) {}
};

void ResetCountModel(CountModel& model) { model = CountModel{}; }

void ResetValueEnumModel(ValueEnumModel& model) {
  model.known = Prob{};
  *model.index = BitTree<8>{};
  model.escape = UintModel{};
}

void ResetBitEnumModel(BitEnumModel& model) {
  for (Prob& bit : model.bits) {
    bit = Prob{};
  }
  model.unknown = Prob{};
  model.residual = UintModel{};
}

void ResetExtInstNumberModel(ExtInstNumberModel& model) {
  model.known = Prob{};
  *model.index = BitTree<8>{};
  model.escape = UintModel{};
}

// Fixed-size POD array whose contents start *uninitialized*: every slot is
// generation-stamp-guarded at its use site, so zeroing at construction (what
// std::vector does) is pure per-decode waste for the stateless path.
template <typename T>
class UninitArray {
 public:
  explicit UninitArray(size_t count) : data_(new T[count]) {}
  T& operator[](size_t i) { return data_[i]; }
  const T& operator[](size_t i) const { return data_[i]; }

 private:
  std::unique_ptr<T[]> data_;
};

UintModel& EnsureUint(std::unique_ptr<UintModel>& model) {
  if (!model) {
    model = std::make_unique<UintModel>();
  }
  return *model;
}

template <typename T, typename Reset>
T& EnsureStamped(std::vector<std::unique_ptr<T>>& slots,
                 std::vector<uint32_t>& stamps, size_t key,
                 uint32_t generation, Reset reset) {
  std::unique_ptr<T>& slot = slots[key];
  if (!slot) {
    slot = std::make_unique<T>();
  } else if (stamps[key] != generation) {
    reset(*slot);
  }
  stamps[key] = generation;
  return *slot;
}

template <int kBits>
BitTree<kBits>& EnsureStampedTree(std::unique_ptr<BitTree<kBits>>& tree,
                                  uint32_t& stamp, uint32_t generation) {
  if (!tree) {
    tree = std::make_unique<BitTree<kBits>>();
  } else if (stamp != generation) {
    *tree = BitTree<kBits>{};
  }
  stamp = generation;
  return *tree;
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

struct ModelState {
  ModelState()
      : opcode_index_trees(
            static_cast<size_t>(Section::kCount) *
            (static_cast<size_t>(NumInstructionClasses()) + 1)),
        opcode_index_tree_stamp(opcode_index_trees.size()),
        opcode_contexts(NumCoreInstructions() + 1),
        count_models(NumCoreInstructions()),
        count_model_stamp(count_models.size()),
        last_word_count(NumCoreInstructions()),
        inst_group_stamp(NumCoreInstructions()),
        type_in_list(std::make_unique<std::vector<Prob>>(
            static_cast<size_t>(NumInstructionClasses()) + 1)),
        type_rank(std::make_unique<std::vector<BitTree<kRankTreeBits>>>(
            static_cast<size_t>(NumInstructionClasses()) + 1)),
        type_escape(static_cast<size_t>(NumInstructionClasses()) + 1),
        last_id(NumCoreInstructions() * 16u),
        last_id_stamp(NumCoreInstructions() * 16u),
        value_enums(static_cast<size_t>(NumOperandKinds()) * 16),
        value_enum_stamp(value_enums.size()),
        bit_enums(NumOperandKinds()),
        bit_enum_stamp(bit_enums.size()),
        integer_values(
            (static_cast<size_t>(NumInstructionClasses()) + 1) * 9),
        integer_value_stamp(integer_values.size()),
        last_literal(NumCoreInstructions() * 16u),
        last_literal_stamp(NumCoreInstructions() * 16u),
        ext_inst_nums(NumExtInstSets()),
        ext_inst_num_stamp(ext_inst_nums.size()),
        string_bytes(std::make_unique<ByteModel>()) {}

  // Must run before any last_id/last_literal/last_word_count access for
  // inst_index in a generation; CodeModule calls it once per instruction.
  void EnsureInstGroup(uint16_t inst_index) {
    if (inst_group_stamp[inst_index] == generation) {
      return;
    }
    inst_group_stamp[inst_index] = generation;
    const size_t base = static_cast<size_t>(inst_index) * 16;
    for (size_t i = 0; i < 16; ++i) {
      last_id_stamp[base + i] = 0;
      last_literal_stamp[base + i] = 0;
    }
    last_word_count[inst_index] = 0;
  }

  void BeginDecode() {
    AdvanceGeneration();
    tracker.Reset();
    swapped_flag = Prob{};
    version_canonical = Prob{};
    version_major = BitTree<4>{};
    version_minor = BitTree<4>{};
    BeginByteModelGeneration(generator_bytes);
    schema_zero = Prob{};
    bound_matches = Prob{};
    known_opcode.fill(Prob{});
    raw_known_count.reset();
    raw_unknown_count.reset();
    bound_value.reset();
    if (operand_bytes) {
      for (ByteModel& model : *operand_bytes) {
        BeginByteModelGeneration(model);
      }
    }
    prev_result_id = 0;
    result_next.fill(Prob{});
    result_in_fwd = Prob{};
    result_fwd_rank = BitTree<kRankTreeBits>{};
    result_delta = {};
    std::fill(type_in_list->begin(), type_in_list->end(), Prob{});
    for (BitTree<kRankTreeBits>& tree : *type_rank) {
      tree = BitTree<kRankTreeBits>{};
    }
    for (std::unique_ptr<UintModel>& model : type_escape) {
      model.reset();
    }
    id_in_list = {};
    id_rank = {};
    for (auto& bucket : id_escape) {
      for (std::unique_ptr<UintModel>& model : bucket) {
        model.reset();
      }
    }
    context_number = UintModel{};
    context_high_signext = Prob{};
    spec_constant_op.reset();
    ext_inst_unknown.reset();
    string_match = Prob{};
    string_rank = BitTree<6>{};
    string_canonical = Prob{};
    BeginByteModelGeneration(*string_bytes);
    recent_strings.clear();
  }

  void BeginRawDecode() {
    // The raw residual format keeps only structural prediction state. None of
    // the adaptive probability arrays participate in decoding, so resetting
    // them would distort the ceiling this experiment is intended to measure.
    AdvanceGeneration();
    tracker.Reset();
    prev_result_id = 0;
    recent_strings.clear();
  }

  Tracker tracker;
  uint32_t generation = 1;
  Prob swapped_flag;
  Prob version_canonical;
  BitTree<4> version_major;
  BitTree<4> version_minor;
  ByteModel generator_bytes;
  Prob schema_zero;
  Prob bound_matches;

  std::array<Prob, static_cast<size_t>(Section::kCount) * 2> known_opcode{};
  std::vector<std::unique_ptr<BitTree<10>>> opcode_index_trees;
  std::vector<uint32_t> opcode_index_tree_stamp;
  std::unique_ptr<BitTree<16>> unknown_opcode_tree;
  uint32_t unknown_opcode_tree_stamp = 0;
  std::vector<OpcodeCtx> opcode_contexts;
  std::vector<std::unique_ptr<CountModel>> count_models;
  std::vector<uint32_t> count_model_stamp;
  UninitArray<uint32_t> last_word_count;
  // Two-level stamps for the per-(instruction x slot) caches: one stamp per
  // instruction here; EnsureInstGroup zeroes that instruction's 16-slot
  // stamp groups (and last word count) on its first touch each generation,
  // so the per-slot stamp arrays are never bulk-cleared and start
  // uninitialized. Per-decode clearing is 3.5 KB plus 128 B per distinct
  // opcode used, instead of 115 KB.
  std::vector<uint32_t> inst_group_stamp;
  // Models on rare paths (corpus-measured: raw counts 1/1080 files, exact
  // bound mispredictions 27/1080, opaque words 4/1080) are allocated on
  // first use so typical decodes never pay for their construction.
  std::unique_ptr<UintModel> raw_known_count;
  std::unique_ptr<UintModel> raw_unknown_count;
  std::unique_ptr<UintModel> bound_value;
  std::unique_ptr<std::array<ByteModel, 4>> operand_bytes;

  uint32_t prev_result_id = 0;
  std::array<Prob, 2> result_next{};
  Prob result_in_fwd;
  BitTree<kRankTreeBits> result_fwd_rank;
  std::array<UintModel, 4> result_delta;

  std::unique_ptr<std::vector<Prob>> type_in_list;
  std::unique_ptr<std::vector<BitTree<kRankTreeBits>>> type_rank;
  // Escape models allocate per slot on first escape: every module escapes
  // somewhere, but through only a handful of the 27/29 slots.
  std::vector<std::unique_ptr<UintModel>> type_escape;

  std::array<std::array<Prob, 9>, 3> id_in_list{};
  std::array<std::array<BitTree<kRankTreeBits>, 9>, 3> id_rank{};
  std::array<std::array<std::unique_ptr<UintModel>, 9>, 3> id_escape{};
  UninitArray<uint32_t> last_id;
  UninitArray<uint32_t> last_id_stamp;  // valid only under inst_group_stamp

  std::vector<std::unique_ptr<ValueEnumModel>> value_enums;
  std::vector<uint32_t> value_enum_stamp;
  std::vector<std::unique_ptr<BitEnumModel>> bit_enums;
  std::vector<uint32_t> bit_enum_stamp;

  std::vector<std::unique_ptr<UintModel>> integer_values;
  std::vector<uint32_t> integer_value_stamp;
  UninitArray<uint32_t> last_literal;
  UninitArray<uint32_t> last_literal_stamp;  // valid under inst_group_stamp
  UintModel context_number;
  Prob context_high_signext;
  std::unique_ptr<UintModel> spec_constant_op;  // rare: 16/1080 files

  std::vector<std::unique_ptr<ExtInstNumberModel>> ext_inst_nums;
  std::vector<uint32_t> ext_inst_num_stamp;
  std::unique_ptr<UintModel> ext_inst_unknown;  // rare: 4/1080 files

  Prob string_match;
  BitTree<6> string_rank;
  Prob string_canonical;
  std::unique_ptr<ByteModel> string_bytes;
  std::vector<std::string> recent_strings;

 private:
  void AdvanceGeneration() {
    ++generation;
    if (generation != 0) {
      return;
    }
    generation = 1;
    opcode_index_tree_stamp.assign(opcode_index_tree_stamp.size(), 0);
    unknown_opcode_tree_stamp = 0;
    for (OpcodeCtx& ctx : opcode_contexts) {
      ctx.stamp = 0;
    }
    count_model_stamp.assign(count_model_stamp.size(), 0);
    inst_group_stamp.assign(inst_group_stamp.size(), 0);
    value_enum_stamp.assign(value_enum_stamp.size(), 0);
    bit_enum_stamp.assign(bit_enum_stamp.size(), 0);
    integer_value_stamp.assign(integer_value_stamp.size(), 0);
    ext_inst_num_stamp.assign(ext_inst_num_stamp.size(), 0);
  }
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
  uint32_t BoundHint() const { return words_[3]; }
  void EmitHeaderWord(uint32_t word) {
    if (emitted_ != 3) {
      assert(word == words_[emitted_]);
    }
    ++emitted_;
  }
  void EmitBodyWord(uint32_t word) { assert(word == words_[emitted_++]); }
  void PatchBound(uint32_t word) { assert(word == words_[3]); }

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
  uint32_t BoundHint() const { return 0; }

  void SetSwapped(bool swapped) { swapped_ = swapped; }

  void EmitHeaderWord(uint32_t word) { Append(word); }
  void EmitBodyWord(uint32_t word) { Append(word); }

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

 private:
  void Append(uint32_t word) {
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

template <class Bits>
uint32_t CodeGenerator(Bits& bits, ModelState& model, uint32_t hint) {
  uint8_t prev = 0;
  uint32_t out = 0;
  for (int byte = 0; byte < 4; ++byte) {
    const uint8_t b = CodeByte(
        bits, model.generator_bytes, prev,
        static_cast<uint8_t>((hint >> (byte * 8)) & 0xffu));
    out |= static_cast<uint32_t>(b) << (byte * 8);
    prev = b;
  }
  return Bits::kEncoding ? hint : out;
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
    // A freshly constructed ByteModel behaves exactly like a generation-
    // bumped one (trees are stamp-guarded), so lazy creation is
    // deterministic on both sides.
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

bool IsGrammarConsistent(const InstructionDesc* desc, uint32_t count) {
  if (!desc) {
    return false;
  }
  return desc->fixed_word_count ? count == desc->min_word_count
                                : count >= desc->min_word_count;
}

uint32_t ZigZag32(uint32_t delta) {
  return (delta << 1) ^ (0u - (delta >> 31));
}

uint32_t UnZigZag32(uint32_t value) {
  return (value >> 1) ^ (0u - (value & 1u));
}

template <class Bits>
uint32_t CodeDelta(Bits& bits, UintModel& model, uint32_t value,
                   uint32_t base) {
  const uint32_t hint = ZigZag32(value - base);
  const uint32_t coded = CodeUint(bits, model, hint);
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

uint16_t PreviousOpcodeKey(const Tracker& tracker) {
  const uint16_t sentinel = NumCoreInstructions();
  const uint16_t prev_opcode = tracker.prev_opcode();
  if (tracker.prev_opcode_class() == 0xFF) {
    return sentinel;
  }
  const int index = FindInstructionIndex(prev_opcode);
  if (index < 0 || index >= NumCoreInstructions()) {
    return sentinel;
  }
  return static_cast<uint16_t>(index);
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
                      std::unique_ptr<UintModel>& escape,
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
  return CodeDelta(bits, EnsureUint(escape), hint, base);
}


template <class Bits>
uint32_t CodeResultId(Bits& bits, ModelState& model,
                      const InstructionDesc* desc, uint32_t hint) {
  const uint32_t predicted = model.prev_result_id + 1;
  const bool sequential = Bits::kEncoding && hint == predicted;
  const size_t ctx = model.tracker.in_function() ? 1u : 0u;
  uint32_t id = predicted;
  if (!bits.CodeBit(model.result_next[ctx], sequential)) {
    const RecencyList& fwd = model.tracker.forward_refs();
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
      id = CodeDelta(bits, model.result_delta[ResultClassBucket(desc)], hint,
                     predicted);
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
                             model.type_escape[cls],
                             model.tracker.recent_types(), hint,
                             model.prev_result_id);
  model.tracker.OnIdUse(id);
  return Bits::kEncoding ? hint : id;
}

template <class Bits>
uint32_t CodeIdRef(Bits& bits, ModelState& model, uint16_t inst_index,
                   const OperandCursor::Field& field, uint32_t hint) {
  const int bucket = IdCategoryBucket(field.category);
  const uint32_t slot = Slot8(field.slot);
  const size_t key = static_cast<size_t>(inst_index) * 16 + Slot16(field.slot);
  const uint32_t base =
      model.last_id_stamp[key] == model.generation ? model.last_id[key]
                                                   : model.prev_result_id;
  uint32_t id = CodeRankedId(bits, model.id_in_list[bucket][slot],
                             model.id_rank[bucket][slot],
                             model.id_escape[bucket][slot],
                             model.tracker.recent_ids(), hint, base);
  model.last_id[key] = id;
  model.last_id_stamp[key] = model.generation;
  model.tracker.OnIdUse(id);
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
  const size_t key = static_cast<size_t>(field.kind) * 16 + Slot16(field.slot);
  ValueEnumModel& ctx =
      EnsureStamped(model.value_enums, model.value_enum_stamp, key,
                    model.generation, ResetValueEnumModel);
  if (bits.CodeBit(ctx.known, known)) {
    index = ctx.index->CodeValue(bits, index);
    if (!Bits::kEncoding && index < kind.num_enumerants) {
      return GetEnumerant(kind, static_cast<uint16_t>(index)).value;
    }
    return Bits::kEncoding ? hint : 0;
  }
  return CodeUint(bits, ctx.escape, hint);
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
      EnsureStamped(model.bit_enums, model.bit_enum_stamp, field.kind,
                    model.generation, ResetBitEnumModel);
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
    value |= CodeUint(bits, ctx.residual, residual_hint);
  }
  return Bits::kEncoding ? hint : value;
}

template <class Bits>
uint32_t CodeLiteralInteger(Bits& bits, ModelState& model,
                            const InstructionDesc* desc,
                            const OperandCursor::Field& field,
                            uint16_t inst_index, uint32_t hint) {
  const size_t key = static_cast<size_t>(inst_index) * 16 + Slot16(field.slot);
  if (model.last_literal_stamp[key] != model.generation) {
    model.last_literal[key] = 0;
    model.last_literal_stamp[key] = model.generation;
  }
  const uint32_t base = model.last_literal[key];
  if constexpr (Bits::kRawResidual) {
    const uint32_t delta = Bits::kEncoding ? ZigZag32(hint - base) : 0;
    const uint32_t coded = bits.CodeUint32(delta);
    const uint32_t value = Bits::kEncoding ? hint : base + UnZigZag32(coded);
    model.last_literal[key] = value;
    return Bits::kEncoding ? hint : value;
  }
  const size_t cls = ClassBucket(desc);
  UintModel& ctx =
      EnsureStamped(model.integer_values, model.integer_value_stamp,
                    cls * 9 + Slot8(field.slot), model.generation,
                    [](UintModel& value) { value = UintModel{}; });
  const uint32_t value = CodeDelta(bits, ctx, hint, base);
  model.last_literal[key] = value;
  return Bits::kEncoding ? hint : value;
}

template <class Bits>
uint32_t CodeContextNumber(Bits& bits, ModelState& model, uint32_t hint) {
  return CodeUint(bits, model.context_number, hint);
}

template <class Bits>
uint32_t CodeSpecConstantOp(Bits& bits, ModelState& model, uint32_t hint) {
  if constexpr (Bits::kRawResidual) {
    (void)model;
    return bits.CodeUint32(hint);
  }
  return CodeUint(bits, EnsureUint(model.spec_constant_op), hint);
}

template <class Bits>
uint32_t CodeExtInstNumber(Bits& bits, ModelState& model,
                           const ExtInstSetDesc* set, uint32_t hint) {
  if (!set) {
    if constexpr (Bits::kRawResidual) {
      return bits.CodeUint32(hint);
    }
    return CodeUint(bits, EnsureUint(model.ext_inst_unknown), hint);
  }
  const int set_index = FindExtInstSetIndex(set);
  if (set_index < 0) {
    if constexpr (Bits::kRawResidual) {
      return bits.CodeUint32(hint);
    }
    return CodeUint(bits, EnsureUint(model.ext_inst_unknown), hint);
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
  ExtInstNumberModel& ctx =
      EnsureStamped(model.ext_inst_nums, model.ext_inst_num_stamp,
                    static_cast<size_t>(set_index), model.generation,
                    ResetExtInstNumberModel);
  if (bits.CodeBit(ctx.known, known)) {
    index = ctx.index->CodeValue(bits, index);
    if (!Bits::kEncoding && index < set->num_instructions) {
      return set->instructions[index].opcode;
    }
    return Bits::kEncoding ? hint : 0;
  }
  return CodeUint(bits, ctx.escape, hint);
}

template <class Bits, size_t kInlineWords>
void CodeString(Bits& bits, ModelState& model, const uint32_t* hint_words,
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
  } else {
    const bool canonical = Bits::kEncoding && IsCanonicalStringBytes(raw);
    if (bits.CodeBit(model.string_canonical, canonical)) {
      const std::string source = raw;
      std::string coded;
      uint8_t prev = 0;
      while (coded.size() < static_cast<size_t>(remaining) * 4u) {
        const uint8_t hint =
            Bits::kEncoding ? static_cast<uint8_t>(source[coded.size()]) : 0;
        const uint8_t b = CodeByte(bits, *model.string_bytes, prev, hint);
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
    const uint8_t prev_class =
        prev_known ? model.tracker.prev_opcode_class()
                   : NumInstructionClasses();
    const size_t tree_ctx =
        static_cast<size_t>(section) *
            (static_cast<size_t>(NumInstructionClasses()) + 1) +
        prev_class;
    BitTree<10>& tree =
        EnsureStampedTree(model.opcode_index_trees[tree_ctx],
                          model.opcode_index_tree_stamp[tree_ctx],
                          model.generation);
    const uint16_t prev_key = PreviousOpcodeKey(model.tracker);
    OpcodeCtx& order1 = model.opcode_contexts[prev_key];
    if (order1.stamp != model.generation) {
      order1.count = 0;
      if (order1.tree) {
        *order1.tree = BitTree<10>{};
      }
      order1.stamp = model.generation;
    }
    uint32_t index = 0;
    if (Bits::kEncoding) {
      const int found = FindInstructionIndex(hint);
      assert(found >= 0);
      index = static_cast<uint32_t>(found);
    }
    if (order1.count < kOpcodeOrder1MinCount) {
      index = tree.CodeValue(bits, index);
      if (!order1.tree) {
        order1.tree = std::make_unique<BitTree<10>>();
      }
      order1.tree->Train(index);
    } else {
      if (!order1.tree) {
        order1.tree = std::make_unique<BitTree<10>>();
      }
      index = order1.tree->CodeValue(bits, index);
    }
    if (order1.count != 0xffffffffu) {
      ++order1.count;
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
        EnsureStampedTree(model.unknown_opcode_tree,
                          model.unknown_opcode_tree_stamp, model.generation)
            .CodeValue(bits, hint));
    *desc_out = nullptr;
    *inst_index_out = NumCoreInstructions();
  }
  return opcode;
}

template <class Bits>
uint32_t CodeWordCount(ModelState& model, Bits& bits, uint16_t inst_index,
                       const InstructionDesc* desc,
                       uint32_t hint, bool* opaque) {
  *opaque = true;
  uint32_t count = hint;
  if (!desc) {
    if constexpr (Bits::kRawResidual) {
      return bits.CodeUint32(hint) & 0xffffu;
    }
    count = CodeUint(bits, EnsureUint(model.raw_unknown_count), hint) & 0xffffu;
    return count;
  }

  if constexpr (Bits::kRawResidual) {
    const bool consistent =
        bits.CodeBit(model.schema_zero, IsGrammarConsistent(desc, hint));
    if (!consistent) {
      return bits.CodeUint32(hint) & 0xffffu;
    }
    *opaque = false;
    if (desc->fixed_word_count) {
      return desc->min_word_count;
    }
    const uint32_t last_count = model.last_word_count[inst_index];
    if (bits.CodeBit(model.schema_zero,
                     Bits::kEncoding && hint == last_count)) {
      count = last_count;
    } else {
      const uint32_t extra = Bits::kEncoding ? hint - desc->min_word_count : 0;
      count = (desc->min_word_count + bits.CodeUint32(extra)) & 0xffffu;
    }
    model.last_word_count[inst_index] = count;
    return count;
  }

  CountModel& count_model =
      EnsureStamped(model.count_models, model.count_model_stamp, inst_index,
                    model.generation, ResetCountModel);
  const bool hint_consistent = IsGrammarConsistent(desc, hint);
  const bool consistent =
      bits.CodeBit(count_model.consistent, hint_consistent);
  if (consistent) {
    *opaque = false;
    if (desc->fixed_word_count) {
      count = desc->min_word_count;
    } else {
      // EnsureInstGroup zeroed last_word_count on this instruction's first
      // touch this generation.
      const uint32_t last_count = model.last_word_count[inst_index];
      if (bits.CodeBit(count_model.same_as_last,
                       Bits::kEncoding && hint == last_count)) {
        count = last_count;
      } else {
        const uint32_t extra_hint = Bits::kEncoding
                                        ? hint - desc->min_word_count
                                        : 0;
        count = (desc->min_word_count +
                 CodeUint(bits, count_model.extra, extra_hint)) &
                0xffffu;
      }
      model.last_word_count[inst_index] = count;
    }
  } else {
    count = CodeUint(bits, EnsureUint(model.raw_known_count), hint) & 0xffffu;
  }
  return count;
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
  WordBuffer<kInlineInstructionWords> inst_words;
  WordBuffer<kInlineOperandWords> coded_words;
  std::vector<uint32_t> string_hints;
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
      model.EnsureInstGroup(inst_index);
    }

    bool opaque = true;
    stat(kStatWordCount);
    const uint32_t declared_count =
        CodeWordCount(model, bits, inst_index, desc, count_hint, &opaque);
    const uint32_t word0 = (declared_count << 16) | opcode;
    inst_words.clear();
    const size_t remaining_body = body_words - body_pos;
    const uint32_t operand_words =
        declared_count == 0
            ? 0
            : static_cast<uint32_t>(
                  std::min<size_t>(declared_count - 1,
                                   remaining_body > 0 ? remaining_body - 1
                                                      : 0));
    inst_words.push_back(word0);

    model.tracker.OnInstructionStart(opcode, desc, declared_count);
    io.EmitBodyWord(word0);

    std::array<uint8_t, 4> prev_bytes = {0, 0, 0, 0};
    OperandCursor cursor;
    // Fixed layouts have only required, one-word operands and no enumerant
    // parameters. Their slot list therefore describes every decoded field
    // directly, so avoid the general cursor state machine on this common path.
    const bool fixed_layout =
        desc != nullptr && desc->fixed_word_count && !opaque;
    const OperandSlotDesc* fixed_slots =
        fixed_layout ? GetOperandSlots(*desc) : nullptr;
    if (!fixed_layout) {
      cursor.Begin(desc, declared_count, opaque);
    }
    uint32_t consumed = 0;
    uint32_t current_result_type = 0;
    const ExtInstSetDesc* current_ext_set = nullptr;
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
        if (field.type == OperandCursor::FieldType::kEnd) {
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
          case OperandCategory::kIdResult:
            stat(kStatResultId);
            stage_single_word(CodeResultId(bits, model, desc, hint0));
            break;
          case OperandCategory::kIdResultType: {
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
            const uint32_t word =
                CodeIdRef(bits, model, inst_index, field, hint0);
            stage_single_word(word);
            if (desc && desc->opcode == 12 && field.slot == 2) {
              const Tracker::IdInfo* info = model.tracker.Find(word);
              current_ext_set = info ? info->ext_set : nullptr;
              cursor.SetExtInstSet(current_ext_set);
            }
            break;
          }
          case OperandCategory::kValueEnum:
            stat(kStatValueEnum);
            stage_single_word(CodeValueEnum(bits, model, field, hint0));
            break;
          case OperandCategory::kBitEnum:
            stat(kStatBitEnum);
            stage_single_word(CodeBitEnum(bits, model, field, hint0));
            break;
          case OperandCategory::kLiteralInteger:
            stat(kStatLitInt);
            stage_single_word(CodeLiteralInteger(bits, model, desc, field,
                                                 inst_index, hint0));
            break;
          case OperandCategory::kLiteralContextDependentNumber: {
            stat(kStatLitCdn);
            consume_span =
                SpanForField(field, desc, current_result_type, model.tracker,
                             inst_words.data(),
                             static_cast<uint32_t>(inst_words.size()),
                             remaining);
            const uint32_t actual_span = std::min(consume_span, remaining);
            if (actual_span == 1) {
              stage_single_word(CodeContextNumber(bits, model, hint0));
            } else if (actual_span >= 2 &&
                model.tracker.TypeWidth(current_result_type) > 32) {
              const uint32_t low = CodeContextNumber(bits, model, hint0);
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
                    CodeContextNumber(bits, model, high_hint));
              }
            } else {
              for (uint32_t i = 0; i < actual_span; ++i) {
                coded_words.push_back(CodeContextNumber(
                    bits, model,
                    Bits::kEncoding ? io.BodyWord(body_pos + 1 + consumed + i)
                                    : 0));
              }
            }
            break;
          }
          case OperandCategory::kLiteralExtInstInteger:
            stat(kStatExtNum);
            stage_single_word(
                CodeExtInstNumber(bits, model, current_ext_set, hint0));
            break;
          case OperandCategory::kLiteralString: {
            stat(kStatString);
            const uint32_t* string_hint_ptr = nullptr;
            if constexpr (Bits::kEncoding) {
              string_hints.clear();
              string_hints.reserve(remaining);
              for (uint32_t i = 0; i < remaining; ++i) {
                string_hints.push_back(io.BodyWord(body_pos + 1 + consumed + i));
              }
              string_hint_ptr = string_hints.data();
            }
            CodeString(bits, model, string_hint_ptr, remaining, &consume_span,
                       coded_words);
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

    model.tracker.OnInstructionEnd(inst_words.data(),
                                   static_cast<uint32_t>(inst_words.size()));
    body_pos += 1 + operand_words;
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
                        return CodeUint(bits, EnsureUint(model.bound_value),
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

struct ModuleDecoder::State {
  ModelState model;
};

ModuleDecoder::ModuleDecoder() : state_(std::make_unique<State>()) {}
ModuleDecoder::~ModuleDecoder() = default;
ModuleDecoder::ModuleDecoder(ModuleDecoder&&) noexcept = default;
ModuleDecoder& ModuleDecoder::operator=(ModuleDecoder&&) noexcept = default;

Status ModuleDecoder::Decode(const uint8_t* payload, size_t payload_size,
                             size_t expected_original_size,
                             std::vector<uint8_t>& out) {
  if (expected_original_size < 20 || (expected_original_size % 4) != 0) {
    return Status::kBadContainer;
  }
  out.clear();
  state_->model.BeginDecode();
  RangeDecoder range(payload, payload_size);
  BitDecoder bits(range);
  DecodeIO io(out, expected_original_size);
  const Status status =
      CodeModule(state_->model, bits, io, expected_original_size / 4);
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

Status DecodeModule(const uint8_t* payload, size_t payload_size,
                    size_t expected_original_size, std::vector<uint8_t>& out) {
  ModuleDecoder decoder;
  return decoder.Decode(payload, payload_size, expected_original_size, out);
}

struct RawResidualModuleDecoder::State {
  ModelState model;
};

RawResidualModuleDecoder::RawResidualModuleDecoder()
    : state_(std::make_unique<State>()) {}
RawResidualModuleDecoder::~RawResidualModuleDecoder() = default;
RawResidualModuleDecoder::RawResidualModuleDecoder(
    RawResidualModuleDecoder&&) noexcept = default;
RawResidualModuleDecoder& RawResidualModuleDecoder::operator=(
    RawResidualModuleDecoder&&) noexcept = default;

Status RawResidualModuleDecoder::Decode(const uint8_t* payload,
                                        size_t payload_size,
                                        size_t expected_original_size,
                                        std::vector<uint8_t>& out) {
  if (expected_original_size < 20 || (expected_original_size % 4) != 0) {
    return Status::kBadContainer;
  }
  out.clear();
  state_->model.BeginRawDecode();
  RawResidualDecoder bits(payload, payload_size);
  DecodeIO io(out, expected_original_size);
  const Status status =
      CodeModule(state_->model, bits, io, expected_original_size / 4);
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

Status DecodeModuleRawResidual(const uint8_t* payload, size_t payload_size,
                               size_t expected_original_size,
                               std::vector<uint8_t>& out) {
  RawResidualModuleDecoder decoder;
  return decoder.Decode(payload, payload_size, expected_original_size, out);
}

}  // namespace arixv
