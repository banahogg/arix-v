// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// ARXP shader-pack encoder: block layout policy and header writer
// (docs/format-arxp.md; public API in include/arixv/arixv.h). The block
// coder and copy planner are in src/pack_block.inc.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <numeric>
#include <utility>
#include <vector>

#include "arixv/arixv.h"
#include "container.h"
#include "model.h"
#include "pack.h"
#include "platform.h"

#if !defined(ARIXV_ENCODER) || (ARIXV_ENCODER == 0)
#error "src/arixv_pack_encode.cpp requires -DARIXV_ENCODER=1"
#endif

namespace arixv {
inline namespace v1 {
namespace pack {
namespace {

static_assert(ARIXV_PACK_ENCODE_OPTIONS_V1_SIZE == 24u,
              "v1 arixv_pack_encode_options is frozen");
static_assert(sizeof(arixv_pack_encode_options) ==
              ARIXV_PACK_ENCODE_OPTIONS_V1_SIZE);
static_assert(offsetof(arixv_pack_encode_options, parallelism) == 8u);
static_assert(offsetof(arixv_pack_encode_options, latency_budget_pct) == 12u);
static_assert(offsetof(arixv_pack_encode_options, max_block_bytes) == 16u);

constexpr uint32_t kDefaultParallelism = 8;

// x * pct / 100 without overflow: x <= 2^52 (2^20 modules of at most 2^32
// bytes, aliasing allowed) and pct <= 100000 would overflow 64 bits as a
// plain product. Exact: (100q + r) * p / 100 = q * p + floor(r * p / 100).
uint64_t ScalePct(uint64_t x, uint64_t pct) {
  return (x / 100) * pct + (x % 100) * pct / 100;
}
// A coded block stages two uint32_t rANS records per input byte in the
// worst case (byte model). On ILP32 a block above PTRDIFF_MAX / 8 bytes
// (~256 MiB) would exceed std::vector's max_size(), so such blocks are
// stored directly, as single-module arixv_encode does.
constexpr uint64_t kMaxCodedBlockBytes = kMaxSpan / (2u * sizeof(uint32_t));
constexpr uint32_t kDefaultBudgetPct = 150;

void PutVar(std::vector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}
void PutLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

bool ValidOptions(const arixv_pack_encode_options* o) {
  if (!o) return true;
  if (o->struct_size < ARIXV_PACK_ENCODE_OPTIONS_V1_SIZE ||
      o->struct_size > kMaxOptionsStructSize) {
    return false;
  }
  if (o->struct_size > sizeof(arixv_pack_encode_options) &&
      !HasZeroTrailingBytes(o, sizeof(arixv_pack_encode_options),
                            o->struct_size)) {
    return false;
  }
  if ((o->flags & ~uint32_t{ARIXV_PACK_ENCODE_FAST_DECODE}) != 0) return false;
  return o->latency_budget_pct == 0 ||
         (o->latency_budget_pct >= 100 && o->latency_budget_pct <= 100000);
}

// Worst-case header: fixed bytes and counts (8 + 3 * 3), checksum (8), and
// per module a size (5), block (3) and position (3) varint and a block
// length (10, every block a module).
uint64_t BoundOf(const size_t* sizes, uint32_t count) {
  if (!sizes || count == 0 || count > kMaxModules) return 0;
  uint64_t total = 8 + 3 * 3 + 8;
  for (uint32_t k = 0; k < count; ++k) {
    // Widen before adding: on ILP32 a size near UINT32_MAX would wrap.
    if (sizes[k] > UINT32_MAX || uint64_t{sizes[k]} > kMaxSpan) return 0;
    total += uint64_t{sizes[k]} + 5 + 3 + 3 + 10;
  }
  return total;
}

}  // namespace

// Block layout (docs/development/multi-module-pack-api.md §5). P is the
// effective parallelism and B the latency budget (percent / 100); decode
// time is modelled as proportional to decoded bytes, so no schedule beats
// the floor F = max(largest module, total / P).
//  - Stage 1: up to P/2 modules the whole-set plan copies most from (exact
//    duplicates count toward their first occurrence), capped at a quarter
//    of the modules, one per block. Stage 1 runs before stage 2, so only
//    modules of at most (B - 1) * F may join it: B = 1 means no stage 1.
//  - Stage 2: the rest, ordered so each module follows the one it copies
//    most from (DFS of that parent forest), cut nearest to a byte target of
//    at least max(rest / 2P, largest stage-2 module). List scheduling bounds
//    stage 2 by rest / P + largest block, so the target may grow to
//    B * F - largest stage-1 module - rest / P: spare budget buys larger
//    blocks.
//  - A block closes before it would exceed max_block_bytes.
//  - Blocks are numbered largest first; members are coded in ascending
//    module order. Blocks that do not compress are stored.
// All arithmetic is integer, so layout is identical on every host.
arixv_status Encode(const ModuleView* modules, size_t count,
                    const EncodeParams& params, std::vector<uint8_t>* out_ptr,
                    EncodeStats* stats) {
  std::vector<uint8_t>& out = *out_ptr;
  EncodeStats local;
  EncodeStats& st = stats ? *stats : local;
  st = EncodeStats{};
  out.clear();
  const uint64_t par = std::min<uint64_t>(
      params.parallelism ? params.parallelism : kDefaultParallelism, count);
  const uint64_t budget_pct =
      params.latency_budget_pct ? params.latency_budget_pct : kDefaultBudgetPct;
  uint64_t total_bytes = 0, largest_bytes = 0;
  for (size_t k = 0; k < count; ++k) {
    total_bytes += modules[k].size;
    largest_bytes = std::max<uint64_t>(largest_bytes, modules[k].size);
  }
  const uint64_t floor_bytes =
      std::max<uint64_t>(largest_bytes, total_bytes / par);

  std::vector<std::vector<uint32_t>> s1_blocks, s2_blocks;
  std::vector<char> in_stage1(count, 0);
  std::vector<uint32_t> seq;  // stage-2 members in assignment order
  std::vector<int64_t> parent(count, -1);
  if (par > 1) {  // implies count > 1
    Plan whole;
    PlanBlock(modules, count, nullptr, 0, &whole);
    std::vector<uint64_t> copied(count, 0);
    std::vector<uint32_t> from_stamp(count, 0);
    std::vector<uint64_t> from_val(count, 0);
    std::vector<uint32_t> touched;
    for (size_t t = 0; t < whole.size(); ++t) {
      touched.clear();
      const uint32_t stamp = static_cast<uint32_t>(t + 1);
      for (const Copy& c : whole[t]) {
        copied[c.base] += c.len;
        if (from_stamp[c.base] != stamp) {
          from_stamp[c.base] = stamp;
          from_val[c.base] = 0;
          touched.push_back(c.base);
        }
        from_val[c.base] += c.len;
      }
      // Most-copied base, ties to the lowest index (order-independent).
      uint64_t best = 0;
      for (uint32_t base : touched) {
        const uint64_t v = from_val[base];
        if (v > best || (v == best && best && base < parent[t])) {
          best = v;
          parent[t] = base;
        }
      }
    }
    // Exact duplicates: parent = first occurrence, credited with an
    // instruction-count estimate (about 16 bytes per instruction).
    std::vector<std::pair<uint64_t, uint32_t>> hashed(count);
    for (uint32_t k = 0; k < count; ++k) {
      const uint64_t hsh = Fnv1a(modules[k].data, modules[k].size) ^
                           (uint64_t{modules[k].size} << 1);
      hashed[k] = {hsh, k};
    }
    std::sort(hashed.begin(), hashed.end());
    for (size_t run = 0; run < count;) {
      size_t end = run + 1;
      while (end < count && hashed[end].first == hashed[run].first) ++end;
      for (size_t a = run + 1; a < end; ++a) {
        const uint32_t k = hashed[a].second;
        for (size_t b = run; b < a; ++b) {
          const uint32_t j = hashed[b].second;
          if (modules[j].size == modules[k].size &&
              (modules[k].size == 0 ||
               std::memcmp(modules[j].data, modules[k].data, modules[k].size) ==
                   0)) {
            parent[k] = j;
            copied[j] += std::max<uint64_t>(1, modules[k].size / 16);
            break;
          }
        }
      }
      run = end;
    }
    std::vector<uint32_t> idx(count);
    std::iota(idx.begin(), idx.end(), 0u);
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
      return copied[a] != copied[b] ? copied[a] > copied[b] : a < b;
    });
    uint64_t want = std::max<uint64_t>(par / 2, 1);
    want = std::min<uint64_t>(want, count / 4);
    want = std::min<uint64_t>(want, count - 1);
    const uint64_t limit = ScalePct(floor_bytes, budget_pct - 100);
    // No allowance (100%, or all-empty sets) means no stage 1 at all: even
    // empty modules would otherwise fit a zero limit.
    if (limit == 0) want = 0;
    for (size_t j = 0, taken = 0; j < count && taken < want; ++j) {
      if (copied[idx[j]] == 0) break;
      if (modules[idx[j]].size > limit) continue;
      in_stage1[idx[j]] = 1;
      ++taken;
    }
    for (uint32_t k = 0; k < count; ++k) {
      if (in_stage1[k]) s1_blocks.push_back({k});
    }
    // DFS of the parent forest; stage-1 members act as roots' parents.
    std::vector<std::vector<uint32_t>> kids(count);
    std::vector<uint32_t> roots;
    for (uint32_t k = 0; k < count; ++k) {
      if (in_stage1[k]) continue;
      if (parent[k] >= 0 && !in_stage1[parent[k]]) {
        kids[parent[k]].push_back(k);
      } else {
        roots.push_back(k);
      }
    }
    std::vector<uint32_t> stack;
    for (uint32_t r : roots) {
      stack.push_back(r);
      while (!stack.empty()) {
        const uint32_t k = stack.back();
        stack.pop_back();
        seq.push_back(k);
        for (size_t c = kids[k].size(); c-- > 0;) stack.push_back(kids[k][c]);
      }
    }
  } else {
    for (uint32_t k = 0; k < count; ++k) seq.push_back(k);
  }
  {
    const uint64_t target = par > 1 ? 2 * par : 1;  // ~two blocks per worker
    uint64_t rest_bytes = 0, largest_rest = 0;
    for (uint32_t k : seq) {
      rest_bytes += modules[k].size;
      largest_rest = std::max<uint64_t>(largest_rest, modules[k].size);
    }
    const uint64_t per =
        std::max<uint64_t>(1, (seq.size() + target - 1) / target);
    uint64_t per_bytes = rest_bytes / target + (rest_bytes % target != 0);
    per_bytes = std::max(per_bytes, largest_rest);
    uint64_t s1_largest = 0;
    for (uint32_t k = 0; k < count; ++k) {
      if (in_stage1[k]) {
        s1_largest = std::max<uint64_t>(s1_largest, modules[k].size);
      }
    }
    const int64_t grow = static_cast<int64_t>(ScalePct(floor_bytes, budget_pct)) -
                         static_cast<int64_t>(s1_largest) -
                         static_cast<int64_t>(rest_bytes / par);
    if (par > 1 && grow > static_cast<int64_t>(per_bytes)) {
      per_bytes = std::min<uint64_t>(static_cast<uint64_t>(grow), total_bytes);
    }
    const bool by_bytes = rest_bytes > 0;
    const uint64_t cap = params.max_block_bytes;
    std::vector<uint32_t> cur;
    uint64_t cur_bytes = 0;
    for (uint32_t k : seq) {
      // Close the block when adding k would end farther from the target
      // than stopping (nearest rounding).
      const bool by_policy =
          par > 1 && (by_bytes ? cur_bytes + modules[k].size / 2 > per_bytes
                               : cur.size() >= per);
      const bool full =
          !cur.empty() &&
          (by_policy ||
           (cap && modules[k].size > cap - std::min(cur_bytes, cap)));
      if (full) {
        s2_blocks.push_back(std::move(cur));
        cur.clear();
        cur_bytes = 0;
      }
      cur.push_back(k);
      cur_bytes += modules[k].size;
    }
    if (!cur.empty()) s2_blocks.push_back(std::move(cur));
    // Ascending coding order inside each block: copy sources come first
    // (plan parents precede their children) and the header can use the
    // implicit order. The test hook reverses it.
    for (auto& blk : s2_blocks) {
      std::sort(blk.begin(), blk.end());
      if (params.reverse_coding_order) std::reverse(blk.begin(), blk.end());
    }
  }
  auto bytes_of = [&](const std::vector<uint32_t>& b) {
    uint64_t s = 0;
    for (uint32_t k : b) s += modules[k].size;
    return s;
  };
  auto sort_blocks = [&](std::vector<std::vector<uint32_t>>& blks) {
    const size_t n = blks.size();
    std::vector<uint64_t> sz(n);
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; ++i) {
      sz[i] = bytes_of(blks[i]);
      order[i] = static_cast<uint32_t>(i);
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return sz[a] != sz[b] ? sz[a] > sz[b] : a < b;
    });
    std::vector<std::vector<uint32_t>> sorted;
    sorted.reserve(n);
    for (uint32_t i : order) sorted.push_back(std::move(blks[i]));
    blks = std::move(sorted);
  };
  sort_blocks(s1_blocks);
  sort_blocks(s2_blocks);

  // Code every block. Stage-1 blocks are decoded back so stage-2 blocks see
  // the shared members exactly as the decoder will (stored blocks share
  // bytes only).
  std::vector<std::vector<uint8_t>> payloads;
  std::vector<uint8_t> stored;
  std::vector<std::vector<DecodedMember>> s1_decoded;
  std::vector<SharedMember> shared;
  auto views_of = [&](const std::vector<uint32_t>& b) {
    std::vector<ModuleView> v;
    for (uint32_t k : b) v.push_back(modules[k]);
    return v;
  };
  auto raw_of = [&](const std::vector<uint32_t>& b) {
    std::vector<uint8_t> raw;
    for (uint32_t k : b) {
      if (modules[k].size) {
        raw.insert(raw.end(), modules[k].data,
                   modules[k].data + modules[k].size);
      }
    }
    return raw;
  };
  const uint8_t codec = params.codec_flags;
  for (int stage = 1; stage <= 2; ++stage) {
    if (stage == 2) {
      for (const auto& dec : s1_decoded) {
        for (const DecodedMember& d : dec) {
          SharedMember m;
          m.bytes = d.data();
          m.size = d.size();
          if (d.valid) {
            m.valid = true;
            m.words = d.words.data();
            m.nwords = d.words.size();
            m.starts = d.starts.data();
            m.nframes = d.starts.size() - 1;
            m.mask = d.mask.data();
          }
          shared.push_back(m);
        }
      }
    }
    for (const auto& b : stage == 1 ? s1_blocks : s2_blocks) {
      const auto v = views_of(b);
      const SharedMember* sh = stage == 2 ? shared.data() : nullptr;
      const size_t ns = stage == 2 ? shared.size() : 0;
      const uint64_t raw_bytes = bytes_of(b);
      std::vector<uint8_t> payload;
      bool use_stored = raw_bytes > kMaxCodedBlockBytes;
      if (!use_stored) {
        Plan plan;
        PlanBlock(v.data(), v.size(), sh, ns, &plan);
        if (!EncodeBlock(v.data(), v.size(), plan, codec, sh, ns, &payload)) {
          return ARIXV_STATUS_INVALID_ARGUMENT;  // internal inconsistency
        }
        use_stored = payload.size() >= raw_bytes;
      }
      if (stage == 1) {
        s1_decoded.emplace_back();
        std::vector<DecodedMember>& dec = s1_decoded.back();
        if (use_stored) {
          dec.resize(b.size());
          for (size_t j = 0; j < b.size(); ++j) {
            const ModuleView& m = modules[b[j]];
            dec[j].bytes.assign(m.data, m.data + m.size);
          }
        } else {
          std::vector<uint64_t> sizes;
          for (uint32_t k : b) sizes.push_back(modules[k].size);
          const arixv_status ds =
              DecodeBlock(payload.data(), payload.size(), codec, sizes.data(),
                          static_cast<uint32_t>(b.size()), nullptr, 0, &dec);
          if (ds != ARIXV_STATUS_OK) return ARIXV_STATUS_INVALID_ARGUMENT;
        }
        st.stage1_bytes += raw_bytes;
      }
      st.largest_block_bytes = std::max(st.largest_block_bytes, raw_bytes);
      payloads.push_back(use_stored ? raw_of(b) : std::move(payload));
      stored.push_back(use_stored ? 1 : 0);
    }
  }

  // Header.
  const uint32_t s1 = static_cast<uint32_t>(s1_blocks.size());
  const uint32_t s2 = static_cast<uint32_t>(s2_blocks.size());
  std::vector<uint32_t> block_of(count), order_of(count);
  bool implicit = true;
  uint32_t gb = 0;
  for (const auto* list : {&s1_blocks, &s2_blocks}) {
    for (const auto& b : *list) {
      for (uint32_t j = 0; j < b.size(); ++j) {
        block_of[b[j]] = gb;
        order_of[b[j]] = j;
        if (j && b[j] < b[j - 1]) implicit = false;
      }
      ++gb;
    }
  }
  out.insert(out.end(), kMagic, kMagic + 4);
  out.push_back(kVersion);
  out.push_back(implicit ? kPackImplicitOrder : 0);
  out.push_back(codec);
  out.push_back(0);
  PutVar(out, count);
  PutVar(out, s1);
  PutVar(out, s2);
  for (size_t k = 0; k < count; ++k) {
    PutVar(out, modules[k].size);
    PutVar(out, block_of[k]);
  }
  if (!implicit) {
    for (size_t k = 0; k < count; ++k) PutVar(out, order_of[k]);
  }
  for (size_t b = 0; b < payloads.size(); ++b) {
    PutVar(out, (uint64_t{payloads[b].size()} << 1) | stored[b]);
  }
  PutLe64(out, Fnv1a(out.data(), out.size()));
  st.header_bytes = out.size();
  for (const auto& p : payloads) out.insert(out.end(), p.begin(), p.end());
  for (uint8_t s : stored) st.stored_blocks += s;
  st.stage1_blocks = s1;
  st.stage2_blocks = s2;
  return ARIXV_STATUS_OK;
}

namespace {

arixv_status EncodeImpl(const void* const* modules, const size_t* sizes,
                        uint32_t count, void* out, size_t* out_size,
                        const arixv_pack_encode_options* opts) {
  if (!modules || !sizes || !out || !out_size || !ValidOptions(opts) ||
      count == 0 || count > kMaxModules) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  std::vector<ModuleView> views(count);
  for (uint32_t k = 0; k < count; ++k) {
    if (sizes[k] > UINT32_MAX || uint64_t{sizes[k]} > kMaxSpan ||
        (!modules[k] && sizes[k])) {
      return ARIXV_STATUS_INVALID_ARGUMENT;
    }
    views[k] = {static_cast<const uint8_t*>(modules[k]), sizes[k]};
  }
  EncodeParams p;
  if (opts) {
    p.parallelism = opts->parallelism;
    p.latency_budget_pct = opts->latency_budget_pct;
    p.max_block_bytes = opts->max_block_bytes;
    if (opts->flags & ARIXV_PACK_ENCODE_FAST_DECODE) {
      p.codec_flags |= kCodecFastDecode;
    }
  }
  std::vector<uint8_t> encoded;
  const arixv_status st = Encode(views.data(), count, p, &encoded, nullptr);
  if (st != ARIXV_STATUS_OK) return st;
  if (*out_size < encoded.size()) {
    *out_size = encoded.size();
    return ARIXV_STATUS_BUFFER_TOO_SMALL;
  }
  std::memcpy(out, encoded.data(), encoded.size());
  *out_size = encoded.size();
  return ARIXV_STATUS_OK;
}

}  // namespace
}  // namespace pack
}  // namespace v1
}  // namespace arixv

extern "C" {

size_t ARIXV_CALL arixv_pack_encode_bound(
    const size_t* sizes, uint32_t count,
    const arixv_pack_encode_options* opts) ARIXV_NOEXCEPT {
  if (!arixv::v1::pack::ValidOptions(opts)) return 0;
  const uint64_t b = arixv::v1::pack::BoundOf(sizes, count);
  if (b > arixv::v1::pack::kMaxSpan || b > SIZE_MAX) return 0;
  return static_cast<size_t>(b);
}

arixv_status ARIXV_CALL arixv_pack_encode(
    const void* const* modules, const size_t* sizes, uint32_t count,
    void* out, size_t* out_size,
    const arixv_pack_encode_options* opts) ARIXV_NOEXCEPT {
#if ARIXV_HAS_EXCEPTIONS
  try {
    return arixv::v1::pack::EncodeImpl(modules, sizes, count, out, out_size,
                                       opts);
  } catch (...) {
    return ARIXV_STATUS_OUT_OF_MEMORY;
  }
#else
  return arixv::v1::pack::EncodeImpl(modules, sizes, count, out, out_size,
                                     opts);
#endif
}

}  // extern "C"
