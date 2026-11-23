// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

// RESEARCH ONLY (Track B gate 2): per-module structural dump for the
// repeated-structure opportunity probe (scripts/bench/multi_module_structure.py).
//
// For every input module it writes:
//   - the standalone structural payload size (EncodeModule, as ARX2 mode 0);
//   - every body instruction's words, a per-word ID-role mask (0 other,
//     1 IdResult definition, 2 ID reference), and the exact encoder entropy
//     (bits) the unchanged single-module model spent on that instruction in
//     context, from the gated ARIXV_RESEARCH_INST_COST hook.
//
// Frames and roles come from research::ModelIdRoles: the model's own
// instruction framing and operand categories, exactly what the AXST codec
// uses for copy bases. Opaque words (unknown or grammar-inconsistent
// instructions) have role 0, so copies of them must be word-identical.
//
// Output (little-endian):
//   "AXSD" u32 version=1 u32 module_count
//   per module: u32 path_len, path, u8 status (0 ok, 1 not model-eligible,
//     2 unreadable), u8 swapped, u32 file_size, u32 payload_bytes,
//     f64 header_bits, f64 tail_bits, u32 inst_count,
//     per instruction: u32 nwords, nwords*u32 words (native order, unswapped),
//     nwords*u8 mask, f64 bits.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "grammar_tables.h"
#include "model.h"
#include "research_pack.h"

namespace {


constexpr uint32_t kMagic = 0x07230203u;
constexpr uint32_t kMagicSwapped = 0x03022307u;

bool ReadFile(const std::string& path, std::vector<uint8_t>* bytes) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  bytes->assign(std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>());
  return true;
}

uint32_t Swap(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}

struct Inst {
  uint32_t begin;  // absolute word index
  uint32_t nwords;
};

// The model's own frames and ID roles (research::ModelIdRoles, the AXST
// codec's copy-base masks), converted to absolute word positions.
bool Walk(const std::vector<uint8_t>& bytes, std::vector<Inst>* insts,
          std::vector<uint8_t>* mask) {
  std::vector<arixv::research::IdRoleFrame> frames;
  std::vector<uint8_t> body;
  if (!arixv::research::ModelIdRoles(bytes.data(), bytes.size(), &frames,
                                     &body)) {
    return false;
  }
  mask->assign(5, 0);
  mask->insert(mask->end(), body.begin(), body.end());
  for (const auto& f : frames) insts->push_back({f.begin + 5, f.nwords});
  return true;
}

template <typename T>
void Put(std::ostream& o, const T& v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: arixv_structure_dump OUT.bin FILE.spv... | "
                 "OUT.bin @LIST\n");
    return 2;
  }
  setenv("ARIXV_STATS", "1", 1);
  std::vector<std::string> paths;
  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] == '@') {
      std::ifstream list(argv[i] + 1);
      std::string line;
      while (std::getline(list, line)) {
        if (!line.empty()) paths.push_back(line);
      }
    } else {
      paths.emplace_back(argv[i]);
    }
  }
  std::ofstream out(argv[1], std::ios::binary);
  out.write("AXSD", 4);
  Put<uint32_t>(out, 1);
  Put<uint32_t>(out, static_cast<uint32_t>(paths.size()));
  int failures = 0;
  for (const std::string& path : paths) {
    Put<uint32_t>(out, static_cast<uint32_t>(path.size()));
    out.write(path.data(), static_cast<std::streamsize>(path.size()));
    std::vector<uint8_t> bytes;
    uint8_t status = 0;
    if (!ReadFile(path, &bytes)) {
      status = 2;
      ++failures;
    } else if (!arixv::IsSpirvModelEligible(bytes.data(), bytes.size())) {
      status = 1;
    }
    std::vector<uint32_t> words;
    uint8_t swapped = 0;
    if (status == 0) {
      words.resize(bytes.size() / 4);
      std::memcpy(words.data(), bytes.data(), words.size() * 4);
      if (words[0] == kMagicSwapped) {
        swapped = 1;
        for (uint32_t& v : words) v = Swap(v);
      }
      if (words[0] != kMagic) status = 1;
    }
    Put<uint8_t>(out, status);
    Put<uint8_t>(out, swapped);
    Put<uint32_t>(out, static_cast<uint32_t>(bytes.size()));
    if (status != 0) {
      Put<uint32_t>(out, 0);
      Put<double>(out, 0.0);
      Put<double>(out, 0.0);
      Put<uint32_t>(out, 0);
      continue;
    }
    std::vector<uint8_t> payload;
    std::vector<double> cum;
    if (!arixv::research::EncodeModuleInstCosts(bytes.data(), bytes.size(),
                                                payload, cum)) {
      std::fprintf(stderr, "stats not enabled\n");
      return 3;
    }
    std::vector<Inst> insts;
    std::vector<uint8_t> mask;
    if (!Walk(bytes, &insts, &mask)) insts.clear();
    // cum: [start, inst0 start, ..., instN-1 start, end]. The model's
    // instruction framing equals the grammar frame walk for eligible input;
    // on a count mismatch the module is written with no instructions.
    if (cum.size() != insts.size() + 2) {
      // Written with inst_count 0: the analysis keeps it whole-literal.
      std::fprintf(stderr,
                   "note: %s: model/grammar frame mismatch (%zu vs %zu); "
                   "kept whole-literal\n",
                   path.c_str(), cum.size() - 2, insts.size());
    }
    Put<uint32_t>(out, static_cast<uint32_t>(payload.size()));
    const bool aligned = cum.size() == insts.size() + 2;
    Put<double>(out, aligned ? cum[1] - cum[0] : cum.back() - cum.front());
    Put<double>(out, 0.0);  // post-loop costs are in the last instruction
    Put<uint32_t>(out, aligned ? static_cast<uint32_t>(insts.size()) : 0u);
    if (!aligned) continue;
    for (size_t i = 0; i < insts.size(); ++i) {
      const Inst& in = insts[i];
      Put<uint32_t>(out, in.nwords);
      out.write(reinterpret_cast<const char*>(words.data() + in.begin),
                static_cast<std::streamsize>(in.nwords) * 4);
      out.write(reinterpret_cast<const char*>(mask.data() + in.begin),
                in.nwords);
      Put<double>(out, cum[i + 2] - cum[i + 1]);
    }
  }
  out.close();
  return failures == 0 ? 0 : 1;
}
