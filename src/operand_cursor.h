// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

// Grammar-driven walk over the operand words of a single SPIR-V instruction.
//
// Shared by the encoder, the decoder, and the M0 coverage tool, so its
// behavior must be a deterministic function of (instruction desc, declared
// word count, values reported via Consume). The consumer
// peeks Next(), codes/parses that field, then reports what it saw with
// Consume(); the cursor never reads instruction words itself.
#pragma once

#include <cstdint>

#include "grammar_tables.h"
#include "platform.h"

namespace arixv {
inline namespace v1 {

class OperandCursor {
 public:
  enum class FieldType : uint8_t {
    kOperand,     // a grammar-described operand starting at the current word
    kOpaqueWord,  // one word with no grammar interpretation (degraded mode)
    kEnd,         // instruction fully consumed
  };

  struct Field {
    FieldType type;
    uint16_t kind;             // operand kind index (kOperand only)
    OperandCategory category;  // category of `kind`
    uint8_t slot;              // top-level layout slot index, capped at 15;
                               // a stable context input for the model
    bool is_enum_param;        // parameter of a previously consumed enumerant
    bool in_composite;         // component of a Pair* kind
    uint32_t remaining_words;  // operand words still unconsumed (excl. word0)
  };

  // Start walking one instruction. `inst` may be null (unknown opcode):
  // every word is then kOpaqueWord. `word_count` is the declared total
  // including word0 (must be >= 1; the cursor walks word_count - 1 operand
  // words). Grammar interpretation is used only when the caller already
  // determined the declared count is grammar-consistent;
  // otherwise the caller passes force_opaque = true.
  void Begin(const InstructionDesc* inst, uint32_t word_count,
             bool force_opaque);

  // Peek the next field. Idempotent until Consume() is called.
  // Inline fast path for the common case (a plain top-level slot with words
  // left, no pending enumerant parameters, no composite); everything else
  // goes through NextSlow(), which leaves identical state.
  ARIXV_FORCE_INLINE Field Next() {
    if (pending_pos_ >= pending_count_ && composite_pos_ < 0 && !degraded_ &&
        remaining_ != 0 && slot_pos_ < num_slots_) {
      const OperandSlotDesc& slot = slots_[slot_pos_];
      const OperandCategory category = GetOperandKind(slot.kind).category;
      if (category != OperandCategory::kUnknown &&
          category != OperandCategory::kComposite) {
        pending_pos_ = 0;
        pending_count_ = 0;
        top_slot_for_context_ =
            static_cast<uint8_t>(slot_pos_ < 15 ? slot_pos_ : 15);
        return Field{FieldType::kOperand, slot.kind, category,
                     top_slot_for_context_, false, false, remaining_};
      }
    }
    return NextSlow();
  }

  // The current field consumed `words` words (>= 1 except where noted), and
  // `value0` is the value of its first word. value0 drives the walk for:
  // ValueEnum/BitEnum (schedules enumerant parameters — BitEnum parameters
  // in increasing bit-position order), LiteralExtInstInteger (selects the
  // ext instruction layout), and OpSpecConstantOp's
  // LiteralSpecConstantOpInteger (selects the inner instruction layout,
  // skipping its result and result-type slots). Multi-word fields (strings,
  // wide literals) report their total word span. Consuming more words than
  // remain degrades the walk instead of overrunning.
  void Consume(uint32_t words, uint32_t value0);
  // Inline fast path for a plain top-level operand whose value does not
  // steer the walk (not an enum, ext-inst number, or spec-op opcode); the
  // rest goes through ConsumeSlow(), which leaves identical state.
  ARIXV_FORCE_INLINE void Consume(const Field& field, uint32_t words,
                                  uint32_t value0) {
    if (field.type == FieldType::kOperand && !field.is_enum_param &&
        !field.in_composite && words != 0 && words <= remaining_ &&
        field.category != OperandCategory::kValueEnum &&
        field.category != OperandCategory::kBitEnum &&
        field.category != OperandCategory::kLiteralExtInstInteger &&
        field.category != OperandCategory::kLiteralSpecConstantOpInteger) {
      remaining_ -= words;
      if (inst_ && inst_->opcode == 12 &&
          field.category == OperandCategory::kIdRef && field.slot == 2) {
        awaiting_ext_number_ = true;
      }
      if (slots_[slot_pos_].quantifier != Quantifier::kVariadic ||
          remaining_ == 0) {
        ++slot_pos_;
      }
      return;
    }
    ConsumeSlow(field, words, value0);
  }

  // OpExtInst: when the set-ID operand is consumed the consumer resolves the
  // set (via its ID tracking) and calls this — possibly with nullptr for an
  // unknown set, which degrades the remaining walk to opaque words once the
  // instruction-number field has been consumed.
  void SetExtInstSet(const ExtInstSetDesc* set);

  // Updates the remaining operand-word budget mid-walk (used when a deferred
  // OpExtInst resolves to a non-fixed extended instruction and codes its
  // remaining operand count after the instruction number).
  void SetRemaining(uint32_t remaining) { remaining_ = remaining; }

  // True once grammar interpretation was abandoned mid-walk (missing
  // required operand, string overrun, unknown inner opcode, unknown ext
  // set/number, unknown composite/category, quantifier state that cannot
  // fit the remaining words). Remaining words are emitted as kOpaqueWord.
  bool degraded() const { return degraded_; }

 private:
  Field NextSlow();
  void ConsumeSlow(const Field& field, uint32_t words, uint32_t value0);

  // Schedules enumerant parameter kinds, compacting the drained queue prefix
  // first; degrades on genuine overflow instead of writing out of bounds.
  void AppendPending(const uint16_t* kinds, uint8_t count, uint8_t slot,
                     bool in_composite);

  // Implementation note: the walk state is a small explicit stack —
  // (slot list, position, quantifier progress, pending enum-parameter kinds,
  // composite expansion) — never recursion, so hostile inputs cannot
  // overflow the C++ stack. See operand_cursor.cpp.
  struct State;

  const InstructionDesc* inst_ = nullptr;
  const ExtInstSetDesc* ext_set_ = nullptr;
  uint32_t remaining_ = 0;
  bool degraded_ = false;
  // Fixed-capacity pending-parameter queue; overflow degrades (bounded by
  // grammar reality: no real enumerant chain is deep).
  static constexpr int kMaxPending = 32;
  uint16_t pending_kinds_[kMaxPending];
  int pending_count_ = 0;
  int pending_pos_ = 0;
  // Top-level slot progression.
  const OperandSlotDesc* slots_ = nullptr;
  int num_slots_ = 0;
  int slot_pos_ = 0;
  int composite_pos_ = -1;  // -1: not inside a composite
  uint16_t composite_kind_ = 0;
  bool awaiting_ext_number_ = false;
  bool awaiting_spec_op_ = false;
  uint8_t top_slot_for_context_ = 0;
};

}  // namespace v1
}  // namespace arixv
