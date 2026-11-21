#include "grammar_tables.h"

#include <cstring>
#include <iostream>
#include <string_view>

#include "symbol_coder.h"
#include "tracker.h"

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

// Generated cold-start priors must equal SymbolCdfSetPrior applied to the
// weights they were built from (the generator mirrors it in Python).
template <int kN>
bool PriorMatches(uint16_t pool_index, const uint32_t* w) {
  if (pool_index >= arixv::NumPriorCdfs()) {
    return false;
  }
  arixv::SymbolCdf expected;
  arixv::SymbolCdfSetPrior<kN>(expected, w);
  return std::memcmp(expected.d, arixv::GetPriorCdf(pool_index),
                     sizeof(expected.d)) == 0;
}

bool OpcodeLegal(uint8_t section, uint32_t index) {
  return (arixv::GetOpcodeSectionLegal(section)[index >> 5] >> (index & 31)) & 1u;
}

int CheckPriors() {
  const uint32_t n = arixv::NumCoreInstructions();
  for (uint8_t s = 0; s < static_cast<uint8_t>(arixv::Section::kCount); ++s) {
    uint32_t weight[1024];
    for (uint32_t i = 0; i < 1024; ++i) {
      weight[i] = i >= n ? 0
                  : OpcodeLegal(s, i) ? arixv::kOpcodePriorLegalWeight
                                      : 1;
    }
    const uint16_t* prior = arixv::GetOpcodeSectionPrior(s);
    uint32_t w[16];
    for (uint32_t top = 0; top < 4; ++top) {
      w[top] = 0;
      for (uint32_t i = 0; i < 256; ++i) w[top] += weight[(top << 8) | i];
    }
    if (!PriorMatches<4>(prior[0], w)) return Fail("opcode root prior mismatch");
    for (uint32_t top = 0; top < 4; ++top) {
      for (uint32_t m = 0; m < 16; ++m) {
        w[m] = 0;
        for (uint32_t l = 0; l < 16; ++l) w[m] += weight[(top << 8) | (m << 4) | l];
      }
      if (!PriorMatches<16>(prior[1 + top], w)) {
        return Fail("opcode mid prior mismatch");
      }
    }
    for (uint32_t leaf = 0; leaf < 64; ++leaf) {
      if (!PriorMatches<16>(prior[5 + leaf], &weight[leaf << 4])) {
        return Fail("opcode leaf prior mismatch");
      }
    }
  }
  // Spot-check the layout rules the masks encode.
  const auto legal = [](uint32_t opcode, arixv::Section s) {
    const int index = arixv::FindInstructionIndex(opcode);
    return index >= 0 &&
           OpcodeLegal(static_cast<uint8_t>(s), static_cast<uint32_t>(index));
  };
  using arixv::Section;
  if (!legal(17, Section::kCapabilities) || legal(17, Section::kDebug) ||
      !legal(71, Section::kAnnotations) || legal(71, Section::kFunctionBodies) ||
      !legal(21, Section::kTypesConstantsVars) ||
      legal(21, Section::kFunctionBodies) ||
      !legal(59, Section::kFunctionBodies) ||
      !legal(54, Section::kFunctionBodies) ||
      !legal(128, Section::kFunctionBodies) ||
      legal(128, Section::kTypesConstantsVars) ||
      !legal(8, Section::kTypesConstantsVars) ||
      !legal(12, Section::kEntryPoints)) {
    return Fail("opcode section legality is wrong");
  }
  for (uint32_t count = 0; count <= 256; ++count) {
    uint32_t w[16];
    for (uint32_t h = 0; h < 16; ++h) {
      const uint32_t first = h * 16;
      w[h] = count > first ? (count - first < 16 ? count - first : 16) : 0;
    }
    if (!PriorMatches<16>(arixv::GetIndexRootPrior(count), w)) {
      return Fail("index root prior mismatch");
    }
  }
  if (arixv::GetIndexLeafPrior(0) != 0) return Fail("index leaf prior 0 not uniform");
  for (uint32_t partial = 1; partial < 16; ++partial) {
    uint32_t w[16];
    for (uint32_t l = 0; l < 16; ++l) w[l] = l < partial ? 1 : 0;
    if (!PriorMatches<16>(arixv::GetIndexLeafPrior(partial), w)) {
      return Fail("index leaf prior mismatch");
    }
  }
  if (arixv::NumRegisteredGenerators() < 8 || arixv::NumRegisteredGenerators() > 256) {
    return Fail("generator registry size is implausible");
  }
  {
    uint32_t weights[256];
    for (uint32_t b = 0; b < 256; ++b) {
      weights[b] = arixv::kStringOrder0FloorWeight;
    }
    for (char ch : arixv::GetStringPrimer()) {
      ++weights[static_cast<uint8_t>(ch)];
    }
    uint32_t root_w[16];
    for (uint32_t h = 0; h < 16; ++h) {
      root_w[h] = 0;
      for (uint32_t l = 0; l < 16; ++l) {
        root_w[h] += weights[(h << 4) | l];
      }
    }
    if (!PriorMatches<16>(arixv::GetStringOrder0RootPrior(), root_w)) {
      return Fail("string order-0 root prior mismatch");
    }
    for (uint32_t h = 0; h < 16; ++h) {
      if (!PriorMatches<16>(arixv::GetStringOrder0LeafPrior(h),
                            &weights[h << 4])) {
        return Fail("string order-0 leaf prior mismatch");
      }
    }
    for (uint32_t h = 8; h < 16; ++h) {
      if (arixv::GetStringOrder0LeafPrior(h) != 0) {
        return Fail("absent high-nibble string order-0 leaf not uniform");
      }
    }
  }
  return 0;
}

}  // namespace

int main() {
  if (const int failed = CheckPriors()) {
    return failed;
  }
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
