#include "operand_cursor.h"

#include <cassert>

using arixv::ExtInstSetDesc;
using arixv::FindEnumerant;
using arixv::FindExtInstSet;
using arixv::FindInstruction;
using arixv::GetOperandKind;
using arixv::OperandCategory;
using arixv::OperandCursor;

namespace {

OperandCursor::Field NextOperand(OperandCursor& cursor) {
  OperandCursor::Field field = cursor.Next();
  assert(field.type == OperandCursor::FieldType::kOperand);
  return field;
}

void ExpectEnd(OperandCursor& cursor) {
  assert(cursor.Next().type == OperandCursor::FieldType::kEnd);
}

void TestOpNop() {
  OperandCursor cursor;
  cursor.Begin(FindInstruction(0), 1, false);
  ExpectEnd(cursor);
  assert(!cursor.degraded());
}

void TestDecorateEnumParam() {
  const uint32_t kDecorationBuiltIn = 11;
  OperandCursor cursor;
  cursor.Begin(FindInstruction(71), 4, false);
  OperandCursor::Field field = NextOperand(cursor);
  assert(field.category == OperandCategory::kIdRef);
  cursor.Consume(1, 100);
  field = NextOperand(cursor);
  assert(field.category == OperandCategory::kValueEnum);
  const auto* enumerant = FindEnumerant(GetOperandKind(field.kind),
                                        kDecorationBuiltIn);
  assert(enumerant != nullptr);
  assert(enumerant->num_params == 1);
  cursor.Consume(1, kDecorationBuiltIn);
  field = NextOperand(cursor);
  assert(field.is_enum_param);
  assert(field.kind == arixv::GetEnumParamKinds(*enumerant)[0]);
  cursor.Consume(1, 0);
  ExpectEnd(cursor);
  assert(!cursor.degraded());
}

void TestLoadMemoryAccess() {
  OperandCursor cursor;
  cursor.Begin(FindInstruction(61), 4, false);
  assert(NextOperand(cursor).category == OperandCategory::kIdResultType);
  cursor.Consume(1, 1);
  assert(NextOperand(cursor).category == OperandCategory::kIdResult);
  cursor.Consume(1, 2);
  assert(NextOperand(cursor).category == OperandCategory::kIdRef);
  cursor.Consume(1, 3);
  ExpectEnd(cursor);
  assert(!cursor.degraded());

  cursor.Begin(FindInstruction(61), 6, false);
  cursor.Consume(1, 1);
  cursor.Consume(1, 2);
  cursor.Consume(1, 3);
  OperandCursor::Field field = NextOperand(cursor);
  assert(field.category == OperandCategory::kBitEnum);
  cursor.Consume(1, 2);
  field = NextOperand(cursor);
  assert(field.is_enum_param);
  assert(field.category == OperandCategory::kLiteralInteger);
  cursor.Consume(1, 16);
  ExpectEnd(cursor);
  assert(!cursor.degraded());
}

void TestSwitchPairs() {
  OperandCursor cursor;
  cursor.Begin(FindInstruction(251), 3, false);
  cursor.Consume(1, 1);
  cursor.Consume(1, 2);
  ExpectEnd(cursor);
  assert(!cursor.degraded());

  cursor.Begin(FindInstruction(251), 9, false);
  cursor.Consume(1, 1);
  cursor.Consume(1, 2);
  for (int i = 0; i < 3; ++i) {
    OperandCursor::Field field = NextOperand(cursor);
    assert(field.in_composite);
    assert(field.category == OperandCategory::kLiteralInteger);
    cursor.Consume(1, static_cast<uint32_t>(i));
    field = NextOperand(cursor);
    assert(field.in_composite);
    assert(field.category == OperandCategory::kIdRef);
    cursor.Consume(1, static_cast<uint32_t>(10 + i));
  }
  ExpectEnd(cursor);
  assert(!cursor.degraded());
}

void TestExtInst() {
  const ExtInstSetDesc* glsl = FindExtInstSet("GLSL.std.450");
  assert(glsl != nullptr);

  OperandCursor cursor;
  cursor.Begin(FindInstruction(12), 6, false);
  cursor.Consume(1, 1);
  cursor.Consume(1, 2);
  OperandCursor::Field field = NextOperand(cursor);
  assert(field.category == OperandCategory::kIdRef);
  assert(field.slot == 2);
  cursor.Consume(1, 50);
  cursor.SetExtInstSet(glsl);
  field = NextOperand(cursor);
  assert(field.category == OperandCategory::kLiteralExtInstInteger);
  cursor.Consume(1, 13);
  field = NextOperand(cursor);
  assert(field.category == OperandCategory::kIdRef);
  cursor.Consume(1, 3);
  ExpectEnd(cursor);
  assert(!cursor.degraded());

  cursor.Begin(FindInstruction(12), 6, false);
  cursor.Consume(1, 1);
  cursor.Consume(1, 2);
  cursor.Consume(1, 50);
  cursor.SetExtInstSet(nullptr);
  cursor.Consume(1, 13);
  assert(cursor.degraded());
  assert(cursor.Next().type == OperandCursor::FieldType::kOpaqueWord);
  cursor.Consume(1, 3);
  ExpectEnd(cursor);
}

void TestSpecConstantOp() {
  OperandCursor cursor;
  cursor.Begin(FindInstruction(52), 6, false);
  cursor.Consume(1, 1);
  cursor.Consume(1, 2);
  OperandCursor::Field field = NextOperand(cursor);
  assert(field.category == OperandCategory::kLiteralSpecConstantOpInteger);
  cursor.Consume(1, 128);
  field = NextOperand(cursor);
  assert(field.category == OperandCategory::kIdRef);
  cursor.Consume(1, 3);
  field = NextOperand(cursor);
  assert(field.category == OperandCategory::kIdRef);
  cursor.Consume(1, 4);
  ExpectEnd(cursor);
  assert(!cursor.degraded());
}

void TestDegradesAndOpaque() {
  OperandCursor cursor;
  cursor.Begin(FindInstruction(128), 4, false);
  cursor.Consume(1, 1);
  cursor.Consume(1, 2);
  cursor.Consume(1, 3);
  ExpectEnd(cursor);
  assert(cursor.degraded());

  cursor.Begin(nullptr, 3, false);
  assert(cursor.Next().type == OperandCursor::FieldType::kOpaqueWord);
  cursor.Consume(1, 1);
  assert(cursor.Next().type == OperandCursor::FieldType::kOpaqueWord);
  cursor.Consume(1, 2);
  ExpectEnd(cursor);

  cursor.Begin(FindInstruction(128), 1, false);
  ExpectEnd(cursor);
  assert(cursor.degraded());

  cursor.Begin(FindInstruction(128), 3, true);
  assert(cursor.Next().type == OperandCursor::FieldType::kOpaqueWord);
  cursor.Consume(1, 1);
  assert(cursor.Next().type == OperandCursor::FieldType::kOpaqueWord);
  cursor.Consume(1, 2);
  ExpectEnd(cursor);

  cursor.Begin(FindInstruction(61), 4, false);
  assert(cursor.Next().type == OperandCursor::FieldType::kOperand);
  cursor.Consume(10, 1);
  assert(cursor.degraded());
  ExpectEnd(cursor);
}

}  // namespace

int main() {
  TestOpNop();
  TestDecorateEnumParam();
  TestLoadMemoryAccess();
  TestSwitchPairs();
  TestExtInst();
  TestSpecConstantOp();
  TestDegradesAndOpaque();
  return 0;
}
