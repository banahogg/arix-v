// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// RESEARCH ONLY (Track B gate 1b, docs/development/multi-module-gate1b.md).
//
// Internal multi-module sequence hook for the shader-pack research probes:
// encode/decode an ordered list of SPIR-V modules with an explicit model
// state-transfer policy and stream framing. This is not a production
// container, wire format, or API, and it never produces ARX2 bytes: research
// streams carry their own magic ("AXRP") and version byte, which the 1.0
// decoder rejects as kBadContainer.
//
// Only translation units compiled with -DARIXV_RESEARCH_PACK (the opt-in
// ARIXV_BUILD_RESEARCH_PACK CMake targets) may include this header. Standard
// library targets, public headers, and ARX2 mode selection are unaffected.

#ifndef ARIXV_RESEARCH_PACK_H_
#define ARIXV_RESEARCH_PACK_H_

#ifndef ARIXV_RESEARCH_PACK
#error "research_pack.h is research-only: compile with -DARIXV_RESEARCH_PACK"
#endif
#ifdef ARIXV_RESEARCH_TUNING
#error "research pack probes use the frozen default tuning (no ARIXV_RESEARCH_TUNING)"
#endif

#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "arixv/arixv.h"
#include "container.h"
#include "model.h"

#if !ARIXV_HAS_PACK_
#error "research pack probes reuse the ARXP codec: build with ARIXV_BUILD_PACK=ON"
#endif

namespace arixv {
inline namespace v1 {
namespace research {

// Research-only C++ conveniences over the 1.0 C API, for tools and tests.
#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
inline std::vector<uint8_t> Arx2Encode(const uint8_t* data, size_t size,
                                       bool force_coded = false) {
  std::vector<uint8_t> out;
  (void)EncodeInternal(data, size, force_coded, out);
  return out;
}
#endif
inline arixv_status Arx2Decode(const uint8_t* data, size_t size,
                               std::vector<uint8_t>& out) {
  out.clear();
  size_t n = 0;
  arixv_status s = arixv_decode(data, size, nullptr, &n, nullptr);
  if (s != ARIXV_STATUS_OK) return s;
  out.resize(n);
  s = arixv_decode(data, size, out.data(), &n, nullptr);
  if (s != ARIXV_STATUS_OK) {
    out.clear();
    return s;
  }
  out.resize(n);
  return ARIXV_STATUS_OK;
}

inline constexpr uint8_t kResearchMagic[] = {'A', 'X', 'R', 'P'};
inline constexpr size_t kResearchMagicBytes = sizeof(kResearchMagic);
inline constexpr uint8_t kResearchVersion = 1;

// Allowlisted transfer families (bitmask). Everything not listed here starts
// fresh in every module (see the state-lifetime inventory in the gate-1b doc).
enum Family : uint8_t {
  kFamilyNone = 0,
  kFamilyOpcode = 1,  // known_opcode, opcode index trees, order-1 contexts
  kFamilyCount = 2,   // word-count consistency/class models, optional tails
  kFamilyEnum = 4,    // value-enum and bit-enum operand models
  kFamilyAll = 7,
};

// Adaptation-age (warm-up counter) policy for transferred contexts. Values
// (probabilities / CDF lanes 1..15) are always carried; only the age, i.e.
// the packed Prob count (high 4 bits) and SymbolCdf::d[0], changes. The
// opcode order-1 promotion count is treated as an age too.
enum class AgePolicy : uint8_t {
  kCarry = 0,         // carry values and age unchanged
  kReset = 1,         // carry values, age 0 (fast early adaptation again)
  kIntermediate = 2,  // carry values, age clamped to the caps below
};

// Predeclared intermediate-age caps (one fixed diagnostic point, not a sweep).
// Default-tuning Prob ages are 0..3 (3 = steady); kSymbolRateCap trees
// saturate at 30, byte-rate trees at 126; order-1 promotion happens at 4.
inline constexpr uint16_t kIntermediateProbAge = 1;
inline constexpr uint16_t kIntermediateCdfAge = 6;
inline constexpr uint32_t kIntermediateOrder1Count = 2;

enum class Framing : uint8_t {
  kSeparate = 0,    // one independently flushed coder payload per module
  kContinuous = 1,  // one coder stream for the whole sequence, flushed once
};

struct SequencePolicy {
  Framing framing = Framing::kSeparate;
  uint8_t families = kFamilyNone;
  AgePolicy age = AgePolicy::kCarry;
};

inline bool operator==(const SequencePolicy& a, const SequencePolicy& b) {
  return a.framing == b.framing && a.families == b.families && a.age == b.age;
}

struct ModuleView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// ---------------------------------------------------------------------------
// Model-level hooks (src/model.cpp, ARIXV_RESEARCH_PACK only).
// ---------------------------------------------------------------------------

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
// White-box diagnostic for tests: encodes `source` alone, transfers its final
// state into a fresh model under `policy`, and compares the two along the
// same keyed walk the transfer uses. Counts are over selected families.
struct TransferReport {
  size_t contexts = 0;             // lazily created source contexts walked
  size_t probs = 0;                // Prob pairs compared
  size_t cdfs = 0;                 // SymbolCdf pairs compared
  size_t probs_with_age = 0;       // source Probs with nonzero age
  size_t cdfs_with_age = 0;        // source CDFs with nonzero d[0]
  size_t probs_age_above_cap = 0;  // source ages above the policy cap
  size_t cdfs_age_above_cap = 0;
  size_t value_mismatches = 0;     // must be 0: values always carried
  size_t age_mismatches = 0;       // must be 0: age == min(src, cap)
  size_t presence_mismatches = 0;  // must be 0: dst created iff src created
  size_t unselected_source_contexts = 0;  // source contexts left behind
  bool non_allowlisted_fresh = false;     // everything else == fresh model
  void Add(const TransferReport& o) {
    contexts += o.contexts;
    probs += o.probs;
    cdfs += o.cdfs;
    probs_with_age += o.probs_with_age;
    cdfs_with_age += o.cdfs_with_age;
    probs_age_above_cap += o.probs_age_above_cap;
    cdfs_age_above_cap += o.cdfs_age_above_cap;
    value_mismatches += o.value_mismatches;
    age_mismatches += o.age_mismatches;
    presence_mismatches += o.presence_mismatches;
  }
};
TransferReport InspectTransfer(const ModuleView& source,
                               const SequencePolicy& policy);

// Every module must satisfy IsSpirvModelEligible (callers check first; this
// asserts nothing and simply codes what it is given). Module k starts from a
// fresh ModelState; if policy.families != 0 and k > 0, the allowlisted
// families are then copied by value from module k-1's final state.
// Separate framing appends one payload per module to `payloads`; continuous
// framing appends exactly one payload.
void EncodeSequencePayloads(const ModuleView* modules, size_t count,
                            const SequencePolicy& policy,
                            std::vector<std::vector<uint8_t>>& payloads);
#ifdef ARIXV_RESEARCH_INST_COST
// RESEARCH ONLY (gate 2 attribution): encodes one module exactly like
// EncodeModule and returns cumulative encoder entropy in bits: element 0 is
// the start, element 1+i the total at the start of body instruction i, and
// the last element the total after the module. Requires ARIXV_STATS in the
// environment (returns false otherwise). Single-threaded only.
bool EncodeModuleInstCosts(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& payload,
                           std::vector<double>& cumulative_bits);
#endif
#endif

// Inverse of EncodeSequencePayloads. `payloads`/`payload_sizes` has `count`
// entries for separate framing and one for continuous. module_sizes[k] must
// be >= 20 and a multiple of 4; outs[k] receives exactly module_sizes[k]
// bytes on kOk. Every coder payload must be consumed exactly (separate: each
// payload after its module; continuous: once, after the last module).
arixv_status DecodeSequencePayloads(const uint8_t* const* payloads,
                              const size_t* payload_sizes,
                              const size_t* module_sizes, uint8_t* const* outs,
                              size_t count, const SequencePolicy& policy);

// ---------------------------------------------------------------------------
// Research stream framing (header-only). Layout, all sizes compact uint32
// (container.h AppendCompactUint32):
//   "AXRP" | version (1) | flags | module_count (>= 1)
//   | body_words[k] for each module (original size = 20 + 4 * body_words)
//   | separate only: payload_size[k] for each module
//   | payload bytes (separate: concatenated in module order; continuous: one)
// flags: bit 0 framing, bits 1-3 families, bits 4-5 age policy, bits 6-7 0.
// Canonical: age must be kCarry when families == 0. There is no checksum,
// per-module index, or random-access table; decode is whole-sequence only.
// ---------------------------------------------------------------------------

inline constexpr size_t kMaxResearchModules = 1u << 20;

inline uint8_t PolicyFlags(const SequencePolicy& p) {
  return static_cast<uint8_t>(static_cast<uint8_t>(p.framing) |
                              ((p.families & kFamilyAll) << 1) |
                              (static_cast<uint8_t>(p.age) << 4));
}

inline bool PolicyFromFlags(uint8_t flags, SequencePolicy* p) {
  if ((flags & 0xC0u) != 0) return false;
  const uint8_t age = static_cast<uint8_t>((flags >> 4) & 3u);
  if (age > static_cast<uint8_t>(AgePolicy::kIntermediate)) return false;
  p->framing = static_cast<Framing>(flags & 1u);
  p->families = static_cast<uint8_t>((flags >> 1) & kFamilyAll);
  p->age = static_cast<AgePolicy>(age);
  if (p->families == kFamilyNone && p->age != AgePolicy::kCarry) return false;
  return true;
}

inline arixv_status ReadResearchCompactUint32(const uint8_t* data, size_t size,
                                        size_t* pos, uint32_t* value) {
  if (*pos >= size) return ARIXV_STATUS_TRUNCATED;
  const uint8_t b0 = data[(*pos)++];
  auto need = [&](size_t n) { return size - *pos >= n; };
  if (b0 < 0xC0u) {
    *value = b0;
    return ARIXV_STATUS_OK;
  }
  if (b0 <= 0xFBu) {
    if (!need(1)) return ARIXV_STATUS_TRUNCATED;
    *value = kSizeOneByteLimit + (static_cast<uint32_t>(b0 - 0xC0u) << 8) +
             data[(*pos)++];
    return ARIXV_STATUS_OK;
  }
  int n = b0 == 0xFCu ? 2 : b0 == 0xFDu ? 3 : b0 == 0xFEu ? 4 : 0;
  if (n == 0) return ARIXV_STATUS_BAD_CONTAINER;
  if (!need(static_cast<size_t>(n))) return ARIXV_STATUS_TRUNCATED;
  uint32_t v = 0;
  for (int i = 0; i < n; ++i) {
    v |= static_cast<uint32_t>(data[*pos + static_cast<size_t>(i)]) << (8 * i);
  }
  *pos += static_cast<size_t>(n);
  if (b0 == 0xFCu) {
    *value = kSizeTwoByteLimit + v;
  } else if (b0 == 0xFDu) {
    *value = kSizeThreeByteLimit + v;
  } else {
    if (v < kSizeFourByteLimit) return ARIXV_STATUS_BAD_CONTAINER;  // non-canonical
    *value = v;
  }
  return ARIXV_STATUS_OK;
}

struct EncodedSequence {
  std::vector<uint8_t> stream;  // complete research stream
  size_t header_bytes = 0;      // everything before the first payload byte
  size_t payload_bytes = 0;     // coder payload bytes (sum over members)
  // Separate framing: each member's complete flushed payload size (an exact
  // per-module cost). Continuous: empty (no per-member byte attribution).
  std::vector<size_t> member_payload_bytes;
};

enum class SequenceError {
  kOk,
  kEmpty,
  kTooMany,
  kNotModelEligible,  // `failed_index` names the first rejected member
  kTooLarge,          // a member exceeds the 32-bit module size domain
};

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
inline SequenceError EncodeSequence(const ModuleView* modules, size_t count,
                                    const SequencePolicy& policy,
                                    EncodedSequence* out,
                                    size_t* failed_index = nullptr) {
  *out = EncodedSequence{};
  if (count == 0) return SequenceError::kEmpty;
  if (count > kMaxResearchModules) return SequenceError::kTooMany;
  for (size_t k = 0; k < count; ++k) {
    if (modules[k].size > std::numeric_limits<uint32_t>::max()) {
      if (failed_index) *failed_index = k;
      return SequenceError::kTooLarge;
    }
    if (!arixv::IsSpirvModelEligible(modules[k].data, modules[k].size)) {
      if (failed_index) *failed_index = k;
      return SequenceError::kNotModelEligible;
    }
  }
  std::vector<std::vector<uint8_t>> payloads;
  EncodeSequencePayloads(modules, count, policy, payloads);

  std::vector<uint8_t>& s = out->stream;
  s.insert(s.end(), kResearchMagic, kResearchMagic + kResearchMagicBytes);
  s.push_back(kResearchVersion);
  s.push_back(PolicyFlags(policy));
  AppendCompactUint32(static_cast<uint32_t>(count), s);
  for (size_t k = 0; k < count; ++k) {
    AppendCompactUint32(
        static_cast<uint32_t>((modules[k].size - kSpirvHeaderBytes) / 4u), s);
  }
  if (policy.framing == Framing::kSeparate) {
    for (const auto& p : payloads) {
      AppendCompactUint32(static_cast<uint32_t>(p.size()), s);
      out->member_payload_bytes.push_back(p.size());
    }
  }
  out->header_bytes = s.size();
  for (const auto& p : payloads) {
    s.insert(s.end(), p.begin(), p.end());
    out->payload_bytes += p.size();
  }
  return SequenceError::kOk;
}
#endif

// Decodes a complete research stream. `max_output` bounds the sum of the
// declared module sizes before any output is allocated; the module count is
// bounded by the stream length (each module needs at least one size byte).
inline arixv_status DecodeSequence(const uint8_t* data, size_t size,
                             std::vector<std::vector<uint8_t>>& modules,
                             SequencePolicy* policy_out = nullptr,
                             size_t max_output = size_t{1} << 30) {
  modules.clear();
  if (size < kResearchMagicBytes) return ARIXV_STATUS_BAD_CONTAINER;
  for (size_t i = 0; i < kResearchMagicBytes; ++i) {
    if (data[i] != kResearchMagic[i]) return ARIXV_STATUS_BAD_CONTAINER;
  }
  size_t pos = kResearchMagicBytes;
  if (size - pos < 2) return ARIXV_STATUS_TRUNCATED;
  if (data[pos++] != kResearchVersion) return ARIXV_STATUS_BAD_CONTAINER;
  SequencePolicy policy;
  if (!PolicyFromFlags(data[pos++], &policy)) return ARIXV_STATUS_BAD_CONTAINER;

  uint32_t count = 0;
  arixv_status st = ReadResearchCompactUint32(data, size, &pos, &count);
  if (st != ARIXV_STATUS_OK) return st;
  if (count == 0 || count > kMaxResearchModules) return ARIXV_STATUS_BAD_CONTAINER;
  // Each module needs a body-size byte (and, separately framed, a
  // payload-size byte): reject counts the remaining bytes cannot describe
  // before allocating per-module bookkeeping.
  const size_t per_module = policy.framing == Framing::kSeparate ? 2u : 1u;
  if ((size - pos) / per_module < count) return ARIXV_STATUS_TRUNCATED;

  std::vector<size_t> module_sizes(count);
  size_t total = 0;
  for (uint32_t k = 0; k < count; ++k) {
    uint32_t words = 0;
    st = ReadResearchCompactUint32(data, size, &pos, &words);
    if (st != ARIXV_STATUS_OK) return st;
    if (words > kMaxSpirvBodyWords) return ARIXV_STATUS_BAD_CONTAINER;
    const size_t bytes = kSpirvHeaderBytes + static_cast<size_t>(words) * 4u;
    if (bytes > max_output || total > max_output - bytes) {
      return ARIXV_STATUS_TOO_LARGE;
    }
    total += bytes;
    module_sizes[k] = bytes;
  }

  std::vector<const uint8_t*> payloads;
  std::vector<size_t> payload_sizes;
  if (policy.framing == Framing::kSeparate) {
    payload_sizes.resize(count);
    for (uint32_t k = 0; k < count; ++k) {
      uint32_t psize = 0;
      st = ReadResearchCompactUint32(data, size, &pos, &psize);
      if (st != ARIXV_STATUS_OK) return st;
      payload_sizes[k] = psize;
    }
    size_t offset = pos;
    payloads.resize(count);
    for (uint32_t k = 0; k < count; ++k) {
      if (payload_sizes[k] > size - offset) return ARIXV_STATUS_TRUNCATED;
      payloads[k] = data + offset;
      offset += payload_sizes[k];
    }
    if (offset != size) return ARIXV_STATUS_BAD_CONTAINER;  // trailing bytes
  } else {
    payloads.push_back(data + pos);
    payload_sizes.push_back(size - pos);
  }

  std::vector<std::vector<uint8_t>> out(count);
  std::vector<uint8_t*> outs(count);
  for (uint32_t k = 0; k < count; ++k) {
    out[k].resize(module_sizes[k]);
    outs[k] = out[k].data();
  }
  st = DecodeSequencePayloads(payloads.data(), payload_sizes.data(),
                              module_sizes.data(), outs.data(), count, policy);
  if (st != ARIXV_STATUS_OK) return st;
  modules = std::move(out);
  if (policy_out) *policy_out = policy;
  return ARIXV_STATUS_OK;
}

// Instruction frames and per-body-word ID roles (0 other, 1 IdResult, 2 ID
// reference) exactly as the model codes the module: the copy-source masks of
// the ARXP block codec (src/pack_block.inc). Used by arixv_structure_dump so
// encoder-side analyses see the same roles. Model-eligible input only (false
// otherwise).
struct IdRoleFrame {
  uint32_t begin;   // body word index
  uint32_t nwords;
};
#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
bool ModelIdRoles(const uint8_t* data, size_t size,
                  std::vector<IdRoleFrame>* frames,
                  std::vector<uint8_t>* mask);
#endif

}  // namespace research
}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_RESEARCH_PACK_H_
