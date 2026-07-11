// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "tracker.h"

#include <algorithm>
#include <cstring>
#include <string>

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
  if (keys_.empty()) {
    return nullptr;
  }
  size_t i = static_cast<size_t>(id * kIdHashMul) & mask_;
  while (keys_[i] != 0) {
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
  if (keys_.empty()) {
    Rehash(kIdMapMinSlots);
  }
  size_t i = static_cast<size_t>(id * kIdHashMul) & mask_;
  while (keys_[i] != 0) {
    if (keys_[i] == id) {
      return values_[i];
    }
    i = (i + 1) & mask_;
  }
  if ((size_ + 1) * 10 >= keys_.size() * 7) {
    Rehash(keys_.size() * 2);
    i = static_cast<size_t>(id * kIdHashMul) & mask_;
    while (keys_[i] != 0) {
      i = (i + 1) & mask_;
    }
  }
  keys_[i] = id;
  values_[i] = IdInfo{};
  ++size_;
  return values_[i];
}

void Tracker::IdMap::Clear() {
  if (size_ != 0) {
    std::fill(keys_.begin(), keys_.end(), 0u);
    size_ = 0;
  }
  has_zero_ = false;
}

void Tracker::IdMap::Reserve(size_t expected_ids) {
  size_t slots = kIdMapMinSlots;
  while (slots < expected_ids && slots < kIdMapMaxReserveSlots) {
    slots *= 2;
  }
  if (slots > keys_.size()) {
    Rehash(slots);
  }
}

void Tracker::IdMap::Rehash(size_t slot_count) {
  std::vector<uint32_t> old_keys = std::move(keys_);
  std::vector<IdInfo> old_values = std::move(values_);
  keys_.assign(slot_count, 0u);
  values_.assign(slot_count, IdInfo{});
  mask_ = slot_count - 1;
  for (size_t from = 0; from < old_keys.size(); ++from) {
    const uint32_t id = old_keys[from];
    if (id == 0) {
      continue;
    }
    size_t i = static_cast<size_t>(id * kIdHashMul) & mask_;
    while (keys_[i] != 0) {
      i = (i + 1) & mask_;
    }
    keys_[i] = id;
    values_[i] = old_values[from];
  }
}

int RecencyList::Find(uint32_t id) const {
  for (int i = 0; i < size_; ++i) {
    if (items_[i] == id) {
      return i;
    }
  }
  return -1;
}

void RecencyList::Touch(uint32_t id) {
  if (cap_ <= 0) {
    return;
  }
  const int rank = Find(id);
  if (rank == 0) {
    return;
  }
  if (rank > 0) {
    // Move-to-front by shifting only the [0, rank] prefix, instead of an
    // erase + front insert that each shift the whole vector. Result is
    // identical: items_[rank] leads, the rest keep their relative order.
    std::memmove(&items_[1], &items_[0],
                 static_cast<size_t>(rank) * sizeof(uint32_t));
    items_[0] = id;
    return;
  }
  const int move_count = size_ < cap_ ? size_ : cap_ - 1;
  if (move_count > 0) {
    std::memmove(&items_[1], &items_[0],
                 static_cast<size_t>(move_count) * sizeof(uint32_t));
  }
  items_[0] = id;
  if (size_ < cap_) {
    ++size_;
  }
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

void Tracker::OnInstructionStart(uint32_t opcode, const InstructionDesc* desc,
                                 uint32_t /*word_count*/) {
  cur_opcode_ = opcode;
  cur_desc_ = desc;
}

void Tracker::OnIdUse(uint32_t id) {
  IdInfo& info = Ensure(id);
  if (!info.defined) {
    forward_refs_.Touch(id);
  }
  recent_ids_.Touch(id);
  if (info.kind == IdKind::kType) {
    recent_types_.Touch(id);
  }
}

void Tracker::OnResultId(uint32_t id) {
  Ensure(id);
  recent_ids_.Touch(id);
}

void Tracker::OnInstructionEnd(const uint32_t* words, uint32_t count) {
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
      forward_refs_.Remove(result_id);
      if (HasInstructionFlag(desc, kInstDeclaresType)) {
        info.kind = IdKind::kType;
        info.type_class = ToTrackerTypeClass(desc->type_class);
        info.width =
            HasInstructionFlag(desc, kInstTypeWidth) ? words[desc->aux_word] : 0;
        recent_types_.Touch(result_id);
      } else if (desc->result_bucket == 1) {
        info.kind = IdKind::kConstant;
      } else if (HasInstructionFlag(desc, kInstVariable)) {
        info.kind = IdKind::kVariable;
      } else if (HasInstructionFlag(desc, kInstFunction)) {
        info.kind = IdKind::kFunction;
      } else if (HasInstructionFlag(desc, kInstLabel)) {
        info.kind = IdKind::kLabel;
      } else if (HasInstructionFlag(desc, kInstExtInstImport)) {
        info.kind = IdKind::kExtInstSet;
        info.ext_set = FindExtInstSet(
            ReadString(words, desc->aux_word, count - desc->aux_word));
      } else {
        info.kind = IdKind::kValue;
      }
      recent_ids_.Touch(result_id);
    }
  }

  if (HasInstructionFlag(desc, kInstFunction)) {
    in_function_ = true;
    in_block_ = false;
  } else if (HasInstructionFlag(desc, kInstFunctionEnd)) {
    in_function_ = false;
    in_block_ = false;
  } else if (HasInstructionFlag(desc, kInstLabel)) {
    in_block_ = true;
  } else if (HasInstructionFlag(desc, kInstTerminator)) {
    in_block_ = false;
  }

  prev_opcode_ = static_cast<uint16_t>(opcode);
  prev_opcode_class_ = desc ? desc->instruction_class : 0xFFu;
  ++instruction_index_;
}

void Tracker::Reset() {
  section_ = Section::kCapabilities;
  prev_opcode_ = 0xFFFF;
  prev_opcode_class_ = 0xFF;
  in_function_ = false;
  in_block_ = false;
  max_seen_id_ = 0;
  instruction_index_ = 0;
  ids_.Clear();
  recent_ids_.Clear();
  recent_types_.Clear();
  forward_refs_.Clear();
  cur_desc_ = nullptr;
  cur_opcode_ = 0;
}

void Tracker::Reserve(size_t word_count) {
  // word_count is derived from the untrusted header size; clamp so a hostile
  // container cannot force a giant allocation before any payload is decoded.
  // A module cannot define or reference more distinct IDs than it has words,
  // so word_count is already a comfortable overestimate of the ID count.
  ids_.Reserve(std::min<size_t>(word_count, kMaxReserveWords));
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
