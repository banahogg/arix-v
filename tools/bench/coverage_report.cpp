#include "grammar_names.h"
#include "grammar_tables.h"
#include "operand_cursor.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using arixv::ExtInstSetDesc;
using arixv::FindEnumerant;
using arixv::FindExtInstruction;
using arixv::FindExtInstSet;
using arixv::FindInstruction;
using arixv::FindInstructionName;
using arixv::GetExtInstSetName;
using arixv::GetOperandKind;
using arixv::OperandCategory;
using arixv::OperandCursor;

constexpr uint32_t kSpirvMagic = 0x07230203u;

struct Report {
  uint64_t files_scanned = 0;
  uint64_t files_not_spirv = 0;
  uint64_t instructions_total = 0;
  uint64_t fixed_layout_instructions = 0;
  uint64_t fixed_layout_operand_words = 0;
  uint64_t unknown_opcodes = 0;
  uint64_t grammar_inconsistent = 0;
  uint64_t degraded_walks = 0;
  uint64_t leftover_mismatches = 0;
  uint64_t forward_refs = 0;
  uint64_t unknown_ext_sets = 0;
  uint64_t unknown_ext_set_uses = 0;
  uint64_t unknown_ext_numbers = 0;
  uint64_t unknown_enum_values = 0;
  uint64_t unknown_bitenum_bits = 0;
  std::map<uint32_t, uint64_t> unknown_opcode_hist;
  std::map<std::string, uint64_t> degraded_hist;
  std::map<std::string, uint64_t> leftover_hist;
  std::map<std::string, uint64_t> forward_ref_hist;
  std::map<std::string, uint64_t> ext_set_hist;
  std::map<OperandCategory, uint64_t> category_words;
};

struct ModuleState {
  std::unordered_set<uint32_t> defined_ids;
  std::unordered_map<uint32_t, uint32_t> type_widths;
  std::unordered_map<uint32_t, const ExtInstSetDesc*> ext_imports;
};

std::string CategoryName(OperandCategory category) {
  switch (category) {
    case OperandCategory::kIdResult:
      return "IdResult";
    case OperandCategory::kIdResultType:
      return "IdResultType";
    case OperandCategory::kIdRef:
      return "IdRef";
    case OperandCategory::kIdScope:
      return "IdScope";
    case OperandCategory::kIdMemorySemantics:
      return "IdMemorySemantics";
    case OperandCategory::kValueEnum:
      return "ValueEnum";
    case OperandCategory::kBitEnum:
      return "BitEnum";
    case OperandCategory::kLiteralInteger:
      return "LiteralInteger";
    case OperandCategory::kLiteralString:
      return "LiteralString";
    case OperandCategory::kLiteralContextDependentNumber:
      return "LiteralContextDependentNumber";
    case OperandCategory::kLiteralExtInstInteger:
      return "LiteralExtInstInteger";
    case OperandCategory::kLiteralSpecConstantOpInteger:
      return "LiteralSpecConstantOpInteger";
    case OperandCategory::kComposite:
      return "Composite";
    case OperandCategory::kUnknown:
      return "Unknown";
  }
  return "Unknown";
}

std::vector<std::pair<std::string, uint64_t>> TopString(
    const std::map<std::string, uint64_t>& hist) {
  std::vector<std::pair<std::string, uint64_t>> rows(hist.begin(), hist.end());
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) {
              if (a.second != b.second) {
                return a.second > b.second;
              }
              return a.first < b.first;
            });
  if (rows.size() > 10) {
    rows.resize(10);
  }
  return rows;
}

std::vector<std::pair<uint32_t, uint64_t>> TopOpcode(
    const std::map<uint32_t, uint64_t>& hist) {
  std::vector<std::pair<uint32_t, uint64_t>> rows(hist.begin(), hist.end());
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) {
              if (a.second != b.second) {
                return a.second > b.second;
              }
              return a.first < b.first;
            });
  if (rows.size() > 10) {
    rows.resize(10);
  }
  return rows;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return false;
  }
  bytes->resize(static_cast<size_t>(size));
  file.seekg(0, std::ios::beg);
  if (!bytes->empty()) {
    file.read(reinterpret_cast<char*>(bytes->data()),
              static_cast<std::streamsize>(bytes->size()));
  }
  return file.good() || file.eof();
}

uint32_t LoadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint32_t> BytesToWords(const std::vector<uint8_t>& bytes) {
  std::vector<uint32_t> words(bytes.size() / 4);
  for (size_t i = 0; i < words.size(); ++i) {
    words[i] = LoadLe32(bytes.data() + i * 4);
  }
  return words;
}

uint32_t StringSpanWords(const std::vector<uint32_t>& words, size_t word_index,
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

std::string ReadString(const std::vector<uint32_t>& words, size_t word_index,
                       uint32_t span) {
  std::string result;
  for (uint32_t i = 0; i < span; ++i) {
    const uint32_t word = words[word_index + i];
    for (uint32_t byte = 0; byte < 4; ++byte) {
      const char ch = static_cast<char>((word >> (byte * 8)) & 0xffu);
      if (ch == '\0') {
        return result;
      }
      result.push_back(ch);
    }
  }
  return result;
}

uint32_t SpanForField(const OperandCursor::Field& field, uint32_t opcode,
                      uint32_t current_result_type,
                      const ModuleState& module,
                      const std::vector<uint32_t>& words, size_t word_index,
                      uint32_t remaining) {
  switch (field.category) {
    case OperandCategory::kLiteralString:
      return StringSpanWords(words, word_index, remaining);
    case OperandCategory::kLiteralContextDependentNumber:
      if (opcode == 43 || opcode == 50) {
        const auto width_it = module.type_widths.find(current_result_type);
        if (width_it == module.type_widths.end()) {
          return remaining;
        }
        return width_it->second > 32 ? 2u : 1u;
      }
      return 1;
    case OperandCategory::kIdResult:
    case OperandCategory::kIdResultType:
    case OperandCategory::kIdRef:
    case OperandCategory::kIdScope:
    case OperandCategory::kIdMemorySemantics:
    case OperandCategory::kValueEnum:
    case OperandCategory::kBitEnum:
    case OperandCategory::kLiteralInteger:
    case OperandCategory::kLiteralExtInstInteger:
    case OperandCategory::kLiteralSpecConstantOpInteger:
    case OperandCategory::kComposite:
    case OperandCategory::kUnknown:
      return 1;
  }
  return 1;
}

bool IsIdReference(OperandCategory category) {
  return category == OperandCategory::kIdResultType ||
         category == OperandCategory::kIdRef ||
         category == OperandCategory::kIdScope ||
         category == OperandCategory::kIdMemorySemantics;
}

std::string OpName(const arixv::InstructionDesc* inst, uint32_t opcode) {
  if (inst) {
    if (const char* name = FindInstructionName(opcode)) {
      return name;
    }
  }
  std::ostringstream out;
  out << "opcode " << opcode;
  return out.str();
}

void NoteEnumAnomalies(const OperandCursor::Field& field, uint32_t value,
                       Report* report) {
  const arixv::OperandKindDesc& kind = GetOperandKind(field.kind);
  if (field.category == OperandCategory::kValueEnum) {
    if (FindEnumerant(kind, value) == nullptr) {
      ++report->unknown_enum_values;
    }
  } else if (field.category == OperandCategory::kBitEnum) {
    for (uint32_t bit = 0; bit < 32; ++bit) {
      const uint32_t mask = 1u << bit;
      if ((value & mask) != 0 && FindEnumerant(kind, mask) == nullptr) {
        ++report->unknown_bitenum_bits;
      }
    }
  }
}

void ScanModule(const std::vector<uint32_t>& words, Report* report) {
  ModuleState module;
  size_t pc = 5;
  while (pc < words.size()) {
    const uint32_t word0 = words[pc];
    const uint32_t word_count = word0 >> 16;
    const uint32_t opcode = word0 & 0xffffu;
    ++report->instructions_total;

    const arixv::InstructionDesc* inst = FindInstruction(opcode);
    if (!inst) {
      ++report->unknown_opcodes;
      ++report->unknown_opcode_hist[opcode];
    }

    const bool valid_frame =
        word_count > 0 && pc + word_count <= words.size();
    bool grammar_consistent = false;
    if (inst && valid_frame) {
      grammar_consistent = inst->fixed_word_count
                               ? word_count == inst->min_word_count
                               : word_count >= inst->min_word_count;
      if (!grammar_consistent) {
        ++report->grammar_inconsistent;
      }
    } else if (inst) {
      ++report->grammar_inconsistent;
    }

    if (!valid_frame) {
      break;
    }

    if (inst && grammar_consistent) {
      if (inst->fixed_word_count) {
        ++report->fixed_layout_instructions;
        report->fixed_layout_operand_words += word_count - 1;
      }
      OperandCursor cursor;
      cursor.Begin(inst, word_count, false);
      const std::string op_name = OpName(inst, opcode);
      uint32_t consumed = 0;
      uint32_t result_id = 0;
      uint32_t current_result_type = 0;
      uint32_t type_width = 0;
      std::string ext_import_name;
      const ExtInstSetDesc* current_ext_set = nullptr;
      bool mismatch = false;

      while (true) {
        OperandCursor::Field field = cursor.Next();
        if (field.type == OperandCursor::FieldType::kEnd) {
          if (consumed != word_count - 1) {
            mismatch = true;
          }
          break;
        }
        if (consumed >= word_count - 1) {
          mismatch = true;
          break;
        }

        const size_t operand_index = pc + 1 + consumed;
        const uint32_t remaining = word_count - 1 - consumed;
        const uint32_t value0 = words[operand_index];
        uint32_t span = 1;
        if (field.type == OperandCursor::FieldType::kOperand) {
          span = SpanForField(field, opcode, current_result_type, module,
                              words, operand_index, remaining);
          NoteEnumAnomalies(field, value0, report);
          report->category_words[field.category] += std::min(span, remaining);

          if (IsIdReference(field.category) &&
              module.defined_ids.find(value0) == module.defined_ids.end()) {
            ++report->forward_refs;
            std::ostringstream key;
            key << op_name << " slot " << static_cast<uint32_t>(field.slot);
            ++report->forward_ref_hist[key.str()];
          }

          if (field.category == OperandCategory::kIdResultType) {
            current_result_type = value0;
          } else if (field.category == OperandCategory::kIdResult) {
            result_id = value0;
          } else if ((opcode == 21 || opcode == 22) &&
                     field.category == OperandCategory::kLiteralInteger &&
                     type_width == 0) {
            type_width = value0;
          } else if (opcode == 11 &&
                     field.category == OperandCategory::kLiteralString) {
            ext_import_name =
                ReadString(words, operand_index, std::min(span, remaining));
          } else if (opcode == 12 && field.category == OperandCategory::kIdRef &&
                     field.slot == 2) {
            const auto set_it = module.ext_imports.find(value0);
            current_ext_set =
                set_it == module.ext_imports.end() ? nullptr : set_it->second;
          } else if (opcode == 12 &&
                     field.category ==
                         OperandCategory::kLiteralExtInstInteger) {
            if (current_ext_set) {
              ++report->ext_set_hist[GetExtInstSetName(*current_ext_set)];
              if (FindExtInstruction(*current_ext_set, value0) == nullptr) {
                ++report->unknown_ext_numbers;
              }
            } else {
              ++report->unknown_ext_set_uses;
            }
          }
        } else {
          report->category_words[OperandCategory::kUnknown] += 1;
        }

        cursor.Consume(field, span, value0);
        if (opcode == 12 && field.type == OperandCursor::FieldType::kOperand &&
            field.category == OperandCategory::kIdRef && field.slot == 2) {
          cursor.SetExtInstSet(current_ext_set);
        }

        consumed += std::min(span, remaining);
      }

      if (cursor.degraded()) {
        ++report->degraded_walks;
        ++report->degraded_hist[op_name];
      }
      if (mismatch) {
        ++report->leftover_mismatches;
        ++report->leftover_hist[op_name];
      }

      if (opcode == 11 && result_id != 0) {
        const ExtInstSetDesc* set = FindExtInstSet(ext_import_name);
        if (set) {
          module.ext_imports[result_id] = set;
        } else {
          module.ext_imports[result_id] = nullptr;
          ++report->unknown_ext_sets;
        }
      }
      if ((opcode == 21 || opcode == 22) && result_id != 0 &&
          type_width != 0) {
        module.type_widths[result_id] = type_width;
      }
      if (result_id != 0) {
        module.defined_ids.insert(result_id);
      }
    }

    pc += word_count;
  }
}

void ScanFile(const std::filesystem::path& path, Report* report) {
  ++report->files_scanned;
  std::vector<uint8_t> bytes;
  if (!ReadFile(path, &bytes) || bytes.size() < 20 || bytes.size() % 4 != 0) {
    ++report->files_not_spirv;
    return;
  }
  const std::vector<uint32_t> words = BytesToWords(bytes);
  if (words.empty() || words[0] != kSpirvMagic) {
    ++report->files_not_spirv;
    return;
  }
  ScanModule(words, report);
}

void PrintTopOpcode(const std::map<uint32_t, uint64_t>& hist) {
  for (const auto& row : TopOpcode(hist)) {
    std::cout << "  " << row.first << ": " << row.second << "\n";
  }
}

void PrintTopString(const std::map<std::string, uint64_t>& hist) {
  for (const auto& row : TopString(hist)) {
    std::cout << "  " << row.first << ": " << row.second << "\n";
  }
}

std::vector<std::filesystem::path> CollectInputs(int argc, char** argv) {
  std::vector<std::filesystem::path> paths;
  if (argc == 3 && std::string(argv[1]) == "--corpus") {
    const std::filesystem::path root(argv[2]);
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file() && entry.path().extension() == ".spv") {
        paths.push_back(entry.path());
      }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
  }
  for (int i = 1; i < argc; ++i) {
    paths.emplace_back(argv[i]);
  }
  return paths;
}

void PrintReport(const Report& report) {
  std::cout << "files scanned: " << report.files_scanned << "\n";
  std::cout << "files not SPIR-V: " << report.files_not_spirv << "\n\n";

  std::cout << "instructions total: " << report.instructions_total << "\n";
  std::cout << "fixed-layout instructions: "
            << report.fixed_layout_instructions << "\n";
  std::cout << "fixed-layout operand words: "
            << report.fixed_layout_operand_words << "\n";
  std::cout << "unknown opcodes: " << report.unknown_opcodes << "\n";
  std::cout << "unknown opcode top-10:\n";
  PrintTopOpcode(report.unknown_opcode_hist);
  std::cout << "grammar-inconsistent instructions: "
            << report.grammar_inconsistent << "\n\n";

  std::cout << "degraded cursor walks: " << report.degraded_walks << "\n";
  std::cout << "degraded cursor walks top-10:\n";
  PrintTopString(report.degraded_hist);
  std::cout << "\n";

  std::cout << "leftover mismatches: " << report.leftover_mismatches << "\n";
  std::cout << "leftover mismatches top-10:\n";
  PrintTopString(report.leftover_hist);
  std::cout << "\n";

  std::cout << "operand-category coverage (words):\n";
  for (int i = static_cast<int>(OperandCategory::kIdResult);
       i <= static_cast<int>(OperandCategory::kUnknown); ++i) {
    const OperandCategory category = static_cast<OperandCategory>(i);
    const auto it = report.category_words.find(category);
    const uint64_t count =
        it == report.category_words.end() ? 0 : it->second;
    std::cout << "  " << CategoryName(category) << ": " << count << "\n";
  }
  std::cout << "\n";

  std::cout << "forward references: " << report.forward_refs << "\n";
  std::cout << "forward references top-10:\n";
  PrintTopString(report.forward_ref_hist);
  std::cout << "\n";

  std::cout << "ext-inst per-set instruction counts:\n";
  PrintTopString(report.ext_set_hist);
  std::cout << "unknown ext-inst sets: " << report.unknown_ext_sets << "\n";
  std::cout << "unknown ext-inst set uses: " << report.unknown_ext_set_uses
            << "\n";
  std::cout << "unknown ext-inst numbers: " << report.unknown_ext_numbers
            << "\n\n";

  std::cout << "unknown enum values: " << report.unknown_enum_values << "\n";
  std::cout << "unknown BitEnum bits: " << report.unknown_bitenum_bits << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Report report;
  const std::vector<std::filesystem::path> paths = CollectInputs(argc, argv);
  for (const auto& path : paths) {
    ScanFile(path, &report);
  }
  PrintReport(report);
  return 0;
}
