#include "grammar_tables.h"

#include <iostream>
#include <string_view>

namespace {

int Fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

const arixv::OperandKindDesc* FindKindByName(std::string_view name) {
  for (uint16_t i = 0; i < arixv::NumOperandKinds(); ++i) {
    const arixv::OperandKindDesc& kind = arixv::GetOperandKind(i);
    if (kind.name == name) {
      return &kind;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  const arixv::InstructionDesc* function_call =
      arixv::FindInstruction(57);
  if (function_call == nullptr) {
    return Fail("OpFunctionCall not found");
  }
  if (!function_call->has_result || !function_call->has_result_type) {
    return Fail("OpFunctionCall result flags are wrong");
  }
  const arixv::OperandSlotDesc* function_call_operands =
      arixv::GetOperandSlots(*function_call);
  if (function_call->num_operands == 0 ||
      function_call_operands[function_call->num_operands - 1].quantifier !=
          arixv::Quantifier::kVariadic) {
    return Fail("OpFunctionCall last operand is not variadic");
  }

  if (arixv::FindInstruction(71) == nullptr) {
    return Fail("OpDecorate not found");
  }

  const arixv::ExtInstSetDesc* glsl =
      arixv::FindExtInstSet("GLSL.std.450");
  if (glsl == nullptr) {
    return Fail("GLSL.std.450 set not found");
  }
  const arixv::InstructionDesc* sin = arixv::FindExtInstruction(*glsl, 14);
  if (sin == nullptr) {
    return Fail("GLSL Sin not found");
  }
  const arixv::OperandSlotDesc* sin_operands = glsl->operand_slots + sin->first_operand;
  if (sin->num_operands != 1 ||
      arixv::GetOperandKind(sin_operands[0].kind).category !=
          arixv::OperandCategory::kIdRef) {
    return Fail("GLSL Sin does not have exactly one IdRef operand");
  }

  if (arixv::FindExtInstSet("NonSemantic.ClspvReflection.5") == nullptr) {
    return Fail("NonSemantic.ClspvReflection.5 prefix lookup failed");
  }

  const arixv::OperandKindDesc* memory_access = FindKindByName("MemoryAccess");
  if (memory_access == nullptr) {
    return Fail("MemoryAccess kind not found");
  }
  const arixv::EnumerantDesc* aligned =
      arixv::FindEnumerant(*memory_access, 2);
  if (aligned == nullptr) {
    return Fail("MemoryAccess Aligned enumerant not found");
  }
  if (aligned->num_params != 1) {
    return Fail("MemoryAccess Aligned parameter count is wrong");
  }
  const uint16_t* aligned_params = arixv::GetEnumParamKinds(*aligned);
  if (arixv::GetOperandKind(aligned_params[0]).category !=
      arixv::OperandCategory::kLiteralInteger) {
    return Fail("MemoryAccess Aligned parameter kind is not LiteralInteger");
  }

  const arixv::InstructionDesc* type_float = arixv::FindInstruction(22);
  if (type_float == nullptr) {
    return Fail("OpTypeFloat not found");
  }
  if (!type_float->has_result) {
    return Fail("OpTypeFloat result flag is wrong");
  }
  if (type_float->min_word_count < 3) {
    return Fail("OpTypeFloat minimum word count is too small");
  }
  if (type_float->result_bucket != 0 ||
      type_float->type_class != arixv::InstructionTypeClass::kFloat ||
      !arixv::HasInstructionFlag(type_float, arixv::kInstDeclaresType) ||
      !arixv::HasInstructionFlag(type_float, arixv::kInstTypeWidth)) {
    return Fail("OpTypeFloat generated classification is wrong");
  }

  const arixv::InstructionDesc* branch = arixv::FindInstruction(249);
  if (branch == nullptr ||
      !arixv::HasInstructionFlag(branch, arixv::kInstTerminator)) {
    return Fail("OpBranch terminator classification is wrong");
  }

  const arixv::InstructionDesc* function = arixv::FindInstruction(54);
  if (function == nullptr || function->result_bucket != 2 ||
      !arixv::HasInstructionFlag(function, arixv::kInstFunction)) {
    return Fail("OpFunction generated classification is wrong");
  }

  return 0;
}
