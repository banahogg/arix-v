// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "tracker.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>

#if ARIXV_RECENCY_SSE2
#include <emmintrin.h>
#endif

#include "operand_cursor.h"

namespace arixv {
namespace {

static_assert(static_cast<uint8_t>(InstructionTypeClass::kOther) ==
                  static_cast<uint8_t>(Tracker::TypeClass::kOther),
              "generated type class values must match Tracker::TypeClass");
static_assert(static_cast<uint8_t>(InstructionTypeClass::kFunction) ==
                  static_cast<uint8_t>(Tracker::TypeClass::kFunction),
              "generated type class values must match Tracker::TypeClass");

Tracker::TypeClass ToTrackerTypeClass(InstructionTypeClass type_class) {
  return static_cast<Tracker::TypeClass>(static_cast<uint8_t>(type_class));
}

uint32_t StringSpanWords(const uint32_t* words, uint32_t word_index,
                         uint32_t remaining) {
  for (uint32_t i = 0; i < remaining; ++i) {
    const uint32_t word = words[word_index + i];
    for (uint32_t byte = 0; byte < 4; ++byte) {
      if (((word >> (byte * 8)) & 0xffu) == 0) {
        return i + 1;
      }
    }
  }
  return remaining + 1;
}

std::string ReadString(const uint32_t* words, uint32_t word_index,
                       uint32_t span) {
  std::string out;
  for (uint32_t i = 0; i < span; ++i) {
    const uint32_t word = words[word_index + i];
    for (uint32_t byte = 0; byte < 4; ++byte) {
      const char ch = static_cast<char>((word >> (byte * 8)) & 0xffu);
      if (ch == '\0') {
        return out;
      }
      out.push_back(ch);
    }
  }
  return out;
}

// Fibonacci-style multiplier; odd, so sequential IDs (the common case) walk
// the table without clustering.
constexpr uint32_t kIdHashMul = 2654435761u;
constexpr size_t kIdMapMinSlots = 256;
// Reserve() allocation cap: 1M slots keep a hostile header's up-front cost in
// the same ballpark as the bucket array the old unordered_map reserved.
constexpr size_t kIdMapMaxReserveSlots = size_t{1} << 20;

}  // namespace

const Tracker::IdInfo* Tracker::IdMap::Find(uint32_t id) const {
  if (id == 0) {
    return has_zero_ ? &zero_info_ : nullptr;
  }
  if (capacity_ == 0) {
    return nullptr;
  }
  size_t i = static_cast<size_t>(id * kIdHashMul) & mask_;
  while (slot_gen_[i] == generation_) {
    if (keys_[i] == id) {
      return &values_[i];
    }
    i = (i + 1) & mask_;
  }
  return nullptr;
}

Tracker::IdInfo& Tracker::IdMap::Ensure(uint32_t id) {
  if (id == 0) {
    if (!has_zero_) {
      has_zero_ = true;
      zero_info_ = IdInfo{};
    }
    return zero_info_;
  }
  if (capacity_ == 0) {
    Rehash(kIdMapMinSlots);
  }
  size_t i = static_cast<size_t>(id * kIdHashMul) & mask_;
  while (slot_gen_[i] == generation_) {
    if (keys_[i] == id) {
      return values_[i];
    }
    i = (i + 1) & mask_;
  }
  if ((size_ + 1) * 10 >= capacity_ * 7) {
    Rehash(capacity_ * 2);
    i = static_cast<size_t>(id * kIdHashMul) & mask_;
    while (slot_gen_[i] == generation_) {
      i = (i + 1) & mask_;
    }
  }
  keys_[i] = id;
  slot_gen_[i] = generation_;
  values_[i] = IdInfo{};
  ++size_;
  return values_[i];
}

void Tracker::IdMap::Clear() {
  // Bump the generation so every slot's stamp no longer matches: the whole map
  // reads as empty without touching the key array. Only when the 32-bit
  // generation wraps back to 0 (after ~4 billion modules through one map) do we
  // pay a one-time bulk reset to re-establish the "0 => empty" invariant.
  ++generation_;
  if (generation_ == 0) {
    std::fill(slot_gen_.get(), slot_gen_.get() + capacity_, 0u);
    generation_ = 1;
  }
  size_ = 0;
  has_zero_ = false;
}

void Tracker::IdMap::Reserve(size_t expected_ids) {
  size_t slots = kIdMapMinSlots;
  while (slots < expected_ids && slots < kIdMapMaxReserveSlots) {
    slots *= 2;
  }
  if (slots > capacity_) {
    Rehash(slots);
  }
}

void Tracker::IdMap::Rehash(size_t slot_count) {
  const std::unique_ptr<uint32_t[]> old_keys = std::move(keys_);
  const std::unique_ptr<IdInfo[]> old_values = std::move(values_);
  const std::unique_ptr<uint32_t[]> old_gen = std::move(slot_gen_);
  const size_t old_capacity = capacity_;
  // Keys and values stay uninitialized (written when a slot is claimed);
  // only the stamps are zeroed. Fresh stamps are 0 and occupied slots below
  // are stamped with generation_, which is never 0 during use (Clear() skips
  // 0), so nothing else in the new array reads as occupied.
  keys_.reset(new uint32_t[slot_count]);
  values_.reset(new IdInfo[slot_count]);
  slot_gen_.reset(new uint32_t[slot_count]());
  capacity_ = slot_count;
  mask_ = slot_count - 1;
  for (size_t from = 0; from < old_capacity; ++from) {
    if (old_gen[from] != generation_) {
      continue;  // slot was empty in the current generation
    }
    const uint32_t id = old_keys[from];
    size_t i = static_cast<size_t>(id * kIdHashMul) & mask_;
    while (slot_gen_[i] == generation_) {
      i = (i + 1) & mask_;
    }
    keys_[i] = id;
    slot_gen_[i] = generation_;
    values_[i] = old_values[from];
  }
}

uint32_t RecencyList::MatchMask(uint32_t id) const {
  // Fixed-width compare over the whole storage (no early-exit branch);
  // entries past size_ are stale and masked off.
  uint32_t mask = 0;
#if ARIXV_RECENCY_SSE2
  const __m128i key = _mm_set1_epi32(static_cast<int>(id));
  const __m128i* v = reinterpret_cast<const __m128i*>(items_.data());
  const __m128i e0 = _mm_cmpeq_epi32(_mm_load_si128(v + 0), key);
  const __m128i e1 = _mm_cmpeq_epi32(_mm_load_si128(v + 1), key);
  const __m128i e2 = _mm_cmpeq_epi32(_mm_load_si128(v + 2), key);
  const __m128i e3 = _mm_cmpeq_epi32(_mm_load_si128(v + 3), key);
  const __m128i lo = _mm_packs_epi32(e0, e1);
  const __m128i hi = _mm_packs_epi32(e2, e3);
  mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_packs_epi16(lo, hi)));
#else
  for (int i = 0; i < kRecencyListStorageCap; ++i) {
    mask |= static_cast<uint32_t>(items_[static_cast<size_t>(i)] == id) << i;
  }
#endif
  return mask & ((1u << size_) - 1u);
}

int RecencyList::Find(uint32_t id) const {
  const uint32_t mask = MatchMask(id);
  return mask != 0 ? __builtin_ctz(mask) : -1;
}

void RecencyList::Touch(uint32_t id) {
  if (cap_ <= 0) {
    return;
  }
  const uint32_t mask = MatchMask(id);
  // `last` is the slot that the move-to-front overwrites: the touched entry
  // when present, else the first free slot (or the oldest entry when full).
  int last;
  if (mask != 0) {
    last = __builtin_ctz(mask);
    if (last == 0) {
      return;
    }
  } else {
    last = size_ < cap_ ? size_ : cap_ - 1;
    if (size_ < cap_) {
      ++size_;
    }
  }
  ShiftIn(last, id);
}

void RecencyList::Replace(uint32_t old_id, uint32_t id) {
  if (cap_ <= 0) {
    return;
  }
  const uint32_t mask = MatchMask(old_id);
  if (mask == 0) {
    // Insert as a new entry (callers never list id itself already).
    const int last = size_ < cap_ ? size_ : cap_ - 1;
    if (size_ < cap_) {
      ++size_;
    }
    ShiftIn(last, id);
    return;
  }
  const int at = __builtin_ctz(mask);
  if (at == 0) {
    items_[0] = id;
    return;
  }
  ShiftIn(at, id);
}

void RecencyList::ShiftIn(int last, uint32_t id) {
  // Shift [0, last) up by one with a fixed-width select instead of a
  // variable-length memmove call; slots past `last` keep their value.
#if ARIXV_RECENCY_SSE2
  __m128i* v = reinterpret_cast<__m128i*>(items_.data());
  const __m128i v0 = _mm_load_si128(v + 0);
  const __m128i v1 = _mm_load_si128(v + 1);
  const __m128i v2 = _mm_load_si128(v + 2);
  const __m128i v3 = _mm_load_si128(v + 3);
  // Whole list moved up one lane, id entering at lane 0.
  const __m128i s0 = _mm_or_si128(_mm_slli_si128(v0, 4),
                                  _mm_cvtsi32_si128(static_cast<int>(id)));
  const __m128i s1 = _mm_or_si128(_mm_slli_si128(v1, 4), _mm_srli_si128(v0, 12));
  const __m128i s2 = _mm_or_si128(_mm_slli_si128(v2, 4), _mm_srli_si128(v1, 12));
  const __m128i s3 = _mm_or_si128(_mm_slli_si128(v3, 4), _mm_srli_si128(v2, 12));
  // Lane i takes the shifted value iff i <= last.
  const __m128i limit = _mm_set1_epi32(last + 1);
  const auto blend = [&limit](__m128i shifted, __m128i orig, int base) {
    const __m128i lane = _mm_setr_epi32(base, base + 1, base + 2, base + 3);
    const __m128i take = _mm_cmpgt_epi32(limit, lane);
    return _mm_or_si128(_mm_and_si128(take, shifted),
                        _mm_andnot_si128(take, orig));
  };
  _mm_store_si128(v + 0, blend(s0, v0, 0));
  _mm_store_si128(v + 1, blend(s1, v1, 4));
  _mm_store_si128(v + 2, blend(s2, v2, 8));
  _mm_store_si128(v + 3, blend(s3, v3, 12));
#else
  const std::array<uint32_t, kRecencyListStorageCap> old = items_;
  for (int i = 1; i < kRecencyListStorageCap; ++i) {
    const size_t u = static_cast<size_t>(i);
    items_[u] = i <= last ? old[u - 1] : old[u];
  }
  items_[0] = id;
#endif
}

void RecencyList::Remove(uint32_t id) {
  const int rank = Find(id);
  if (rank >= 0) {
    const int move_count = size_ - rank - 1;
    if (move_count > 0) {
      std::memmove(&items_[rank], &items_[rank + 1],
                   static_cast<size_t>(move_count) * sizeof(uint32_t));
    }
    --size_;
  }
}

Tracker::Tracker() { Reset(); }

const Tracker::IdInfo* Tracker::Find(uint32_t id) const {
  return ids_.Find(id);
}

uint32_t Tracker::TypeWidth(uint32_t type_id) const {
  const IdInfo* info = Find(type_id);
  if (!info || info->kind != IdKind::kType) {
    return 0;
  }
  return (info->type_class == TypeClass::kInt ||
          info->type_class == TypeClass::kFloat)
             ? info->width
             : 0;
}

void Tracker::RecordDerivedType(std::vector<DerivedTypeEntry>& table,
                                uint32_t key_a, uint32_t key_b,
                                uint32_t type_id) {
  if (table.size() >= kMaxDerivedTypeEntries) {
    return;
  }
  for (DerivedTypeEntry& entry : table) {
    if (entry.key_a == key_a && entry.key_b == key_b) {
      entry.type_id = type_id;
      return;
    }
  }
  table.push_back(DerivedTypeEntry{key_a, key_b, type_id});
}

uint32_t Tracker::LookupDerivedType(const std::vector<DerivedTypeEntry>& table,
                                    uint32_t key_a, uint32_t key_b) {
  for (const DerivedTypeEntry& entry : table) {
    if (entry.key_a == key_a && entry.key_b == key_b) {
      return entry.type_id;
    }
  }
  return 0;
}

uint32_t Tracker::PointerTypeOf(uint32_t storage_class,
                                uint32_t pointee_type) const {
  return pointee_type != 0
             ? LookupDerivedType(ptr_types_, storage_class, pointee_type)
             : 0;
}

uint32_t Tracker::VectorTypeOf(uint32_t elem_type, uint32_t count) const {
  return elem_type != 0 ? LookupDerivedType(vec_types_, elem_type, count) : 0;
}

uint32_t Tracker::MatrixTypeOf(uint32_t col_type, uint32_t count) const {
  return col_type != 0 ? LookupDerivedType(mat_types_, col_type, count) : 0;
}

uint32_t Tracker::ArrayTypeOf(uint32_t elem_type, uint32_t length) const {
  return elem_type != 0 ? LookupDerivedType(arr_types_, elem_type, length) : 0;
}

uint32_t Tracker::StructTypeOf(const uint32_t* member_types,
                               uint32_t count) const {
  if (count == 0) {
    return 0;
  }
  for (size_t i = struct_types_.size(); i > 0; --i) {
    const uint32_t sid = struct_types_[i - 1];
    const IdInfo* info = Find(sid);
    if (!info || info->kind != IdKind::kType ||
        info->type_class != TypeClass::kStruct ||
        info->members.count != count) {
      continue;
    }
    const uint32_t* stored = member_types_.data() + info->members.offset;
    bool match = true;
    for (uint32_t k = 0; k < count; ++k) {
      if (stored[k] != member_types[k]) {
        match = false;
        break;
      }
    }
    if (match) {
      return sid;
    }
  }
  return 0;
}

uint32_t Tracker::WalkCompositeByIds(uint32_t type_id,
                                     const uint32_t* index_ids,
                                     uint32_t count) const {
  constexpr uint32_t kMaxWalkDepth = 32;
  if (type_id == 0 || count == 0 || count > kMaxWalkDepth) {
    return 0;
  }
  uint32_t cur = type_id;
  for (uint32_t i = 0; i < count; ++i) {
    const IdInfo* info = Find(cur);
    if (!info || info->kind != IdKind::kType) {
      return 0;
    }
    switch (info->type_class) {
      case TypeClass::kStruct: {
        uint32_t member_index = 0;
        if (!ConstantIntValue(index_ids[i], &member_index)) {
          return 0;
        }
        cur = StructMemberType(*info, member_index);
        break;
      }
      case TypeClass::kVector:
      case TypeClass::kMatrix:
      case TypeClass::kArray:
      case TypeClass::kRuntimeArray:
        cur = info->element_type;
        break;
      default:
        return 0;
    }
    if (cur == 0) {
      return 0;
    }
  }
  return cur;
}

uint32_t Tracker::WalkCompositeByLiterals(uint32_t type_id,
                                          const uint32_t* indices,
                                          uint32_t count) const {
  constexpr uint32_t kMaxWalkDepth = 32;
  if (type_id == 0 || count == 0 || count > kMaxWalkDepth) {
    return 0;
  }
  uint32_t cur = type_id;
  for (uint32_t i = 0; i < count; ++i) {
    const IdInfo* info = Find(cur);
    if (!info || info->kind != IdKind::kType) {
      return 0;
    }
    switch (info->type_class) {
      case TypeClass::kStruct:
        cur = StructMemberType(*info, indices[i]);
        break;
      case TypeClass::kVector:
      case TypeClass::kMatrix:
      case TypeClass::kArray:
      case TypeClass::kRuntimeArray:
        cur = info->element_type;
        break;
      default:
        return 0;
    }
    if (cur == 0) {
      return 0;
    }
  }
  return cur;
}

void Tracker::OnInstructionStart(uint32_t opcode, const InstructionDesc* desc,
                                 uint32_t /*word_count*/) {
  cur_opcode_ = opcode;
  cur_desc_ = desc;
}

Tracker::IdUse Tracker::OnIdUse(uint32_t id, bool touch_recent_ids) {
  IdInfo& info = Ensure(id);
  if (!info.defined) {
    forward_refs_.Touch(id);
  }
  if (touch_recent_ids) {
    recent_ids_.Touch(id);
  }
  if (info.kind == IdKind::kType) {
    recent_types_.Touch(id);
  } else if (info.type_list != 0) {
    assert(info.type_list != kRecentTypesList);
    typed_lists_[info.type_list - 1].Touch(id);
  }
  return IdUse{info.result_type, info.type_list};
}

// Only looks the type up (never inserts it), so callers' IdInfo references
// stay valid; a type the tracker has never seen (hostile input) gets no list.
uint32_t Tracker::ValuesListFor(uint32_t type) {
  IdInfo* type_info = ids_.FindMutable(type);
  if (type_info == nullptr) {
    return 0;
  }
  if (type_info->values_list == 0) {
    typed_lists_.emplace_back(kRankListCapIds);
    type_info->values_list = static_cast<uint32_t>(typed_lists_.size());
  }
  return type_info->values_list;
}

uint32_t Tracker::KindListFor(IdKind kind) {
  uint32_t& list = kind_lists_[static_cast<size_t>(kind)];
  if (list == 0) {
    typed_lists_.emplace_back(kRankListCapIds);
    list = static_cast<uint32_t>(typed_lists_.size());
  }
  return list;
}

// Result types are coded from recent_types(), so touching recent_ids() here
// only pushed the values that ID operands reference further down the list
// (removing it: −0.52% corpus size alone, −1.04% with OnResultId below).
void Tracker::OnResultTypeUse(uint32_t id) {
  IdInfo& info = Ensure(id);
  if (!info.defined) {
    forward_refs_.Touch(id);
  }
  if (info.kind == IdKind::kType) {
    recent_types_.Touch(id);
  }
}

// No recency touch: OnInstructionEnd moves the result to the front once the
// instruction is complete, and an instruction's own later operands do not
// reference its result, so touching it here only shifted their ranks.
// No map insert either: OnInstructionEnd inserts the result, and until then a
// default entry and a missing one read the same to every consumer (Find ->
// TypeWidth 0 / no ext set; OnIdUse and Ensure create the default). Only
// max_seen_id_ must see the ID now.
void Tracker::OnResultId(uint32_t id) {
  if (id > max_seen_id_) {
    max_seen_id_ = id;
  }
  // Continuing a run moves that run to the front as `id`; anything else
  // starts a new run.
  result_runs_.Replace(id - 1, id);
}

void Tracker::OnInstructionEnd(const uint32_t* words, uint32_t count,
                               uint16_t inst_index,
                               uint32_t first_id_operand) {
  const InstructionDesc* desc = cur_desc_;
  const uint32_t opcode = cur_opcode_;

  if (desc) {
    section_ = std::max(section_, SectionOfOpcode(opcode, desc));

    const bool grammar_consistent = desc->fixed_word_count
                                        ? count == desc->min_word_count
                                        : count >= desc->min_word_count;

    // No operand re-walk: per-operand ID bookkeeping already happened via
    // OnIdUse/OnResultId while the instruction was coded, and everything the
    // semantic update below needs sits at a fixed word index — the result
    // type at word 1 with the result ID directly after it, and the
    // width/component/import-name operand at desc->aux_word (both invariants
    // guaranteed by the table generator, which also guarantees
    // aux_word < min_word_count <= count here).
    const uint32_t result_id = grammar_consistent && desc->has_result
                                   ? words[desc->has_result_type ? 2 : 1]
                                   : 0;
    if (result_id != 0) {
      IdInfo& info = Ensure(result_id);
      info.defined = true;
      info.result_type = desc->has_result_type ? words[1] : 0;
      info.type_class = TypeClass::kOther;
      info.aux_flag = 0;
      info.width = 0;
      info.members = {0, 0};
      info.element_type = 0;
      forward_refs_.Remove(result_id);
      uint32_t list = 0;
      if (HasInstructionFlag(desc, kInstDeclaresType)) {
        info.kind = IdKind::kType;
        info.element_type = first_id_operand;
        info.type_class = ToTrackerTypeClass(desc->type_class);
        if (HasInstructionFlag(desc, kInstTypeWidth)) {
          info.width = words[desc->aux_word];
          const int wb = ScalarWidthBucket(info.width);
          if (info.type_class == TypeClass::kInt && count >= 4) {
            const uint8_t is_signed = words[3] != 0 ? 1u : 0u;
            info.aux_flag = is_signed;
            if (wb >= 0) {
              scalar_types_[is_signed][wb] = result_id;
            }
          } else if (info.type_class == TypeClass::kFloat && wb >= 0) {
            scalar_types_[2][wb] = result_id;
          }
        } else if (info.type_class == TypeClass::kBool) {
          bool_type_id_ = result_id;
        } else if (info.type_class == TypeClass::kPointer && count >= 4) {
          info.width = words[2];
          RecordDerivedType(ptr_types_, words[2], words[3], result_id);
        } else if (info.type_class == TypeClass::kVector && count >= 4) {
          info.width = words[3];
          RecordDerivedType(vec_types_, words[2], words[3], result_id);
        } else if (info.type_class == TypeClass::kMatrix && count >= 4) {
          info.width = words[3];
          RecordDerivedType(mat_types_, words[2], words[3], result_id);
        } else if (info.type_class == TypeClass::kArray && count >= 4) {
          uint32_t len = 0;
          if (ConstantIntValue(words[3], &len)) {
            info.width = len;
            RecordDerivedType(arr_types_, words[2], len, result_id);
          }
        } else if (info.type_class == TypeClass::kSampledImage && count >= 3) {
          IdInfo* img = ids_.FindMutable(words[2]);
          if (img && img->kind == IdKind::kType &&
              img->type_class == TypeClass::kImage) {
            img->width = result_id;
          }
        } else if (info.type_class == TypeClass::kStruct) {
          const uint32_t num_members = count > 2 ? count - 2 : 0;
          const size_t avail = member_types_.size() < kMaxMemberTypes
                                   ? kMaxMemberTypes - member_types_.size()
                                   : 0;
          const uint32_t take =
              static_cast<uint32_t>(std::min<size_t>(num_members, avail));
          info.members.offset = static_cast<uint32_t>(member_types_.size());
          info.members.count = take;
          if (take > 0) {
            member_types_.insert(member_types_.end(), words + 2,
                                 words + 2 + take);
          }
          if (take == num_members && num_members > 0 &&
              struct_types_.size() < kMaxDerivedTypeEntries) {
            struct_types_.push_back(result_id);
          }
        } else if (info.type_class == TypeClass::kFunction) {
          const uint32_t num_params = count > 3 ? count - 3 : 0;
          const size_t avail = member_types_.size() < kMaxMemberTypes
                                   ? kMaxMemberTypes - member_types_.size()
                                   : 0;
          const uint32_t take =
              static_cast<uint32_t>(std::min<size_t>(num_params, avail));
          info.members.offset = static_cast<uint32_t>(member_types_.size());
          info.members.count = take;
          if (take > 0) {
            member_types_.insert(member_types_.end(), words + 3,
                                 words + 3 + take);
          }
        }
        recent_types_.Touch(result_id);
        list = kRecentTypesList;
      } else if (desc->result_bucket == 1) {
        info.kind = IdKind::kConstant;
        if (HasInstructionFlag(desc, kInstConstantNumber) &&
            (count == 4 || (count == 5 && words[4] == 0))) {
          const IdInfo* rtype = Find(words[1]);
          if (rtype && rtype->kind == IdKind::kType &&
              rtype->type_class == TypeClass::kInt) {
            info.aux_flag = 1;
            info.width = words[3];
          }
        } else if (opcode == 46 && count == 3) {
          const IdInfo* rtype = Find(words[1]);
          if (rtype && rtype->kind == IdKind::kType &&
              rtype->type_class == TypeClass::kInt) {
            info.aux_flag = 1;
            info.width = 0;
          }
        }
      } else if (HasInstructionFlag(desc, kInstVariable)) {
        info.kind = IdKind::kVariable;
        list = KindListFor(IdKind::kVariable);
      } else if (HasInstructionFlag(desc, kInstFunction)) {
        info.kind = IdKind::kFunction;
        list = KindListFor(IdKind::kFunction);
      } else if (HasInstructionFlag(desc, kInstLabel)) {
        info.kind = IdKind::kLabel;
      } else if (HasInstructionFlag(desc, kInstExtInstImport)) {
        info.kind = IdKind::kExtInstSet;
        info.ext_set = FindExtInstSet(
            ReadString(words, desc->aux_word, count - desc->aux_word));
        list = KindListFor(IdKind::kExtInstSet);
      } else {
        info.kind = IdKind::kValue;
      }
      recent_ids_.Touch(result_id);
      if (list == 0 && info.result_type != 0) {
        list = ValuesListFor(info.result_type);
      }
      info.type_list = list;
      if (list != 0 && list != kRecentTypesList) {
        typed_lists_[list - 1].Touch(result_id);
      }
    }
  }

  if (HasInstructionFlag(desc, kInstFunction)) {
    in_function_ = true;
    in_block_ = false;
    current_fn_type_ = (desc && count >= 5) ? words[4] : 0;
    current_fn_param_index_ = 0;
  } else if (HasInstructionFlag(desc, kInstFunctionEnd)) {
    in_function_ = false;
    in_block_ = false;
    current_fn_type_ = 0;
    current_fn_param_index_ = 0;
  } else if (HasInstructionFlag(desc, kInstLabel)) {
    in_block_ = true;
    current_fn_type_ = 0;
    current_fn_param_index_ = 0;
  } else if (HasInstructionFlag(desc, kInstTerminator)) {
    in_block_ = false;
  } else if (opcode == 55 && current_fn_type_ != 0) {
    ++current_fn_param_index_;
  }

  prev_opcode_ = static_cast<uint16_t>(opcode);
  prev_inst_index_ = inst_index;
  prev_opcode_class_ = desc ? desc->instruction_class : 0xFFu;
  ++instruction_index_;
}

void Tracker::Reset() {
  section_ = Section::kCapabilities;
  prev_opcode_ = 0xFFFF;
  prev_inst_index_ = 0xFFFF;
  prev_opcode_class_ = 0xFF;
  in_function_ = false;
  in_block_ = false;
  max_seen_id_ = 0;
  instruction_index_ = 0;
  ids_.Clear();
  recent_ids_.Clear();
  recent_types_.Clear();
  forward_refs_.Clear();
  result_runs_.Clear();
  typed_lists_.clear();
  kind_lists_.fill(0);
  bool_type_id_ = 0;
  std::memset(scalar_types_, 0, sizeof(scalar_types_));
  current_fn_type_ = 0;
  current_fn_param_index_ = 0;
  member_types_.clear();
  struct_types_.clear();
  ptr_types_.clear();
  vec_types_.clear();
  mat_types_.clear();
  arr_types_.clear();
  cur_desc_ = nullptr;
  cur_opcode_ = 0;
}

void Tracker::Reserve(size_t word_count) {
  // word_count is derived from the untrusted header size; clamp so a hostile
  // container cannot force a giant allocation before any payload is decoded.
  // Real modules define roughly one ID per 4-6 words, so word_count / 4
  // rarely needs a doubling rehash; the full word count overprovisioned the
  // 24-byte slots ~4x (corpus-measured: page faults per decode 1.55 -> 0.32
  // in 64-file batch processes; / 2 was neutral and / 8 rehashed too often).
  ids_.Reserve(std::min<size_t>(word_count, kMaxReserveWords) / 4);
}

Tracker::IdInfo& Tracker::Ensure(uint32_t id) {
  if (id > max_seen_id_) {
    max_seen_id_ = id;
  }
  return ids_.Ensure(id);
}

Section SectionOfOpcode(uint32_t /*opcode*/, const InstructionDesc* desc) {
  if (!desc) {
    return Section::kCapabilities;
  }
  return static_cast<Section>(desc->section);
}

uint32_t SpanForField(const OperandCursor::Field& field,
                      const InstructionDesc* desc,
                      uint32_t current_result_type, const Tracker& tracker,
                      const uint32_t* words, uint32_t word_index,
                      uint32_t remaining) {
  if (field.type != OperandCursor::FieldType::kOperand) {
    return 1;
  }
  switch (field.category) {
    case OperandCategory::kLiteralString:
      return StringSpanWords(words, word_index, remaining);
    case OperandCategory::kLiteralContextDependentNumber:
      if (HasInstructionFlag(desc, kInstConstantNumber)) {
        const uint32_t width = tracker.TypeWidth(current_result_type);
        if (width == 0) {
          return remaining;
        }
        return width > 32 ? 2u : 1u;
      }
      return 1;
    default:
      return 1;
  }
}

}  // namespace arixv
