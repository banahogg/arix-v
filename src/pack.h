// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// Internal interface of the ARXP shader-pack implementation
// (docs/format-arxp.md):
//  - block codec, in src/pack_block.inc (compiled into src/model.cpp, since
//    it drives the model through CodeModule's hook interface);
//  - container and two-stage decode, in src/arixv_pack.cpp;
//  - encoder layout policy and header writer, in src/arixv_pack_encode.cpp.

#ifndef ARIXV_PACK_H_
#define ARIXV_PACK_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "arixv/arixv.h"

namespace arixv {
inline namespace v1 {
namespace pack {

inline constexpr uint8_t kMagic[4] = {'A', 'R', 'X', 'P'};
inline constexpr uint8_t kVersion = 1;
// pack_flags (header byte 5).
inline constexpr uint8_t kPackImplicitOrder = 1;
// codec_flags (header byte 6): options of every coded block.
inline constexpr uint8_t kCodecWarm = 1;        // warm statistics
inline constexpr uint8_t kCodecFastDecode = 2;  // no-adapt train-through
inline constexpr uint8_t kCodecMask = 3;
inline constexpr uint32_t kMaxModules = 1u << 20;
inline constexpr uint64_t kMaxSpan =
    static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());

// FNV-1a 64, the header checksum.
inline uint64_t Fnv1a(const uint8_t* p, size_t n,
                      uint64_t h = 0xcbf29ce484222325ull) {
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

// A decoded member that later members may copy from or duplicate: plain
// pointers, so the data can live in a block's own storage or in scratch.
struct SharedMember {
  const uint8_t* bytes = nullptr;  // module bytes
  size_t size = 0;
  bool valid = false;              // a copy source (fields below set)
  const uint32_t* words = nullptr; // whole module, native order
  size_t nwords = 0;
  const uint32_t* starts = nullptr;  // frame start offsets, nframes + 1
  size_t nframes = 0;
  const uint8_t* mask = nullptr;     // per body word: 0 other, 1 def, 2 use
};

// One decoded block member, owned. Structurally coded members keep their
// bytes in `words`; the others in `bytes`.
struct DecodedMember {
  std::vector<uint32_t> words;
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> starts;  // copy sources only
  std::vector<uint8_t> mask;     // copy sources only
  bool structural = false;
  bool valid = false;            // a copy source
  const uint8_t* data() const {
    return structural ? reinterpret_cast<const uint8_t*>(words.data())
                      : bytes.data();
  }
  size_t size() const { return structural ? words.size() * 4 : bytes.size(); }
};

bool HostIsLittleEndian();

// One planned copy: instruction `at` of a member copies `len` instructions
// starting at instruction `pos` of member `base` (global index: shared
// members first).
struct Copy {
  uint32_t at, base, pos, len;
};

// Decodes one coded block payload: `count` members with the given sizes in
// coding order, after `ns` shared members. The payload must be consumed
// exactly. On error `*out` is unspecified.
arixv_status DecodeBlock(const uint8_t* payload, size_t payload_size,
                         uint8_t codec_flags, const uint64_t* sizes,
                         uint32_t count, const SharedMember* shared,
                         size_t ns, std::vector<DecodedMember>* out);

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
struct ModuleView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

using Plan = std::vector<std::vector<Copy>>;  // per member, sorted by `at`

// Greedy copy planner over the block's members, with the shared members as
// extra copy sources. `plan` gets one entry per member.
void PlanBlock(const ModuleView* modules, size_t count,
               const SharedMember* shared, size_t ns, Plan* plan);

// Encodes one block payload following `plan` (copies are an encoder
// choice; the decoder needs no plan). Returns false only on an internal
// inconsistency.
bool EncodeBlock(const ModuleView* modules, size_t count, const Plan& plan,
                 uint8_t codec_flags, const SharedMember* shared, size_t ns,
                 std::vector<uint8_t>* payload);

// Test hook: the planner's integer log2, floor(log2(x) * 2^16), x >= 1.
int64_t Log2Q16(uint64_t x);

// Test and tooling hook: instruction frames and per-body-word ID roles
// (0 other, 1 result ID, 2 ID use) exactly as the model codes the module,
// i.e. the copy-source view of it. False unless model-eligible.
struct FrameSpan {
  uint32_t begin;   // body word index
  uint32_t nwords;
};
bool ModuleIdRoles(const uint8_t* data, size_t size,
                   std::vector<FrameSpan>* frames, std::vector<uint8_t>* mask);

// Internal encoder entry point (arixv_pack_encode plus test-only knobs).
struct EncodeParams {
  uint32_t parallelism = 0;         // 0 = default
  uint32_t latency_budget_pct = 0;  // 0 = default
  uint64_t max_block_bytes = 0;
  uint8_t codec_flags = kCodecWarm;
  bool reverse_coding_order = false;  // test hook: explicit header positions
};
struct EncodeStats {
  uint32_t stage1_blocks = 0, stage2_blocks = 0, stored_blocks = 0;
  uint64_t stage1_bytes = 0, largest_block_bytes = 0, header_bytes = 0;
};
// Inputs must already be validated (count in [1, kMaxModules], sizes at
// most UINT32_MAX, data non-NULL when size > 0, codec_flags within
// kCodecMask, budget 0 or 100..100000).
arixv_status Encode(const ModuleView* modules, size_t count,
                    const EncodeParams& params, std::vector<uint8_t>* out,
                    EncodeStats* stats);
#endif

// Test hook: header length (through the checksum) of a valid pack, else 0.
size_t HeaderBytes(const void* in, size_t in_size);

}  // namespace pack
}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_PACK_H_
