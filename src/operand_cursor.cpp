// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#include "operand_cursor.h"

namespace arixv {
inline namespace v1 {
namespace {

uint8_t CapSlot(int slot) {
  return static_cast<uint8_t>(slot < 15 ? slot : 15);
}

bool IsResultKind(uint16_t kind) {
  const OperandCategory category = GetOperandKind(kind).category;
  return category == OperandCategory::kIdResult ||
         category == OperandCategory::kIdResultType;
}

uint16_t PackPending(uint16_t kind, uint8_t slot, bool in_composite) {
  return static_cast<uint16_t>((kind & 0xffu) | ((slot & 0x0fu) << 8) |
                               (in_composite ? 0x1000u : 0u));
}

uint16_t PendingKind(uint16_t packed) {
  return static_cast<uint16_t>(packed & 0xffu);
}

uint8_t PendingSlot(uint16_t packed) {
  return static_cast<uint8_t>((packed >> 8) & 0x0fu);
}

bool PendingInComposite(uint16_t packed) {
  return (packed & 0x1000u) != 0;
}

}  // namespace

void OperandCursor::AppendPending(const uint16_t* kinds, uint8_t count,
                                  uint8_t slot, bool in_composite) {
  if (count == 0 || degraded_) {
    return;
  }
  // Compact the drained prefix first so the physical append index always has
  // room when the logical size does; the capacity check below must therefore
  // never pass while the write would overflow.
  if (pending_pos_ > 0) {
    for (int i = pending_pos_; i < pending_count_; ++i) {
      pending_kinds_[i - pending_pos_] = pending_kinds_[i];
    }
    pending_count_ -= pending_pos_;
    pending_pos_ = 0;
  }
  if (pending_count_ + count > kMaxPending) {
    degraded_ = true;
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    if (kinds[i] > 0xFF) {  // PackPending stores kinds in 8 bits
      degraded_ = true;
      return;
    }
    pending_kinds_[pending_count_++] = PackPending(kinds[i], slot,
                                                   in_composite);
  }
}

void OperandCursor::Begin(const InstructionDesc* inst, uint32_t word_count,
                          bool force_opaque) {
  inst_ = inst;
  ext_set_ = nullptr;
  remaining_ = word_count > 0 ? word_count - 1 : 0;
  degraded_ = inst == nullptr || force_opaque;
  slots_ = (!degraded_ && inst) ? GetOperandSlots(*inst) : nullptr;
  num_slots_ = (!degraded_ && inst) ? inst->num_operands : 0;
  slot_pos_ = 0;
  composite_pos_ = -1;
  composite_kind_ = 0;
  awaiting_ext_number_ = false;
  awaiting_spec_op_ = false;
  top_slot_for_context_ = 0;
  pending_count_ = 0;
  pending_pos_ = 0;
}

OperandCursor::Field OperandCursor::NextSlow() {
  Field field = {FieldType::kEnd, 0, OperandCategory::kUnknown,
                 top_slot_for_context_, false, false, remaining_};

  if (remaining_ == 0) {
    if (!degraded_ && (pending_pos_ < pending_count_ ||
                       composite_pos_ >= 0 || slot_pos_ < num_slots_)) {
      while (slot_pos_ < num_slots_ &&
             slots_[slot_pos_].quantifier != Quantifier::kOne) {
        ++slot_pos_;
      }
      if (pending_pos_ < pending_count_ || composite_pos_ >= 0 ||
          slot_pos_ < num_slots_) {
        degraded_ = true;
      }
    }
    return field;
  }

  if (degraded_) {
    field.type = FieldType::kOpaqueWord;
    field.slot = top_slot_for_context_;
    return field;
  }

  if (pending_pos_ < pending_count_) {
    const uint16_t pending = pending_kinds_[pending_pos_];
    const uint16_t pending_kind = PendingKind(pending);
    const OperandKindDesc& kind = GetOperandKind(pending_kind);
    if (kind.category == OperandCategory::kUnknown ||
        kind.category == OperandCategory::kComposite) {
      degraded_ = true;
      field.type = FieldType::kOpaqueWord;
      field.slot = PendingSlot(pending);
    } else {
      field.type = FieldType::kOperand;
      field.kind = pending_kind;
      field.category = kind.category;
      field.slot = PendingSlot(pending);
      field.is_enum_param = true;
      field.in_composite = PendingInComposite(pending);
    }
    return field;
  }
  pending_pos_ = 0;
  pending_count_ = 0;

  for (;;) {
    if (composite_pos_ >= 0) {
      const OperandKindDesc& composite = GetOperandKind(composite_kind_);
      const uint16_t* components = GetCompositeComponents(composite);
      if (composite_pos_ >= composite.num_components) {
        composite_pos_ = -1;
        continue;
      }
      const uint16_t component_kind = components[composite_pos_];
      const OperandKindDesc& component = GetOperandKind(component_kind);
      if (component.category == OperandCategory::kUnknown ||
          component.category == OperandCategory::kComposite) {
        degraded_ = true;
        field.type = FieldType::kOpaqueWord;
        field.slot = top_slot_for_context_;
      } else {
        field.type = FieldType::kOperand;
        field.kind = component_kind;
        field.category = component.category;
        field.slot = top_slot_for_context_;
        field.in_composite = true;
      }
      return field;
    }

    if (slot_pos_ >= num_slots_) {
      return field;
    }

    const OperandSlotDesc& slot = slots_[slot_pos_];
    top_slot_for_context_ = CapSlot(slot_pos_);
    if (slot.quantifier != Quantifier::kOne && remaining_ == 0) {
      ++slot_pos_;
      continue;
    }
    if (slot.quantifier == Quantifier::kOne && remaining_ == 0) {
      degraded_ = true;
      field.type = FieldType::kOpaqueWord;
      field.slot = top_slot_for_context_;
      return field;
    }

    const OperandKindDesc& kind = GetOperandKind(slot.kind);
    if (kind.category == OperandCategory::kUnknown) {
      degraded_ = true;
      field.type = FieldType::kOpaqueWord;
      field.slot = top_slot_for_context_;
      return field;
    }
    if (kind.category == OperandCategory::kComposite) {
      if (kind.num_components == 0) {
        degraded_ = true;
        field.type = FieldType::kOpaqueWord;
        field.slot = top_slot_for_context_;
        return field;
      }
      composite_kind_ = slot.kind;
      composite_pos_ = 0;
      continue;
    }

    field.type = FieldType::kOperand;
    field.kind = slot.kind;
    field.category = kind.category;
    field.slot = top_slot_for_context_;
    return field;
  }
}

void OperandCursor::Consume(uint32_t words, uint32_t value0) {
  Consume(Next(), words, value0);
}

void OperandCursor::ConsumeSlow(const Field& field, uint32_t words,
                                uint32_t value0) {
  if (field.type == FieldType::kEnd) {
    return;
  }
  if (words == 0) {
    degraded_ = true;
    return;
  }
  if (words > remaining_) {
    remaining_ = 0;
    degraded_ = true;
    return;
  }

  remaining_ -= words;

  bool layout_swapped = false;
  if (field.type == FieldType::kOperand) {
    // field.category was resolved from field.kind by Next(); no re-lookup.
    if (field.category == OperandCategory::kValueEnum) {
      const OperandKindDesc& kind = GetOperandKind(field.kind);
      const EnumerantDesc* enumerant = FindEnumerant(kind, value0);
      if (enumerant) {
        const uint16_t* params = GetEnumParamKinds(*enumerant);
        AppendPending(params, enumerant->num_params, field.slot,
                      field.in_composite);
      }
    } else if (field.category == OperandCategory::kBitEnum) {
      const OperandKindDesc& kind = GetOperandKind(field.kind);
      for (uint32_t bit = 0; bit < 32 && !degraded_; ++bit) {
        const uint32_t mask = 1u << bit;
        if ((value0 & mask) == 0) {
          continue;
        }
        const EnumerantDesc* enumerant = FindEnumerant(kind, mask);
        if (!enumerant) {
          continue;
        }
        const uint16_t* params = GetEnumParamKinds(*enumerant);
        AppendPending(params, enumerant->num_params, field.slot,
                      field.in_composite);
      }
    } else if (field.category == OperandCategory::kLiteralExtInstInteger &&
               awaiting_ext_number_) {
      if (!ext_set_) {
        degraded_ = true;
      } else {
        const InstructionDesc* ext_inst = FindExtInstruction(*ext_set_, value0);
        if (!ext_inst) {
          degraded_ = true;
        } else {
          slots_ = ext_set_->operand_slots
                       ? ext_set_->operand_slots + ext_inst->first_operand
                       : nullptr;
          num_slots_ = ext_inst->num_operands;
          slot_pos_ = 0;
          composite_pos_ = -1;
          layout_swapped = true;
        }
      }
      awaiting_ext_number_ = false;
    } else if (field.category ==
                   OperandCategory::kLiteralSpecConstantOpInteger &&
               (awaiting_spec_op_ ||
                (inst_ && inst_->opcode == 52))) {
      const InstructionDesc* inner = FindInstruction(value0);
      if (!inner) {
        degraded_ = true;
      } else {
        slots_ = GetOperandSlots(*inner);
        num_slots_ = inner->num_operands;
        slot_pos_ = 0;
        while (slot_pos_ < num_slots_ && IsResultKind(slots_[slot_pos_].kind)) {
          ++slot_pos_;
        }
        composite_pos_ = -1;
        layout_swapped = true;
      }
      awaiting_spec_op_ = false;
    }
  }

  if (field.is_enum_param) {
    ++pending_pos_;
    if (pending_pos_ >= pending_count_) {
      pending_pos_ = 0;
      pending_count_ = 0;
    }
  } else if (field.in_composite) {
    ++composite_pos_;
    const OperandKindDesc& composite = GetOperandKind(composite_kind_);
    if (composite_pos_ >= composite.num_components) {
      composite_pos_ = -1;
      const OperandSlotDesc& slot = slots_[slot_pos_];
      if (slot.quantifier != Quantifier::kVariadic || remaining_ == 0) {
        ++slot_pos_;
      }
    }
  } else if (field.type == FieldType::kOperand && !layout_swapped) {
    const OperandSlotDesc& slot = slots_[slot_pos_];
    if (inst_ && inst_->opcode == 12 &&
        field.category == OperandCategory::kIdRef && field.slot == 2) {
      awaiting_ext_number_ = true;
    }
    if (slot.quantifier != Quantifier::kVariadic || remaining_ == 0) {
      ++slot_pos_;
    }
  }
}

void OperandCursor::SetExtInstSet(const ExtInstSetDesc* set) {
  ext_set_ = set;
}

}  // namespace v1
}  // namespace arixv
