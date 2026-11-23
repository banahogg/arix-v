// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// RESEARCH ONLY (Track B gate 1b): tests for the multi-module sequence hook
// (src/research_pack.h). Built only with -DARIXV_BUILD_RESEARCH_PACK=ON.
//
// Default build: a CTest executable (fixtures from tests/golden plus
// synthetic modules, deterministic bounded fuzz). The ARXP pack tests are in
// tests/pack_test.cpp. With
// ARIXV_RESEARCH_LIBFUZZER_RT / _DEC defined, the same file is the
// coverage-guided round-trip / hostile-decode libFuzzer harness.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "arixv/arixv.h"
#include "model.h"
#include "research_pack.h"

namespace rp = arixv::research;

namespace {

#define RCHECK(expr)                                                       \
  do {                                                                     \
    if (!(expr)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #expr);                                                 \
      std::fflush(stderr);                                                 \
      std::abort();                                                        \
    }                                                                      \
  } while (false)

using Bytes = std::vector<uint8_t>;

// Every policy the stream format can express (canonical ones).
std::vector<rp::SequencePolicy> AllPolicies() {
  std::vector<rp::SequencePolicy> out;
  for (rp::Framing f : {rp::Framing::kSeparate, rp::Framing::kContinuous}) {
    out.push_back({f, rp::kFamilyNone, rp::AgePolicy::kCarry});
    for (uint8_t fam = 1; fam <= rp::kFamilyAll; ++fam) {
      for (rp::AgePolicy a : {rp::AgePolicy::kCarry, rp::AgePolicy::kReset,
                              rp::AgePolicy::kIntermediate}) {
        out.push_back({f, fam, a});
      }
    }
  }
  return out;
}

[[maybe_unused]] rp::SequencePolicy PolicyFromByte(uint8_t b) {
  const auto all = AllPolicies();
  return all[b % all.size()];
}

std::vector<rp::ModuleView> Views(const std::vector<const Bytes*>& ms) {
  std::vector<rp::ModuleView> v;
  for (const Bytes* m : ms) v.push_back({m->data(), m->size()});
  return v;
}

rp::EncodedSequence MustEncode(const std::vector<const Bytes*>& ms,
                               const rp::SequencePolicy& p) {
  const auto v = Views(ms);
  rp::EncodedSequence enc;
  RCHECK(rp::EncodeSequence(v.data(), v.size(), p, &enc) ==
         rp::SequenceError::kOk);
  return enc;
}

void MustRoundTrip(const std::vector<const Bytes*>& ms,
                   const rp::SequencePolicy& p, rp::EncodedSequence* out_enc =
                                                    nullptr) {
  const rp::EncodedSequence enc = MustEncode(ms, p);
  std::vector<Bytes> dec;
  rp::SequencePolicy parsed;
  const arixv_status st =
      rp::DecodeSequence(enc.stream.data(), enc.stream.size(), dec, &parsed);
  if (st != ARIXV_STATUS_OK) {
    std::fprintf(stderr, "decode failed: %s (framing=%d families=%d age=%d)\n",
                 arixv_status_string(st), static_cast<int>(p.framing),
                 p.families, static_cast<int>(p.age));
  }
  RCHECK(st == ARIXV_STATUS_OK);
  RCHECK(parsed == p);
  RCHECK(dec.size() == ms.size());
  for (size_t k = 0; k < ms.size(); ++k) RCHECK(dec[k] == *ms[k]);
  // The ARX2 decoder must reject research streams.
  Bytes ignored;
  RCHECK(arixv::research::Arx2Decode(enc.stream.data(), enc.stream.size(), ignored) ==
         ARIXV_STATUS_BAD_CONTAINER);
  // Deterministic.
  RCHECK(MustEncode(ms, p).stream == enc.stream);
  if (out_enc) *out_enc = enc;
}

Bytes StructuralPayload(const Bytes& m) {
  Bytes out;
  arixv::EncodeModule(m.data(), m.size(), out);
  return out;
}

// Control 2 identity: separately framed sequences with no transfer carry
// exactly the existing EncodeModule payloads.
[[maybe_unused]] void CheckEmptyTransferIdentity(
    const std::vector<const Bytes*>& ms) {
  const rp::EncodedSequence enc =
      MustEncode(ms, {rp::Framing::kSeparate, 0, rp::AgePolicy::kCarry});
  size_t off = enc.header_bytes;
  for (size_t k = 0; k < ms.size(); ++k) {
    const Bytes ref = StructuralPayload(*ms[k]);
    RCHECK(enc.member_payload_bytes[k] == ref.size());
    RCHECK(std::memcmp(enc.stream.data() + off, ref.data(), ref.size()) == 0);
    off += ref.size();
  }
  RCHECK(off == enc.stream.size());
}


// ---------------------------------------------------------------------------
// AXST (gate 3 structural prototype) helpers.
// ---------------------------------------------------------------------------
struct Roles {
  bool ok = false;
  std::vector<rp::IdRoleFrame> frames;
  std::vector<uint8_t> mask;
};

Roles RolesOf(const Bytes& m) {
  Roles r;
  r.ok = arixv::IsSpirvModelEligible(m.data(), m.size()) && m.size() >= 20 &&
         m[0] == 0x03 && m[1] == 0x02 && m[2] == 0x23 && m[3] == 0x07 &&
         rp::ModelIdRoles(m.data(), m.size(), &r.frames, &r.mask);
  return r;
}


#if !defined(ARIXV_RESEARCH_LIBFUZZER_RT) && \
    !defined(ARIXV_RESEARCH_LIBFUZZER_DEC)
// ---------------------------------------------------------------------------
// Synthetic module builders (same helpers as tests/model_test.cpp).
// ---------------------------------------------------------------------------
void AppendWord(uint32_t word, Bytes& out) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(word >> (8 * i)));
}

Bytes Module(const std::vector<uint32_t>& body, uint32_t version = 0x00010300u,
             uint32_t generator = 0, uint32_t bound = 1, uint32_t schema = 0) {
  Bytes out;
  AppendWord(0x07230203u, out);
  AppendWord(version, out);
  AppendWord(generator, out);
  AppendWord(bound, out);
  AppendWord(schema, out);
  for (uint32_t w : body) AppendWord(w, out);
  return out;
}

uint32_t Op(uint16_t opcode, uint16_t words) {
  return (static_cast<uint32_t>(words) << 16) | opcode;
}

void AppendString(const std::string& text, std::vector<uint32_t>& out) {
  std::string b = text;
  b.push_back('\0');
  while (b.size() % 4) b.push_back('\0');
  for (size_t i = 0; i < b.size(); i += 4) {
    uint32_t w = 0;
    for (int k = 0; k < 4; ++k) {
      w |= static_cast<uint32_t>(static_cast<uint8_t>(b[i + k])) << (8 * k);
    }
    out.push_back(w);
  }
}

// A small but structurally varied shader-like module: capabilities, ext-inst
// import, memory model, entry point, decorations (value enums), types,
// variables, a function with a block, and bit enums (function control).
Bytes ShaderLike(uint32_t seed) {
  std::vector<uint32_t> b;
  b.insert(b.end(), {Op(17, 2), 1});  // OpCapability Shader
  b.push_back(Op(11, 6));             // OpExtInstImport %1 "GLSL.std.450"
  b.push_back(1);
  AppendString("GLSL.std.450", b);
  b.insert(b.end(), {Op(14, 3), 0, 1});  // OpMemoryModel Logical GLSL450
  b.push_back(Op(15, 4 + 2));            // OpEntryPoint Fragment %5 "main"
  b.push_back(4);
  b.push_back(5);
  AppendString("main", b);
  b.insert(b.end(), {Op(16, 3), 5, 7});  // OpExecutionMode OriginUpperLeft
  b.insert(b.end(), {Op(71, 4), 10, 30, seed % 8});  // Location
  b.insert(b.end(), {Op(71, 4), 11, 30, (seed + 1) % 8});
  b.insert(b.end(), {Op(71, 4), 12, 33, seed % 4});  // Binding
  b.insert(b.end(), {Op(19, 2), 2});                 // OpTypeVoid %2
  b.insert(b.end(), {Op(33, 3), 3, 2});              // OpTypeFunction %3 %2
  b.insert(b.end(), {Op(22, 3), 6, 32});             // OpTypeFloat %6 32
  b.insert(b.end(), {Op(23, 4), 7, 6, 4});           // OpTypeVector %7 %6 4
  b.insert(b.end(), {Op(32, 4), 8, 3, 7});  // OpTypePointer %8 Output %7
  b.insert(b.end(), {Op(32, 4), 9, 1, 7});  // OpTypePointer %9 Input %7
  b.insert(b.end(), {Op(59, 4), 8, 10, 3});  // OpVariable %8 %10 Output
  b.insert(b.end(), {Op(59, 4), 9, 11, 1});  // OpVariable %9 %11 Input
  b.insert(b.end(), {Op(43, 4), 6, 13, 0x3f800000u + seed});  // OpConstant
  b.insert(b.end(), {Op(54, 5), 2, 5, seed % 4, 3});  // OpFunction (control)
  b.insert(b.end(), {Op(248, 2), 14});                // OpLabel
  b.insert(b.end(), {Op(61, 4), 7, 15, 11});          // OpLoad
  b.insert(b.end(), {Op(12, 6), 7, 16, 1, 31, 15});   // OpExtInst Normalize
  b.insert(b.end(), {Op(62, 3), 10, 16});             // OpStore
  b.insert(b.end(), {Op(253, 1), Op(56, 1)});         // OpReturn OpFunctionEnd
  return Module(b, 0x00010300u, (8u << 16) | 11u, 17);
}

Bytes HeaderOnly() { return Module({}, 0x00010000u, 0, 1); }

Bytes Swapped(const Bytes& m) {
  Bytes s = m;
  for (size_t i = 0; i + 3 < s.size(); i += 4) {
    std::swap(s[i], s[i + 3]);
    std::swap(s[i + 1], s[i + 2]);
  }
  return s;
}

bool ReadFile(const std::filesystem::path& p, Bytes* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

// Model-eligible modules decoded from the committed ARX2 golden vectors.
std::vector<Bytes> GoldenModules(const std::string& dir) {
  std::vector<Bytes> out;
  if (dir.empty()) return out;
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() == ".arx2") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    Bytes blob, dec;
    RCHECK(ReadFile(f, &blob));
    RCHECK(arixv::research::Arx2Decode(blob.data(), blob.size(), dec) == ARIXV_STATUS_OK);
    if (arixv::IsSpirvModelEligible(dec.data(), dec.size())) {
      out.push_back(dec);
    }
  }
  return out;
}

uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t Rand() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return static_cast<uint32_t>(g_rng >> 16);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void TestSingleModuleIdentity(const std::vector<Bytes>& fixtures) {
  // With one module there is no boundary: every policy (including
  // continuous framing) must carry exactly the EncodeModule payload.
  for (const Bytes& m : fixtures) {
    const Bytes ref = StructuralPayload(m);
    for (const rp::SequencePolicy& p : AllPolicies()) {
      const rp::EncodedSequence enc = MustEncode({&m}, p);
      RCHECK(enc.payload_bytes == ref.size());
      RCHECK(std::memcmp(enc.stream.data() + enc.header_bytes, ref.data(),
                         ref.size()) == 0);
    }
  }
}

void TestEmptyTransferIdentity(const std::vector<Bytes>& fx) {
  std::vector<const Bytes*> all;
  for (const Bytes& m : fx) all.push_back(&m);
  CheckEmptyTransferIdentity(all);
  std::vector<const Bytes*> rev(all.rbegin(), all.rend());
  CheckEmptyTransferIdentity(rev);
  CheckEmptyTransferIdentity({&fx[0], &fx[0], &fx[0]});
}

void TestRoundTrips(const std::vector<Bytes>& fx) {
  std::vector<const Bytes*> all;
  for (const Bytes& m : fx) all.push_back(&m);
  std::vector<std::vector<const Bytes*>> seqs = {
      all,
      std::vector<const Bytes*>(all.rbegin(), all.rend()),
      {&fx[0], &fx[0]},
      {&fx.back(), &fx[0], &fx.back()},
  };
  for (const auto& seq : seqs) {
    for (const rp::SequencePolicy& p : AllPolicies()) MustRoundTrip(seq, p);
  }
}

void TestStreamStructure() {
  const Bytes a = ShaderLike(0);
  const Bytes b = ShaderLike(1);
  // Continuous fresh vs separate empty: same models, different coder framing.
  const auto sep = MustEncode({&a, &b}, {rp::Framing::kSeparate, 0,
                                         rp::AgePolicy::kCarry});
  const auto cont = MustEncode({&a, &b}, {rp::Framing::kContinuous, 0,
                                          rp::AgePolicy::kCarry});
  RCHECK(sep.member_payload_bytes.size() == 2);
  RCHECK(cont.member_payload_bytes.empty());
  RCHECK(cont.header_bytes + 2 == sep.header_bytes ||
         cont.header_bytes < sep.header_bytes);
  // Header: magic, version, flags, count, body words.
  RCHECK(std::memcmp(sep.stream.data(), "AXRP", 4) == 0);
  RCHECK(sep.stream[4] == rp::kResearchVersion);
  RCHECK(sep.stream[5] == 0);
  RCHECK(cont.stream[5] == 1);
  const auto carry = MustEncode({&a, &a}, {rp::Framing::kContinuous,
                                           rp::kFamilyAll,
                                           rp::AgePolicy::kCarry});
  RCHECK(carry.stream[5] == (1 | (7 << 1)));
  // Transfer must actually change coding of a warmed duplicate.
  const auto fresh2 = MustEncode({&a, &a}, {rp::Framing::kContinuous, 0,
                                            rp::AgePolicy::kCarry});
  RCHECK(carry.stream != fresh2.stream);
  // Separate framing with transfer: member 0 unchanged, member 1 differs.
  const auto sep_carry = MustEncode({&a, &a}, {rp::Framing::kSeparate,
                                               rp::kFamilyAll,
                                               rp::AgePolicy::kCarry});
  const Bytes ref = StructuralPayload(a);
  RCHECK(sep_carry.member_payload_bytes[0] == ref.size());
  RCHECK(std::memcmp(sep_carry.stream.data() + sep_carry.header_bytes,
                     ref.data(), ref.size()) == 0);
  const Bytes second(sep_carry.stream.begin() + sep_carry.header_bytes +
                         ref.size(),
                     sep_carry.stream.end());
  RCHECK(second != ref);
  // Different age policies are distinguishable on a warmed boundary.
  const auto reset = MustEncode({&a, &a}, {rp::Framing::kContinuous,
                                           rp::kFamilyAll,
                                           rp::AgePolicy::kReset});
  RCHECK(reset.stream != carry.stream);
}

void TestTransferInspection(const std::vector<Bytes>& fx) {
  for (const Bytes& m : fx) {
    const rp::ModuleView v{m.data(), m.size()};
    // Empty transfer: a fresh model, every non-selected context uncreated.
    const rp::TransferReport none =
        rp::InspectTransfer(v, {rp::Framing::kSeparate, 0,
                                rp::AgePolicy::kCarry});
    RCHECK(none.non_allowlisted_fresh);
    RCHECK(none.contexts == 0 && none.probs == 0 && none.cdfs == 0);
    for (uint8_t fam = 1; fam <= rp::kFamilyAll; ++fam) {
      for (rp::AgePolicy a : {rp::AgePolicy::kCarry, rp::AgePolicy::kReset,
                              rp::AgePolicy::kIntermediate}) {
        const rp::TransferReport r =
            rp::InspectTransfer(v, {rp::Framing::kContinuous, fam, a});
        RCHECK(r.value_mismatches == 0);
        RCHECK(r.age_mismatches == 0);
        RCHECK(r.presence_mismatches == 0);
        RCHECK(r.non_allowlisted_fresh);
        if (fam & (rp::kFamilyOpcode | rp::kFamilyCount)) RCHECK(r.probs > 0);
      }
    }
    const rp::TransferReport all = rp::InspectTransfer(
        v, {rp::Framing::kContinuous, rp::kFamilyAll, rp::AgePolicy::kReset});
    // A real module creates contexts and ages them, so the reset/cap checks
    // above are non-vacuous.
    if (m.size() < 20 + 4 * 8) continue;
    RCHECK(all.contexts > 0);
    RCHECK(all.cdfs > 0);
    RCHECK(all.probs_with_age > 0);
    RCHECK(all.cdfs_with_age > 0);
    const rp::TransferReport mid = rp::InspectTransfer(
        v, {rp::Framing::kContinuous, rp::kFamilyAll,
            rp::AgePolicy::kIntermediate});
    RCHECK(mid.cdfs_age_above_cap > 0 || mid.probs_age_above_cap > 0);
  }
}

void TestRejectsUnsupported() {
  const Bytes good = ShaderLike(1);
  const Bytes text = {'h', 'e', 'l', 'l', 'o', '!', '!', '!'};
  Bytes odd = good;
  odd.push_back(0);
  Bytes short_hdr(good.begin(), good.begin() + 16);
  for (const Bytes* bad : std::vector<const Bytes*>{&text, &odd, &short_hdr}) {
    const auto v = Views({&good, bad});
    rp::EncodedSequence enc;
    size_t failed = 99;
    RCHECK(rp::EncodeSequence(v.data(), v.size(), {}, &enc, &failed) ==
           rp::SequenceError::kNotModelEligible);
    RCHECK(failed == 1);
    RCHECK(enc.stream.empty());
  }
  rp::EncodedSequence enc;
  RCHECK(rp::EncodeSequence(nullptr, 0, {}, &enc) == rp::SequenceError::kEmpty);
}

arixv_status Dec(const Bytes& s, size_t max_output = size_t{1} << 30) {
  std::vector<Bytes> out;
  const arixv_status st =
      rp::DecodeSequence(s.data(), s.size(), out, nullptr, max_output);
  if (st != ARIXV_STATUS_OK) RCHECK(out.empty());
  return st;
}

void TestHostileHeaders() {

  const Bytes a = ShaderLike(0);
  const Bytes b = ShaderLike(1);
  const auto sep = MustEncode({&a, &b}, {rp::Framing::kSeparate,
                                         rp::kFamilyAll, rp::AgePolicy::kCarry});
  const auto cont = MustEncode({&a, &b}, {rp::Framing::kContinuous,
                                          rp::kFamilyAll,
                                          rp::AgePolicy::kCarry});
  RCHECK(Dec(sep.stream) == ARIXV_STATUS_OK);
  RCHECK(Dec(cont.stream) == ARIXV_STATUS_OK);

  // Magic / version / flags.
  RCHECK(Dec({}) == ARIXV_STATUS_BAD_CONTAINER);
  RCHECK(Dec({'A', 'X', 'R'}) == ARIXV_STATUS_BAD_CONTAINER);
  RCHECK(Dec({'A', 'X', 'R', 'P'}) == ARIXV_STATUS_TRUNCATED);
  RCHECK(Dec({'A', 'X', 'R', 'P', 1}) == ARIXV_STATUS_TRUNCATED);
  auto with = [](Bytes s, size_t i, uint8_t v) {
    s[i] = v;
    return s;
  };
  RCHECK(Dec(with(sep.stream, 0, 'B')) == ARIXV_STATUS_BAD_CONTAINER);
  RCHECK(Dec(with(sep.stream, 4, 2)) == ARIXV_STATUS_BAD_CONTAINER);
  RCHECK(Dec(with(sep.stream, 5, 0x40)) == ARIXV_STATUS_BAD_CONTAINER);  // reserved
  RCHECK(Dec(with(sep.stream, 5, 0x30)) == ARIXV_STATUS_BAD_CONTAINER);  // age 3
  RCHECK(Dec(with(sep.stream, 5, 0x10)) == ARIXV_STATUS_BAD_CONTAINER);  // no fam
  // Research decoder rejects ARX2 blobs and vice versa.
  const Bytes arx2 = arixv::research::Arx2Encode(a.data(), a.size());
  RCHECK(Dec(arx2) == ARIXV_STATUS_BAD_CONTAINER);

  // Module count.
  Bytes zero = {'A', 'X', 'R', 'P', 1, 0, 0};
  RCHECK(Dec(zero) == ARIXV_STATUS_BAD_CONTAINER);
  Bytes big = {'A', 'X', 'R', 'P', 1, 0, 0xFE, 0xff, 0xff, 0xff, 0x7f};
  RCHECK(Dec(big) == ARIXV_STATUS_BAD_CONTAINER);
  Bytes many = {'A', 'X', 'R', 'P', 1, 0, 100, 0, 0};  // 100 modules, 2 bytes
  RCHECK(Dec(many) == ARIXV_STATUS_TRUNCATED);
  Bytes cut_count = {'A', 'X', 'R', 'P', 1, 0, 0xC0};
  RCHECK(Dec(cut_count) == ARIXV_STATUS_TRUNCATED);
  Bytes ff = {'A', 'X', 'R', 'P', 1, 0, 0xFF, 0, 0};
  RCHECK(Dec(ff) == ARIXV_STATUS_BAD_CONTAINER);
  Bytes noncanon = {'A', 'X', 'R', 'P', 1, 0, 0xFE, 1, 0, 0, 0, 0, 0};
  RCHECK(Dec(noncanon) == ARIXV_STATUS_BAD_CONTAINER);

  // Body size beyond the 32-bit module domain; total above max_output.
  Bytes huge = {'A', 'X', 'R', 'P', 1, 1, 1, 0xFE, 0xff, 0xff, 0xff, 0xff, 0};
  RCHECK(Dec(huge) == ARIXV_STATUS_BAD_CONTAINER);
  RCHECK(Dec(sep.stream, a.size() + b.size() - 4) == ARIXV_STATUS_TOO_LARGE);
  RCHECK(Dec(sep.stream, a.size() + b.size()) == ARIXV_STATUS_OK);
  RCHECK(Dec(cont.stream, a.size()) == ARIXV_STATUS_TOO_LARGE);

  // Truncation anywhere and trailing bytes are rejected, never accepted.
  for (const Bytes* s : std::vector<const Bytes*>{&sep.stream, &cont.stream}) {
    for (size_t n = 0; n < s->size(); ++n) {
      const Bytes cut(s->begin(), s->begin() + n);
      RCHECK(Dec(cut) != ARIXV_STATUS_OK);
    }
    Bytes extra = *s;
    extra.push_back(0);
    RCHECK(Dec(extra) != ARIXV_STATUS_OK);
  }
  // Separate: payload sizes must tile the rest exactly. Make member 0's
  // payload one byte larger (and the stream identical otherwise).
  {
    // Locate payload size fields: header = 4+1+1+count(1)+2 sizes+2 psizes.
    size_t pos = 6;
    uint32_t v = 0;
    RCHECK(rp::ReadResearchCompactUint32(sep.stream.data(), sep.stream.size(),
                                         &pos, &v) == ARIXV_STATUS_OK);
    for (int k = 0; k < 2; ++k) {
      RCHECK(rp::ReadResearchCompactUint32(sep.stream.data(),
                                           sep.stream.size(), &pos, &v) ==
             ARIXV_STATUS_OK);
    }
    const size_t psize0_at = pos;
    if (sep.stream[psize0_at] < 0xBF) {
      Bytes s = sep.stream;
      ++s[psize0_at];  // member 0 claims a byte of member 1
      RCHECK(Dec(s) != ARIXV_STATUS_OK);
      s[psize0_at] = static_cast<uint8_t>(sep.stream[psize0_at] - 1);
      RCHECK(Dec(s) != ARIXV_STATUS_OK);
    }
  }
  // Swapped module order in the size table desynchronizes decode.
  {
    const auto swapped = MustEncode({&b, &a}, {rp::Framing::kContinuous,
                                               rp::kFamilyAll,
                                               rp::AgePolicy::kCarry});
    Bytes frank(cont.stream.begin(), cont.stream.begin() + cont.header_bytes);
    frank.insert(frank.end(), swapped.stream.begin() + swapped.header_bytes,
                 swapped.stream.end());
    std::vector<Bytes> out;
    const arixv_status st = rp::DecodeSequence(frank.data(), frank.size(), out);
    RCHECK(st != ARIXV_STATUS_OK || out[0] != a || out[1] != b);
  }
  // Policy byte flipped (carry -> reset) with the same payload: never yields
  // the original modules silently.
  {
    Bytes s = cont.stream;
    s[5] = static_cast<uint8_t>(1 | (7 << 1) | (1 << 4));
    std::vector<Bytes> out;
    const arixv_status st = rp::DecodeSequence(s.data(), s.size(), out);
    RCHECK(st != ARIXV_STATUS_OK || out[0] != a || out[1] != b ||
           MustEncode({&a, &b}, {rp::Framing::kContinuous, rp::kFamilyAll,
                                 rp::AgePolicy::kReset})
                   .stream == cont.stream);
  }
}

void TestBoundaries() {
  // Header-only, swapped, tiny and duplicated modules across all policies.
  const Bytes h = HeaderOnly();
  const Bytes s = Swapped(ShaderLike(3));
  const Bytes x = ShaderLike(5);
  for (const rp::SequencePolicy& p : AllPolicies()) {
    MustRoundTrip({&h}, p);
    MustRoundTrip({&h, &h, &h}, p);
    MustRoundTrip({&s, &x, &h, &s}, p);
  }
  CheckEmptyTransferIdentity({&h, &s, &x});
}

// Deterministic bounded fuzz of the sequence path: mutated module round
// trips under random policies, and mutated-stream hostile decodes.
void DeterministicFuzz(const std::vector<Bytes>& fx, int iters) {
  for (int it = 0; it < iters; ++it) {
    const size_t n = 1 + Rand() % 4;
    std::vector<Bytes> mods;
    for (size_t k = 0; k < n; ++k) {
      Bytes m = fx[Rand() % fx.size()];
      const int flips = static_cast<int>(Rand() % 4);
      for (int f = 0; f < flips && m.size() > 4; ++f) {
        const size_t at = 4 + Rand() % (m.size() - 4);  // keep the magic
        m[at] = static_cast<uint8_t>(m[at] ^ (1u << (Rand() % 8)));
      }
      if (Rand() % 8 == 0 && m.size() > 24) m.resize(m.size() - 4);
      mods.push_back(m);
    }
    std::vector<const Bytes*> ptrs;
    for (const Bytes& m : mods) ptrs.push_back(&m);
    const rp::SequencePolicy p = PolicyFromByte(static_cast<uint8_t>(Rand()));
    rp::EncodedSequence enc;
    MustRoundTrip(ptrs, p, &enc);
    if (p.framing == rp::Framing::kSeparate && p.families == 0) {
      CheckEmptyTransferIdentity(ptrs);
    }
    // Hostile: mutate the valid stream.
    Bytes s = enc.stream;
    const int muts = 1 + static_cast<int>(Rand() % 4);
    for (int k = 0; k < muts && !s.empty(); ++k) {
      switch (Rand() % 4) {
        case 0: s[Rand() % s.size()] ^= static_cast<uint8_t>(1u << (Rand() % 8)); break;
        case 1: s.resize(Rand() % s.size()); break;
        case 2: s.insert(s.begin() + Rand() % s.size(), static_cast<uint8_t>(Rand())); break;
        case 3: s[Rand() % s.size()] = static_cast<uint8_t>(Rand()); break;
      }
    }
    std::vector<Bytes> out;
    (void)rp::DecodeSequence(s.data(), s.size(), out, nullptr, size_t{1} << 22);
  }
}


#endif  // !libFuzzer

}  // namespace

#if defined(ARIXV_RESEARCH_LIBFUZZER_RT)
// Round trip: the input is split at 4-byte-aligned SPIR-V magic words into
// modules (non-eligible segments dropped; a single module is duplicated).
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<Bytes> mods;
  size_t start = 0;
  for (size_t i = 4; i + 4 <= size; i += 4) {
    const uint32_t w = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16) |
                       (static_cast<uint32_t>(data[i + 3]) << 24);
    if (w == 0x07230203u || w == 0x03022307u) {
      mods.emplace_back(data + start, data + i);
      start = i;
    }
  }
  mods.emplace_back(data + start, data + size);
  std::vector<const Bytes*> ptrs;
  for (const Bytes& m : mods) {
    if (arixv::IsSpirvModelEligible(m.data(), m.size()) && ptrs.size() < 8) {
      ptrs.push_back(&m);
    }
  }
  if (ptrs.empty()) return 0;
  if (ptrs.size() == 1) ptrs.push_back(ptrs[0]);
  const uint8_t sel = size ? data[size - 1] : 0;
  CheckEmptyTransferIdentity(ptrs);
  MustRoundTrip(ptrs, PolicyFromByte(sel));
  MustRoundTrip(ptrs, {rp::Framing::kContinuous, rp::kFamilyAll,
                       static_cast<rp::AgePolicy>(sel % 3)});
  return 0;
}
#elif defined(ARIXV_RESEARCH_LIBFUZZER_DEC)
// Hostile decode: arbitrary bytes as a research stream. Anything accepted
// must re-encode (under the parsed policy) and decode back to itself.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<Bytes> out;
  rp::SequencePolicy p;
  if (rp::DecodeSequence(data, size, out, &p, size_t{1} << 24) !=
      ARIXV_STATUS_OK) {
    return 0;
  }
  std::vector<const Bytes*> ptrs;
  for (const Bytes& m : out) ptrs.push_back(&m);
  MustRoundTrip(ptrs, p);
  return 0;
}
#else
// Model ID roles (arixv_structure_dump): frames tile the body and roles
// cover every body word.
void TestModelIdRoles(const std::vector<Bytes>& fx) {
  for (const Bytes& m : fx) {
    const Roles r = RolesOf(m);
    if (!r.ok) continue;
    const size_t body = (m.size() - 20) / 4;
    RCHECK(r.mask.size() == body);
    uint32_t at = 0;
    for (const rp::IdRoleFrame& f : r.frames) {
      RCHECK(f.begin == at && f.nwords > 0);
      at += f.nwords;
    }
    RCHECK(at == body);
    for (uint8_t role : r.mask) RCHECK(role <= 2);
  }
  std::vector<rp::IdRoleFrame> frames;
  std::vector<uint8_t> mask;
  const Bytes junk = {1, 2, 3};
  RCHECK(!rp::ModelIdRoles(junk.data(), junk.size(), &frames, &mask));
}

int main(int argc, char** argv) {
  std::string golden_dir;
  int fuzz_iters = 200;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--golden-dir" && i + 1 < argc) golden_dir = argv[++i];
    else if (a == "--fuzz-iters" && i + 1 < argc) fuzz_iters = std::atoi(argv[++i]);
  }
  std::vector<Bytes> fx = GoldenModules(golden_dir);
  if (!golden_dir.empty()) RCHECK(fx.size() >= 4);  // golden 04..08
  fx.push_back(ShaderLike(0));
  fx.push_back(ShaderLike(1));
  fx.push_back(ShaderLike(2));
  fx.push_back(Swapped(ShaderLike(4)));
  std::printf("research_pack_test: %zu fixture modules\n", fx.size());

  TestSingleModuleIdentity(fx);
  TestEmptyTransferIdentity(fx);
  TestStreamStructure();
  TestTransferInspection(fx);
  TestRoundTrips(fx);
  TestRejectsUnsupported();
  TestHostileHeaders();
  TestBoundaries();
  DeterministicFuzz(fx, fuzz_iters);
  TestModelIdRoles(fx);
  std::printf("research_pack_test: ok (%d fuzz iterations)\n", fuzz_iters);
  return 0;
}
#endif
