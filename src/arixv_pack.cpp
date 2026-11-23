// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// ARXP shader-pack container and two-stage decode (docs/format-arxp.md,
// public API in include/arixv/arixv.h).
//
//  - Stateless calls; every call validates the whole header (no session).
//  - Ownership is a partition: one (block, position) entry per module,
//    positions checked unique, every block non-empty.
//  - Payload offsets are prefix sums of block lengths: disjoint by
//    construction, ending exactly at the input end.
//  - Blocks are coded (src/pack_block.inc) or stored verbatim.
//  - Scratch layout is derived from module sizes only: cache-line
//    completion records, then one 64-byte-aligned slice per stage-1 block.
//    Stage 2 reads slices in place.
//  - Checks run in the documented order (arguments, header, header-
//    dependent arguments, buffers, records); nothing is written before they
//    pass. Stage-1 calls clear their record before writing their slice and
//    write it last on success.
//
// Format limits: module_count <= 2^20, module size <= UINT32_MAX, block
// count <= module_count, so all header and scratch arithmetic fits in 64
// bits.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "arixv/arixv.h"
#include "container.h"
#include "pack.h"
#include "platform.h"

namespace arixv {
inline namespace v1 {
namespace pack {
namespace {

static_assert(ARIXV_PACK_INFO_V1_SIZE == 40u, "v1 arixv_pack_info is frozen");
static_assert(sizeof(arixv_pack_info) == ARIXV_PACK_INFO_V1_SIZE);
static_assert(offsetof(arixv_pack_info, magic) == 4u);
static_assert(offsetof(arixv_pack_info, module_count) == 12u);
static_assert(offsetof(arixv_pack_info, total_output_size) == 24u);
static_assert(offsetof(arixv_pack_info, scratch_size) == 32u);
static_assert(sizeof(arixv_pack_module_info) == 16u);
static_assert(offsetof(arixv_pack_module_info, block_index) == 8u);
static_assert(offsetof(arixv_pack_module_info, stage) == 12u);
static_assert(offsetof(arixv_pack_module_info, flags) == 14u);
static_assert(sizeof(arixv_pack_outputs) == ARIXV_PACK_OUTPUTS_V1_SIZE);
static_assert(ARIXV_PACK_DECODE_OPTIONS_V1_SIZE == 8u);
static_assert(sizeof(arixv_pack_decode_options) ==
              ARIXV_PACK_DECODE_OPTIONS_V1_SIZE);

constexpr size_t kRecordStride = 64;
constexpr uint64_t kRecordMagic = 0x454e4f4450585241ull;  // "ARXPDONE"
constexpr uint32_t kInfoMagic = 0x50585241u;              // "ARXP"

template <class T>
T Load(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
[[maybe_unused]] uint64_t LoadLe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

uint64_t Pad8(uint64_t v) { return (v + 7) & ~uint64_t{7}; }
uint64_t Align64(uint64_t v) { return (v + 63) & ~uint64_t{63}; }

// Per stage-1 member slice entry: a descriptor, the module bytes (padded,
// read as words when valid), the per-word role mask, and frame start
// offsets (uint32, nframes + 1 entries; upper bound one frame per word).
struct MemberDesc {
  uint32_t valid;
  uint32_t nframes;
  uint64_t size;
};
uint64_t MemberBytes(uint64_t size) {
  const uint64_t w = size / 4;
  return sizeof(MemberDesc) + Pad8(size) + Pad8(w) + Pad8(4 * (w + 1));
}

struct Record {
  uint64_t magic;
  uint64_t checksum;
  uint32_t index;
  uint32_t reserved;  // 0; no indeterminate padding bytes
};

struct ModMeta {
  uint32_t size = 0;   // validated <= UINT32_MAX
  uint32_t block = 0;  // global block index
};

struct BlockMeta {
  uint64_t payload_off = 0;  // absolute
  uint64_t payload_len = 0;
  uint64_t slice_off = 0;    // stage-1 only, from aligned base
  uint32_t first = 0;        // offset into members
  uint32_t count = 0;
  uint8_t stored = 0;
};

struct Header {
  uint32_t m = 0, s1 = 0, s2 = 0;
  size_t header_len = 0;
  uint64_t checksum = 0;
  uint8_t codec = 0;
  std::vector<ModMeta> mods;      // per module
  std::vector<BlockMeta> blocks;  // per block (s1 + s2)
  std::vector<uint32_t> members;  // block-major, coding order
  uint64_t total = 0, scratch_size = 0;
  uint64_t stage1_modules = 0;
};

// Canonical unsigned LEB128: shortest form, at most 64 bits.
enum class Var { kOk, kTruncated, kBad };
Var ReadVar(const uint8_t* in, size_t n, size_t* pos, uint64_t* v) {
  uint64_t r = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (*pos >= n) return Var::kTruncated;
    const uint8_t c = in[(*pos)++];
    if (shift == 63 && (c & 0xfe)) return Var::kBad;  // overflow
    r |= uint64_t{c & 0x7fu} << shift;
    if (!(c & 0x80)) {
      if (c == 0 && shift > 0) return Var::kBad;  // non-minimal
      *v = r;
      return Var::kOk;
    }
  }
  return Var::kBad;
}
arixv_status VarStatus(Var v) {
  return v == Var::kTruncated ? ARIXV_STATUS_TRUNCATED
                              : ARIXV_STATUS_BAD_CONTAINER;
}

// Whole-header validation (format-arxp.md §2.1, in that order).
arixv_status Parse(const void* in_v, size_t n, Header* h) {
  const uint8_t* in = static_cast<const uint8_t*>(in_v);
  if (!HostIsLittleEndian()) return ARIXV_STATUS_BAD_CONTAINER;
  if (n > kMaxSpan) return ARIXV_STATUS_INVALID_ARGUMENT;
  if (n < 4 || std::memcmp(in, kMagic, 4) != 0) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }
  if (n < 5) return ARIXV_STATUS_TRUNCATED;
  if (in[4] != kVersion) return ARIXV_STATUS_UNSUPPORTED_FORMAT;
  if (n < 8) return ARIXV_STATUS_TRUNCATED;
  const uint8_t pack_flags = in[5];
  const uint8_t codec = in[6];
  if ((pack_flags & ~kPackImplicitOrder) != 0 ||
      (codec & ~kCodecMask) != 0 || in[7] != 0) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }
  h->codec = codec;
  const bool implicit = (pack_flags & kPackImplicitOrder) != 0;
  size_t pos = 8;
  uint64_t m = 0, s1 = 0, s2 = 0;
  Var v;
  if ((v = ReadVar(in, n, &pos, &m)) != Var::kOk ||
      (v = ReadVar(in, n, &pos, &s1)) != Var::kOk ||
      (v = ReadVar(in, n, &pos, &s2)) != Var::kOk) {
    return VarStatus(v);
  }
  if (m == 0 || m > kMaxModules || s2 == 0 || s1 > m || s2 > m - s1) {
    return ARIXV_STATUS_BAD_CONTAINER;
  }
  // Every mandatory header byte must be present before any table is
  // allocated: per module two (or three) varints, per block one, checksum.
  const uint64_t nb64 = s1 + s2;
  if ((implicit ? 2 : 3) * m + nb64 + 8 > n - pos) {
    return ARIXV_STATUS_TRUNCATED;
  }
  h->m = static_cast<uint32_t>(m);
  h->s1 = static_cast<uint32_t>(s1);
  h->s2 = static_cast<uint32_t>(s2);
  const size_t nb = static_cast<size_t>(nb64);
  h->mods.resize(h->m);
  h->blocks.assign(nb, BlockMeta{});
  h->total = 0;
  for (uint32_t i = 0; i < h->m; ++i) {
    uint64_t sz = 0, b = 0;
    if ((v = ReadVar(in, n, &pos, &sz)) != Var::kOk ||
        (v = ReadVar(in, n, &pos, &b)) != Var::kOk) {
      return VarStatus(v);
    }
    if (b >= nb || sz > UINT32_MAX) return ARIXV_STATUS_BAD_CONTAINER;
    h->mods[i] = {static_cast<uint32_t>(sz), static_cast<uint32_t>(b)};
    ++h->blocks[b].count;
    h->total += sz;  // <= 2^20 * 2^32
  }
  uint32_t acc = 0;
  for (size_t b = 0; b < nb; ++b) {
    if (h->blocks[b].count == 0) return ARIXV_STATUS_BAD_CONTAINER;  // empty
    h->blocks[b].first = acc;
    acc += h->blocks[b].count;
  }
  h->members.assign(h->m, UINT32_MAX);
  if (implicit) {
    for (size_t b = 0; b < nb; ++b) h->blocks[b].count = 0;
    for (uint32_t i = 0; i < h->m; ++i) {
      BlockMeta& blk = h->blocks[h->mods[i].block];
      h->members[blk.first + blk.count++] = i;
    }
  } else {
    for (uint32_t i = 0; i < h->m; ++i) {
      uint64_t order = 0;
      if ((v = ReadVar(in, n, &pos, &order)) != Var::kOk) return VarStatus(v);
      const BlockMeta& blk = h->blocks[h->mods[i].block];
      if (order >= blk.count) return ARIXV_STATUS_BAD_CONTAINER;
      uint32_t& slot = h->members[blk.first + order];
      if (slot != UINT32_MAX) return ARIXV_STATUS_BAD_CONTAINER;  // repeated
      slot = i;
    }
  }
  for (size_t b = 0; b < nb; ++b) {
    uint64_t lv = 0;
    if ((v = ReadVar(in, n, &pos, &lv)) != Var::kOk) return VarStatus(v);
    h->blocks[b].payload_len = lv >> 1;
    h->blocks[b].stored = static_cast<uint8_t>(lv & 1);
  }
  if (n - pos < 8) return ARIXV_STATUS_TRUNCATED;
  h->header_len = pos + 8;
  h->checksum = Fnv1a(in, pos);
#ifndef ARIXV_FUZZ_NO_PACK_CHECKSUM
  if (LoadLe64(in + pos) != h->checksum) return ARIXV_STATUS_BAD_CONTAINER;
#endif
  uint64_t off = h->header_len;
  for (size_t b = 0; b < nb; ++b) {
    BlockMeta& blk = h->blocks[b];
    if (blk.payload_len > n - off) return ARIXV_STATUS_TRUNCATED;
    if (blk.stored) {
      uint64_t sum = 0;
      for (uint32_t j = 0; j < blk.count; ++j) {
        sum += h->mods[h->members[blk.first + j]].size;
      }
      if (sum != blk.payload_len) return ARIXV_STATUS_BAD_CONTAINER;
    }
    blk.payload_off = off;
    off += blk.payload_len;
  }
  if (off != n) return ARIXV_STATUS_BAD_CONTAINER;  // trailing bytes
  // Library-derived scratch layout.
  uint64_t so = kRecordStride * uint64_t{h->s1};
  h->stage1_modules = 0;
  for (uint32_t b = 0; b < h->s1; ++b) {
    BlockMeta& blk = h->blocks[b];
    blk.slice_off = so;
    uint64_t slice = 0;
    for (uint32_t j = 0; j < blk.count; ++j) {
      slice += MemberBytes(h->mods[h->members[blk.first + j]].size);
    }
    h->stage1_modules += blk.count;
    so += Align64(slice);
  }
  h->scratch_size = h->s1 ? so + 63 : 0;
  return ARIXV_STATUS_OK;
}

uint8_t* AlignedBase(void* scratch) {
  const uintptr_t p = reinterpret_cast<uintptr_t>(scratch);
  return reinterpret_cast<uint8_t*>((p + 63) & ~uintptr_t{63});
}
const uint8_t* AlignedBase(const void* scratch) {
  return AlignedBase(const_cast<void*>(scratch));
}

// Input struct versioning, as arixv_decode_options: a v1 prefix at least,
// at most kMaxOptionsStructSize, and an all-zero unknown tail.
template <class T>
bool ValidInputStruct(const T* p, uint32_t v1_size) {
  if (p->struct_size < v1_size || p->struct_size > kMaxOptionsStructSize) {
    return false;
  }
  return p->struct_size <= sizeof(T) ||
         HasZeroTrailingBytes(p, sizeof(T), p->struct_size);
}

bool ValidOptions(const arixv_pack_decode_options* opts) {
  return !opts ||
         (ValidInputStruct(opts, ARIXV_PACK_DECODE_OPTIONS_V1_SIZE) &&
          opts->flags == 0);
}

// Step 1: header-independent arguments.
arixv_status CheckArgs(const void* in, const arixv_pack_outputs* out,
                       const void* scratch, size_t scratch_size,
                       const arixv_pack_decode_options* opts) {
  if (!in || !out || !ValidInputStruct(out, ARIXV_PACK_OUTPUTS_V1_SIZE) ||
      !out->outs || !out->caps || !ValidOptions(opts)) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  if (!scratch && scratch_size) return ARIXV_STATUS_INVALID_ARGUMENT;
  return ARIXV_STATUS_OK;
}

// Steps 3 and 4 for a set of blocks [b0, b1): header-dependent arguments,
// then buffers. `scratch_needed` is false for scratch-free calls.
arixv_status CheckBlocks(const Header& h, uint32_t b0, uint32_t b1,
                         const arixv_pack_outputs* out, const void* scratch,
                         size_t scratch_size, bool scratch_needed) {
  if (out->module_count != h.m) return ARIXV_STATUS_INVALID_ARGUMENT;
  if (scratch_needed && h.scratch_size) {
    if (h.scratch_size > kMaxSpan) return ARIXV_STATUS_TOO_LARGE;
    if (!scratch) return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  for (uint32_t b = b0; b < b1; ++b) {
    const BlockMeta& blk = h.blocks[b];
    for (uint32_t j = 0; j < blk.count; ++j) {
      if (uint64_t{h.mods[h.members[blk.first + j]].size} > kMaxSpan) {
        return ARIXV_STATUS_TOO_LARGE;
      }
    }
  }
  // Step 4.
  if (scratch_needed && h.scratch_size && scratch_size < h.scratch_size) {
    return ARIXV_STATUS_BUFFER_TOO_SMALL;
  }
  for (uint32_t b = b0; b < b1; ++b) {
    const BlockMeta& blk = h.blocks[b];
    for (uint32_t j = 0; j < blk.count; ++j) {
      const uint32_t mod = h.members[blk.first + j];
      if (!out->outs[mod] && h.mods[mod].size > 0) {
        return ARIXV_STATUS_INVALID_ARGUMENT;
      }
    }
  }
  for (uint32_t b = b0; b < b1; ++b) {
    const BlockMeta& blk = h.blocks[b];
    for (uint32_t j = 0; j < blk.count; ++j) {
      const uint32_t mod = h.members[blk.first + j];
      if (out->caps[mod] < h.mods[mod].size) {
        return ARIXV_STATUS_BUFFER_TOO_SMALL;
      }
    }
  }
  return ARIXV_STATUS_OK;
}

// Decodes global block b (coded against `shared`, or stored) into its
// members in coding order.
arixv_status DecodeOne(const uint8_t* in, const Header& h, uint32_t b,
                       const SharedMember* shared, size_t ns,
                       std::vector<DecodedMember>* mods) {
  const BlockMeta& blk = h.blocks[b];
  if (blk.stored) {
    mods->clear();
    mods->resize(blk.count);
    const uint8_t* p = in + blk.payload_off;
    for (uint32_t j = 0; j < blk.count; ++j) {
      const size_t n = h.mods[h.members[blk.first + j]].size;
      (*mods)[j].bytes.assign(p, p + n);
      p += n;
    }
    return ARIXV_STATUS_OK;
  }
  std::vector<uint64_t> sizes(blk.count);
  for (uint32_t j = 0; j < blk.count; ++j) {
    sizes[j] = h.mods[h.members[blk.first + j]].size;
  }
  return DecodeBlock(in + blk.payload_off,
                     static_cast<size_t>(blk.payload_len), h.codec,
                     sizes.data(), blk.count, shared, ns, mods);
}

void WriteOutputs(const Header& h, uint32_t b, const arixv_pack_outputs* out,
                  const DecodedMember* mods) {
  const BlockMeta& blk = h.blocks[b];
  for (uint32_t j = 0; j < blk.count; ++j) {
    const uint32_t mod = h.members[blk.first + j];
    if (mods[j].size()) {
      std::memcpy(out->outs[mod], mods[j].data(), mods[j].size());
    }
  }
}

SharedMember ViewOf(const DecodedMember& d) {
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
  return m;
}

arixv_status Stage1(const uint8_t* in, const Header& h, uint32_t block_index,
                    const arixv_pack_outputs* outputs, void* scratch) {
  uint8_t* base = AlignedBase(scratch);
  uint8_t* rec = base + kRecordStride * block_index;
  std::memset(rec, 0, sizeof(Record));  // cleared before any slice write
  std::vector<DecodedMember> mods;
  const arixv_status st = DecodeOne(in, h, block_index, nullptr, 0, &mods);
  if (st != ARIXV_STATUS_OK) return st;
  WriteOutputs(h, block_index, outputs, mods.data());
  // Slice: members in coding order, at offsets derived from sizes only.
  const BlockMeta& blk = h.blocks[block_index];
  uint8_t* p = base + blk.slice_off;
  for (uint32_t j = 0; j < blk.count; ++j) {
    const DecodedMember& dm = mods[j];
    const uint64_t size = dm.size();
    const uint64_t w = size / 4;
    MemberDesc d{dm.valid ? 1u : 0u, 0, size};
    uint8_t* bytes = p + sizeof(MemberDesc);
    uint8_t* mask = bytes + Pad8(size);
    uint8_t* offs = mask + Pad8(w);
    if (dm.valid) {
      if (dm.starts.size() > w + 1 || dm.mask.size() > w ||
          dm.words.size() * 4 != size) {
        return ARIXV_STATUS_BAD_CONTAINER;
      }
      d.nframes = static_cast<uint32_t>(dm.starts.size() - 1);
      std::memcpy(mask, dm.mask.data(), dm.mask.size());
      std::memcpy(offs, dm.starts.data(), dm.starts.size() * 4);
    }
    if (size) std::memcpy(bytes, dm.data(), static_cast<size_t>(size));
    std::memcpy(p, &d, sizeof d);
    p += MemberBytes(size);
  }
  const Record r{kRecordMagic, h.checksum, block_index, 0};
  std::memcpy(rec, &r, sizeof r);  // published last
  return ARIXV_STATUS_OK;
}

arixv_status Stage2(const uint8_t* in, const Header& h, uint32_t block_index,
                    const arixv_pack_outputs* outputs, const void* scratch) {
  const uint32_t b = h.s1 + block_index;
  // Every stage-1 record must be published for this pack.
  const uint8_t* base = h.s1 ? AlignedBase(scratch) : nullptr;
  for (uint32_t i = 0; i < h.s1; ++i) {
    const Record r = Load<Record>(base + kRecordStride * i);
    if (r.magic != kRecordMagic || r.checksum != h.checksum || r.index != i) {
      return ARIXV_STATUS_DEPENDENCY_INCOMPLETE;
    }
  }
  std::vector<DecodedMember> mods;
  if (h.blocks[b].stored) {
    const arixv_status st = DecodeOne(in, h, b, nullptr, 0, &mods);
    if (st != ARIXV_STATUS_OK) return st;
    WriteOutputs(h, b, outputs, mods.data());
    return ARIXV_STATUS_OK;
  }
  // In-place views of the stage-1 slices (shared index order), with a
  // sanity check of each descriptor against the validated header.
  std::vector<SharedMember> view(static_cast<size_t>(h.stage1_modules));
  size_t vi = 0;
  for (uint32_t i = 0; i < h.s1; ++i) {
    const BlockMeta& blk = h.blocks[i];
    const uint8_t* p = base + blk.slice_off;
    for (uint32_t j = 0; j < blk.count; ++j, ++vi) {
      const uint64_t size = h.mods[h.members[blk.first + j]].size;
      const MemberDesc d = Load<MemberDesc>(p);
      const uint64_t w = size / 4;
      if (d.size != size) return ARIXV_STATUS_DEPENDENCY_INCOMPLETE;
      if (d.valid && (size < 20 || size % 4 != 0 || d.nframes == 0 ||
                      d.nframes > (size - 20) / 4)) {
        return ARIXV_STATUS_DEPENDENCY_INCOMPLETE;
      }
      SharedMember& m = view[vi];
      m.bytes = p + sizeof(MemberDesc);
      m.size = static_cast<size_t>(size);
      m.valid = d.valid != 0;
      if (m.valid) {
        m.words = reinterpret_cast<const uint32_t*>(m.bytes);
        m.nwords = static_cast<size_t>(w);
        m.mask = m.bytes + Pad8(size);
        m.starts = reinterpret_cast<const uint32_t*>(m.mask + Pad8(w));
        m.nframes = d.nframes;
      }
      p += MemberBytes(size);
    }
  }
  const arixv_status st = DecodeOne(in, h, b, view.data(), view.size(), &mods);
  if (st != ARIXV_STATUS_OK) return st;
  WriteOutputs(h, b, outputs, mods.data());
  return ARIXV_STATUS_OK;
}

ARIXV_NOINLINE arixv_status InspectImpl(const void* in, size_t in_size,
                                        arixv_pack_info* info) {
  if (!in || !info || info->struct_size < ARIXV_PACK_INFO_V1_SIZE ||
      info->struct_size > kMaxOptionsStructSize) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  Header h;
  const arixv_status st = Parse(in, in_size, &h);
  if (st != ARIXV_STATUS_OK) return st;
  const uint32_t caller_size = info->struct_size;
  info->magic = kInfoMagic;
  info->flags = (h.codec & kCodecFastDecode) ? ARIXV_PACK_INFO_FAST_DECODE : 0;
  info->module_count = h.m;
  info->stage1_block_count = h.s1;
  info->stage2_block_count = h.s2;
  info->total_output_size = h.total;
  info->scratch_size = h.scratch_size;
  if (caller_size > sizeof(arixv_pack_info)) {
    std::memset(reinterpret_cast<uint8_t*>(info) + sizeof(arixv_pack_info), 0,
                caller_size - sizeof(arixv_pack_info));
  }
  return ARIXV_STATUS_OK;
}

ARIXV_NOINLINE arixv_status InspectModulesImpl(const void* in, size_t in_size,
                                               arixv_pack_module_info* modules,
                                               uint32_t count) {
  if (!in || !modules) return ARIXV_STATUS_INVALID_ARGUMENT;
  Header h;
  const arixv_status st = Parse(in, in_size, &h);
  if (st != ARIXV_STATUS_OK) return st;
  if (count != h.m) return ARIXV_STATUS_INVALID_ARGUMENT;
  for (uint32_t i = 0; i < h.m; ++i) {
    const ModMeta& mm = h.mods[i];
    const bool s1 = mm.block < h.s1;
    modules[i].size = mm.size;
    modules[i].block_index = s1 ? mm.block : mm.block - h.s1;
    modules[i].stage = s1 ? ARIXV_PACK_STAGE_1 : ARIXV_PACK_STAGE_2;
    modules[i].reserved = 0;
    modules[i].flags = 0;
  }
  return ARIXV_STATUS_OK;
}

ARIXV_NOINLINE arixv_status ResetScratchImpl(const void* in, size_t in_size,
                                             void* scratch,
                                             size_t scratch_size) {
  if (!in || (!scratch && scratch_size)) return ARIXV_STATUS_INVALID_ARGUMENT;
  Header h;
  const arixv_status st = Parse(in, in_size, &h);
  if (st != ARIXV_STATUS_OK) return st;
  if (h.scratch_size == 0) return ARIXV_STATUS_OK;
  if (h.scratch_size > kMaxSpan) return ARIXV_STATUS_TOO_LARGE;
  if (!scratch) return ARIXV_STATUS_INVALID_ARGUMENT;
  if (scratch_size < h.scratch_size) return ARIXV_STATUS_BUFFER_TOO_SMALL;
  uint8_t* base = AlignedBase(scratch);
  for (uint32_t b = 0; b < h.s1; ++b) {
    std::memset(base + kRecordStride * b, 0, sizeof(Record));
  }
  return ARIXV_STATUS_OK;
}

ARIXV_NOINLINE arixv_status Stage1Impl(const void* in, size_t in_size,
                                       uint32_t block_index,
                                       const arixv_pack_outputs* outputs,
                                       void* scratch, size_t scratch_size,
                                       const arixv_pack_decode_options* opts) {
  arixv_status st = CheckArgs(in, outputs, scratch, scratch_size, opts);
  if (st != ARIXV_STATUS_OK) return st;
  Header h;
  if ((st = Parse(in, in_size, &h)) != ARIXV_STATUS_OK) return st;
  if (outputs->module_count != h.m || block_index >= h.s1) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  st = CheckBlocks(h, block_index, block_index + 1, outputs, scratch,
                   scratch_size, true);
  if (st != ARIXV_STATUS_OK) return st;
  return Stage1(static_cast<const uint8_t*>(in), h, block_index, outputs,
                scratch);
}

ARIXV_NOINLINE arixv_status Stage2Impl(const void* in, size_t in_size,
                                       uint32_t block_index,
                                       const arixv_pack_outputs* outputs,
                                       const void* scratch, size_t scratch_size,
                                       const arixv_pack_decode_options* opts) {
  arixv_status st = CheckArgs(in, outputs, scratch, scratch_size, opts);
  if (st != ARIXV_STATUS_OK) return st;
  Header h;
  if ((st = Parse(in, in_size, &h)) != ARIXV_STATUS_OK) return st;
  if (outputs->module_count != h.m || block_index >= h.s2) {
    return ARIXV_STATUS_INVALID_ARGUMENT;
  }
  const uint32_t b = h.s1 + block_index;
  st = CheckBlocks(h, b, b + 1, outputs, scratch, scratch_size, true);
  if (st != ARIXV_STATUS_OK) return st;
  return Stage2(static_cast<const uint8_t*>(in), h, block_index, outputs,
                scratch);
}

// Single-thread decode: stage-1 members stay in memory and stage 2 reads
// them directly, so no scratch buffer is built.
ARIXV_NOINLINE arixv_status DecodeImpl(const void* in, size_t in_size,
                                       const arixv_pack_outputs* outputs,
                                       const arixv_pack_decode_options* opts) {
  arixv_status st = CheckArgs(in, outputs, nullptr, 0, opts);
  if (st != ARIXV_STATUS_OK) return st;
  Header h;
  if ((st = Parse(in, in_size, &h)) != ARIXV_STATUS_OK) return st;
  // Steps 3-4 for every block before any write.
  st = CheckBlocks(h, 0, h.s1 + h.s2, outputs, nullptr, 0, false);
  if (st != ARIXV_STATUS_OK) return st;
  const uint8_t* p = static_cast<const uint8_t*>(in);
  std::vector<DecodedMember> stage1;
  stage1.reserve(static_cast<size_t>(h.stage1_modules));
  std::vector<SharedMember> view;
  view.reserve(static_cast<size_t>(h.stage1_modules));
  std::vector<DecodedMember> mods;
  for (uint32_t b = 0; b < h.s1; ++b) {
    if ((st = DecodeOne(p, h, b, nullptr, 0, &mods)) != ARIXV_STATUS_OK) {
      return st;
    }
    WriteOutputs(h, b, outputs, mods.data());
    for (DecodedMember& d : mods) {
      stage1.push_back(std::move(d));
      view.push_back(ViewOf(stage1.back()));
    }
  }
  for (uint32_t j = 0; j < h.s2; ++j) {
    const uint32_t b = h.s1 + j;
    st = h.blocks[b].stored
             ? DecodeOne(p, h, b, nullptr, 0, &mods)
             : DecodeOne(p, h, b, view.data(), view.size(), &mods);
    if (st != ARIXV_STATUS_OK) return st;
    WriteOutputs(h, b, outputs, mods.data());
  }
  return ARIXV_STATUS_OK;
}

template <class F>
arixv_status Guard(F&& f) {
#if ARIXV_HAS_EXCEPTIONS
  try {
    return f();
  } catch (...) {
    return ARIXV_STATUS_OUT_OF_MEMORY;
  }
#else
  return f();
#endif
}

}  // namespace

size_t HeaderBytes(const void* in, size_t in_size) {
  Header h;
  if (!in || Parse(in, in_size, &h) != ARIXV_STATUS_OK) return 0;
  return h.header_len;
}

}  // namespace pack
}  // namespace v1
}  // namespace arixv

extern "C" {

using arixv::v1::pack::Guard;

arixv_status ARIXV_CALL arixv_pack_inspect(
    const void* in, size_t in_size, arixv_pack_info* out_info) ARIXV_NOEXCEPT {
  return Guard(
      [&] { return arixv::v1::pack::InspectImpl(in, in_size, out_info); });
}

arixv_status ARIXV_CALL arixv_pack_inspect_modules(
    const void* in, size_t in_size, arixv_pack_module_info* out_modules,
    uint32_t count) ARIXV_NOEXCEPT {
  return Guard([&] {
    return arixv::v1::pack::InspectModulesImpl(in, in_size, out_modules,
                                               count);
  });
}

arixv_status ARIXV_CALL arixv_pack_reset_scratch(
    const void* in, size_t in_size, void* scratch,
    size_t scratch_size) ARIXV_NOEXCEPT {
  return Guard([&] {
    return arixv::v1::pack::ResetScratchImpl(in, in_size, scratch,
                                             scratch_size);
  });
}

arixv_status ARIXV_CALL arixv_pack_decode_stage1(
    const void* in, size_t in_size, uint32_t block_index,
    const arixv_pack_outputs* outputs, void* scratch, size_t scratch_size,
    const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT {
  return Guard([&] {
    return arixv::v1::pack::Stage1Impl(in, in_size, block_index, outputs,
                                       scratch, scratch_size, opts);
  });
}

arixv_status ARIXV_CALL arixv_pack_decode_stage2(
    const void* in, size_t in_size, uint32_t block_index,
    const arixv_pack_outputs* outputs, const void* scratch,
    size_t scratch_size, const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT {
  return Guard([&] {
    return arixv::v1::pack::Stage2Impl(in, in_size, block_index, outputs,
                                       scratch, scratch_size, opts);
  });
}

arixv_status ARIXV_CALL arixv_pack_decode(
    const void* in, size_t in_size, const arixv_pack_outputs* outputs,
    const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT {
  return Guard(
      [&] { return arixv::v1::pack::DecodeImpl(in, in_size, outputs, opts); });
}

}  // extern "C"
