// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

// SPIR-V structural state machine, shared verbatim by encoder and decoder.
//
// THE invariant: tracker state is a deterministic function of
// the instruction words observed so far. The encoder feeds it input words;
// the decoder feeds it reconstructed words; all queries and mutations happen
// from *shared* templated model code in the same order on both sides, so the
// two can never diverge. Mutation entry points are the On*() methods below;
// everything else is const.
//
// Memory: sparse storage only — never sized by the header `bound` or by raw
// ID values (hostile input can declare huge IDs). All lists have fixed caps
// (tuning.h) with drop-oldest eviction. Total memory is O(input size).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "grammar_tables.h"
#include "operand_cursor.h"
#include "tuning.h"

namespace arixv {

// Logical module sections. A *context input*, advanced
// monotonically from observed opcodes — never an enforced rule (out-of-order
// input just predicts poorly).
enum class Section : uint8_t {
  kCapabilities = 0,
  kExtensions,
  kExtInstImports,
  kMemoryModel,
  kEntryPoints,
  kExecutionModes,
  kDebug,
  kAnnotations,
  kTypesConstantsVars,
  kFunctionDecls,
  kFunctionBodies,
  kCount,
};

constexpr int MaxRecencyListCap(int a, int b) { return a > b ? a : b; }
constexpr int kRecencyListStorageCap =
    MaxRecencyListCap(MaxRecencyListCap(kRankListCap, kRankListCapIds),
                      kRankListCapTypes);
static_assert(kRecencyListStorageCap <= 31,
              "RecencyList match masks are 32-bit");

// SSE2 (baseline on x86-64) fast path for the 16-entry recency lists.
#if defined(__SSE2__) && !defined(ARIXV_NO_SIMD)
#define ARIXV_RECENCY_SSE2 1
#else
#define ARIXV_RECENCY_SSE2 0
#endif
#if ARIXV_RECENCY_SSE2
static_assert(kRecencyListStorageCap == 16,
              "SSE2 recency path handles exactly four 4-lane vectors");
#endif

// Fixed-capacity move-to-front recency list for rank coding.
class RecencyList {
 public:
  explicit RecencyList(int cap = kRankListCap) : cap_(cap) {}
  // Rank of id (0 = most recent), or -1 if absent.
  int Find(uint32_t id) const;
  // Move-to-front (inserting if absent, evicting the oldest past cap).
  void Touch(uint32_t id);
  // Moves old_id's entry to the front holding `id`; if old_id is absent,
  // inserts id at the front without checking whether it is already listed.
  void Replace(uint32_t old_id, uint32_t id);
  void Remove(uint32_t id);
  int size() const { return size_; }
  uint32_t at(int rank) const { return items_[static_cast<size_t>(rank)]; }
  void Clear() { size_ = 0; }

 private:
  // Bit i set iff live entry i equals id.
  uint32_t MatchMask(uint32_t id) const;
  // Writes id at slot 0 after moving slots [0, last) up by one.
  void ShiftIn(int last, uint32_t id);

  int cap_;
  int size_ = 0;
  alignas(16) std::array<uint32_t, kRecencyListStorageCap> items_{};  // front = most recent
};


class Tracker {
 public:
  // Coarse classification of a *type* ID's type.
  enum class TypeClass : uint8_t {
    kOther = 0, kVoid, kBool, kInt, kFloat, kVector, kMatrix, kPointer,
    kStruct, kArray, kRuntimeArray, kImage, kSampler, kSampledImage,
    kFunction, kCount,
  };

  enum class IdKind : uint8_t {
    kUnknown = 0,  // referenced but not yet defined (forward reference)
    kType, kConstant, kVariable, kFunction, kLabel, kExtInstSet, kValue,
    kCount,
  };

  // A plain aggregate: IdInfo{} (all zero) is the default — kUnknown, not
  // defined, kOther, width 0, no ext set — which lets the ID map allocate its
  // value array uninitialized. Always create one with IdInfo{}.
  struct IdInfo {
    IdKind kind;
    bool defined;   // result ID has been seen
    // For kind==kType: what type this ID declares.
    TypeClass type_class;
    uint32_t width;         // OpTypeInt/OpTypeFloat bit width
    const ExtInstSetDesc* ext_set;  // kind==kExtInstSet
    uint32_t result_type;   // the defining instruction's Result Type, or 0
    // For kind==kType: the declaration's first ID operand — the pointee of a
    // pointer, the component of a vector, the element of an array, the
    // return type of a function type — or 0.
    uint32_t element_type;
    // For a type ID: 1 + index of the recency list of IDs of that type
    // (typed_lists_), or 0 before its first value.
    uint32_t values_list;
    // Recency list handle for this ID (its type's values_list, a unified kind
    // list, or kRecentTypesList for types), copied at definition so a use
    // needs no second lookup; 0 if none.
    uint32_t type_list;
  };
  static_assert(std::is_trivially_default_constructible_v<IdInfo> &&
                    static_cast<int>(IdKind::kUnknown) == 0 &&
                    static_cast<int>(TypeClass::kOther) == 0,
                "IdInfo{} must be the all-zero default");

  Tracker();

  // ---- Context inputs (const queries during coding) --------------------
  Section section() const { return section_; }
  uint16_t prev_opcode() const { return prev_opcode_; }
  // Core-table index of the previous instruction; meaningful only when
  // prev_opcode_class() != 0xFF (a known previous instruction).
  uint16_t prev_instruction_index() const { return prev_inst_index_; }
  uint8_t prev_opcode_class() const { return prev_opcode_class_; }
  bool in_function() const { return in_function_; }
  bool in_block() const { return in_block_; }
  uint32_t max_seen_id() const { return max_seen_id_; }
  uint32_t instruction_index() const { return instruction_index_; }

  // Null if the ID was never seen (neither defined nor referenced).
  const IdInfo* Find(uint32_t id) const;

  // Width of the type `type_id` if it is a known int/float type (for
  // LiteralContextDependentNumber), else 0.
  uint32_t TypeWidth(uint32_t type_id) const;
  // Result Type of the instruction that defined id (0 if unknown).
  uint32_t ResultTypeOf(uint32_t id) const {
    const IdInfo* info = Find(id);
    return info ? info->result_type : 0;
  }
  // Sentinel list handle for IdKind::kType, aliasing recent_types_ instead of
  // maintaining a duplicate list in typed_lists_.
  static constexpr uint32_t kRecentTypesList = ~uint32_t{0};

  // Recency list number `list` (an IdInfo::type_list / values_list value:
  // 1-based, 0 = none, kRecentTypesList = recent_types_), most recent first;
  // null for 0. Valid until the next tracker update.
  const RecencyList* TypedList(uint32_t list) const {
    if (list != 0 && list <= typed_lists_.size()) {
      return &typed_lists_[list - 1];
    }
    return list == kRecentTypesList ? &recent_types_ : nullptr;
  }
  // Element type recorded for a type ID (0 if none).
  uint32_t ElementTypeOf(uint32_t type_id) const {
    const IdInfo* info = Find(type_id);
    return info ? info->element_type : 0;
  }

  // ---- Candidate lists for rank coding (M3) -----------------------------
  const RecencyList& recent_ids() const { return recent_ids_; }
  const RecencyList& recent_types() const { return recent_types_; }
  const RecencyList& forward_refs() const { return forward_refs_; }
  // Ends of recent result-ID runs, most recent first: a result ID r that
  // continues a run (r - 1 listed) replaces that entry at the front.
  const RecencyList& result_runs() const { return result_runs_; }

  // ---- Mutations (called from shared model code only) --------------------
  // Order per instruction, identical on both sides:
  //   OnInstructionStart(opcode, desc, word_count)
  //   per ID operand, right after it is coded: OnIdUse(id)
  //   right after the result ID is coded:      OnResultId(id)
  //   after all words are known:               OnInstructionEnd(words, count)
  // OnInstructionEnd receives the complete instruction (including word0) and
  // performs the semantic update: section advance, ID table (types,
  // constants, functions, labels, ext-inst sets), function/block state,
  // recency-list definition touches, prev-opcode context.
  void OnInstructionStart(uint32_t opcode, const InstructionDesc* desc,
                          uint32_t word_count);
  // Use of an ID operand (coded from recent_ids()).
  // What OnIdUse read from the ID's entry: its Result Type and its type's
  // recency list (0 if none).
  struct IdUse {
    uint32_t result_type;
    uint32_t type_list;
  };
  IdUse OnIdUse(uint32_t id, bool touch_recent_ids = true);
  // Use of an ID in a result-type slot (coded from recent_types()); unlike
  // OnIdUse it leaves recent_ids() alone.
  void OnResultTypeUse(uint32_t id);
  void OnResultId(uint32_t id);
  // inst_index: the instruction's core-table index (NumCoreInstructions()
  // for an unknown opcode), remembered as prev_instruction_index().
  // first_id_operand: the instruction's first ID operand (after any Result
  // Type / Result ID), or 0; recorded as a type declaration's element type.
  void OnInstructionEnd(const uint32_t* words, uint32_t count,
                        uint16_t inst_index, uint32_t first_id_operand);

  void Reserve(size_t word_count);
  // Heap bytes held by the ID map (diagnostics).
  size_t IdMapBytes() const { return ids_.Bytes(); }
  void Reset();

 private:
  // Open-addressing hash map from ID to IdInfo. Linear probing over
  // power-of-two key/value arrays; a slot is occupied iff its generation
  // stamp equals the map's current generation, and ID 0 (invalid SPIR-V, but
  // reachable from corrupt input) gets a dedicated out-of-table slot. Grows by
  // doubling; Clear() keeps capacity and, instead of zeroing the whole key
  // array, just bumps the generation so all slots read as empty in O(1). The
  // stamp array is only bulk-reset on the (practically never) generation wrap.
  class IdMap {
   public:
    const IdInfo* Find(uint32_t id) const;
    IdInfo* FindMutable(uint32_t id) {
      return const_cast<IdInfo*>(static_cast<const IdMap*>(this)->Find(id));
    }
    IdInfo& Ensure(uint32_t id);
    size_t Bytes() const {
      return capacity_ * (2 * sizeof(uint32_t) + sizeof(IdInfo));
    }
    void Clear();
    // Prepare for roughly `expected_ids` distinct IDs. The count derives from
    // the untrusted header word count, so the resulting allocation is capped
    // (see kMaxReserveSlots in tracker.cpp); real modules past the cap just
    // pay a few doublings.
    void Reserve(size_t expected_ids);

   private:
    void Rehash(size_t slot_count);

    // Parallel arrays of capacity_ (a power of two) slots. Only slot_gen_ is
    // initialized: a slot's key and value are written when it is claimed,
    // and occupancy is decided by the stamp alone.
    std::unique_ptr<uint32_t[]> keys_;
    std::unique_ptr<IdInfo[]> values_;
    std::unique_ptr<uint32_t[]> slot_gen_;  // == generation_ if occupied
    size_t capacity_ = 0;
    uint32_t generation_ = 0;         // bumped by Clear(); 0 => all slots empty
    size_t size_ = 0;                 // occupied slots in the current generation
    size_t mask_ = 0;                 // capacity_ - 1 once allocated
    bool has_zero_ = false;
    IdInfo zero_info_{};
  };

  IdInfo& Ensure(uint32_t id);

  Section section_ = Section::kCapabilities;
  uint16_t prev_opcode_ = 0xFFFF;
  uint16_t prev_inst_index_ = 0xFFFF;
  uint8_t prev_opcode_class_ = 0xFF;
  bool in_function_ = false;
  bool in_block_ = false;
  uint32_t max_seen_id_ = 0;
  uint32_t instruction_index_ = 0;

  IdMap ids_;

  RecencyList recent_ids_{kRankListCapIds};
  RecencyList recent_types_{kRankListCapTypes};
  RecencyList forward_refs_{kRankListCapIds};
  RecencyList result_runs_{kRankListCap};
  // Per-type and per-kind recency lists (see TypedList), created lazily.
  std::vector<RecencyList> typed_lists_;
  std::array<uint32_t, static_cast<size_t>(IdKind::kCount)> kind_lists_{};
  // values_list of `type`, created on first use (0 if the type is unknown).
  uint32_t ValuesListFor(uint32_t type);
  // 1-based typed_lists_ handle for `kind`, created on first use.
  uint32_t KindListFor(IdKind kind);

  const InstructionDesc* cur_desc_ = nullptr;
  uint32_t cur_opcode_ = 0;
};

// Structural opcode→section mapping (spec section order).
Section SectionOfOpcode(uint32_t opcode, const InstructionDesc* desc);

uint32_t SpanForField(const OperandCursor::Field& field,
                      const InstructionDesc* desc,
                      uint32_t current_result_type, const Tracker& tracker,
                      const uint32_t* words, uint32_t word_index,
                      uint32_t remaining);

}  // namespace arixv
