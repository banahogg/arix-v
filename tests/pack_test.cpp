// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// ARXP shader-pack tests: the public pack C API (two-stage and convenience
// decode, encode, inspection), the documented check order and scratch
// rules, hostile input, concurrency, encoder determinism, and the ARXP v1
// golden vectors in tests/golden/pack (docs/format-arxp.md).
//
//   pack_test --golden-dir tests/golden [--fuzz-iters N]
//   pack_test --golden-dir tests/golden --write-pack-goldens

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
#include "pack.h"
#include "test_util.h"

namespace pk = arixv::pack;

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Synthetic modules.
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
// import, memory model, entry point, decorations, types, variables, and a
// function with a block.
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

bool Renumberable(const Bytes& m) {
  std::vector<pk::FrameSpan> f;
  std::vector<uint8_t> mask;
  return m.size() >= 20 && m[0] == 0x03 && m[1] == 0x02 && m[2] == 0x23 &&
         m[3] == 0x07 && pk::ModuleIdRoles(m.data(), m.size(), &f, &mask);
}

uint32_t Word(const Bytes& m, size_t at) {
  return m[at] | (m[at + 1] << 8) | (m[at + 2] << 16) |
         (static_cast<uint32_t>(m[at + 3]) << 24);
}

// Adds `delta` to every ID word (model roles) and to the bound: a variant
// the block coder can copy from its original.
Bytes Renumber(const Bytes& m, uint32_t delta) {
  std::vector<pk::FrameSpan> f;
  std::vector<uint8_t> mask;
  CHECK(pk::ModuleIdRoles(m.data(), m.size(), &f, &mask));
  Bytes out = m;
  auto put = [&](size_t at, uint32_t w) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(w >> (8 * i));
  };
  put(12, Word(m, 12) + delta);
  for (size_t i = 0; i < mask.size(); ++i) {
    if (mask[i]) put(20 + 4 * i, Word(m, 20 + 4 * i) + delta);
  }
  return out;
}

bool ReadFile(const std::filesystem::path& p, Bytes* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

Bytes Arx2Decode(const Bytes& blob) {
  size_t n = 0;
  CHECK(arixv_decode(blob.data(), blob.size(), nullptr, &n, nullptr) ==
        ARIXV_STATUS_OK);
  Bytes out(n);
  CHECK(arixv_decode(blob.data(), blob.size(), out.data(), &n, nullptr) ==
        ARIXV_STATUS_OK);
  return out;
}

// Every committed ARX2 golden vector, decoded, in file-name order.
std::vector<Bytes> AllGoldenArx2(const std::string& dir) {
  std::vector<Bytes> out;
  if (dir.empty()) return out;
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() == ".arx2") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    Bytes blob;
    CHECK(ReadFile(f, &blob));
    out.push_back(Arx2Decode(blob));
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
// Pack helpers.
// ---------------------------------------------------------------------------
std::vector<pk::ModuleView> Views(const std::vector<const Bytes*>& ms) {
  std::vector<pk::ModuleView> v;
  for (const Bytes* m : ms) v.push_back({m->data(), m->size()});
  return v;
}

// Internal encode (test knobs); deterministic. When the parameters are
// expressible through the public API, it must produce the same bytes.
Bytes MustEncode(const std::vector<const Bytes*>& ms,
                 const pk::EncodeParams& p, pk::EncodeStats* stats = nullptr) {
  const auto v = Views(ms);
  Bytes out, again;
  CHECK(pk::Encode(v.data(), v.size(), p, &out, stats) == ARIXV_STATUS_OK);
  CHECK(pk::Encode(v.data(), v.size(), p, &again, nullptr) == ARIXV_STATUS_OK);
  CHECK(again == out);
  if ((p.codec_flags & pk::kCodecWarm) && !p.reverse_coding_order) {
    std::vector<const void*> ptrs;
    std::vector<size_t> sizes;
    for (const Bytes* m : ms) {
      ptrs.push_back(m->empty() ? nullptr : m->data());
      sizes.push_back(m->size());
    }
    arixv_pack_encode_options o = {sizeof o, 0, p.parallelism,
                                   p.latency_budget_pct, p.max_block_bytes};
    if (p.codec_flags & pk::kCodecFastDecode) {
      o.flags = ARIXV_PACK_ENCODE_FAST_DECODE;
    }
    const uint32_t n = static_cast<uint32_t>(ms.size());
    size_t cap = arixv_pack_encode_bound(sizes.data(), n, &o);
    CHECK(cap >= out.size());
    Bytes pub(cap);
    CHECK(arixv_pack_encode(ptrs.data(), sizes.data(), n, pub.data(), &cap,
                            &o) == ARIXV_STATUS_OK);
    pub.resize(cap);
    CHECK(pub == out);
  }
  return out;
}

pk::EncodeParams Par(uint32_t parallelism, uint32_t budget = 0) {
  pk::EncodeParams p;
  p.parallelism = parallelism;
  p.latency_budget_pct = budget;
  return p;
}

arixv_pack_info Inspect(const Bytes& pack) {
  arixv_pack_info info = {};
  info.struct_size = sizeof info;
  CHECK(arixv_pack_inspect(pack.data(), pack.size(), &info) == ARIXV_STATUS_OK);
  return info;
}

std::vector<arixv_pack_module_info> Modules(const Bytes& pack, uint32_t n) {
  std::vector<arixv_pack_module_info> mi(n);
  CHECK(arixv_pack_inspect_modules(pack.data(), pack.size(), mi.data(), n) ==
        ARIXV_STATUS_OK);
  return mi;
}

struct Bufs {
  std::vector<Bytes> data;
  std::vector<void*> ptrs;
  std::vector<size_t> caps;
  arixv_pack_outputs view{};
};

Bufs MakeBufs(const Bytes& pack, uint8_t fill = 0xAB) {
  Bufs b;
  const arixv_pack_info info = Inspect(pack);
  const auto mi = Modules(pack, info.module_count);
  uint64_t sum = 0;
  for (const auto& m : mi) sum += m.size;
  CHECK(sum == info.total_output_size);
  b.data.resize(info.module_count);
  for (uint32_t i = 0; i < info.module_count; ++i) {
    b.data[i].assign(static_cast<size_t>(mi[i].size), fill);
    b.ptrs.push_back(b.data[i].data());
    b.caps.push_back(b.data[i].size());
  }
  b.view = {sizeof(arixv_pack_outputs), info.module_count, b.ptrs.data(),
            b.caps.data()};
  return b;
}

arixv_status Stage1(const Bytes& p, uint32_t i, const arixv_pack_outputs* o,
                    void* s, size_t ss) {
  return arixv_pack_decode_stage1(p.data(), p.size(), i, o, s, ss, nullptr);
}
arixv_status Stage2(const Bytes& p, uint32_t i, const arixv_pack_outputs* o,
                    const void* s, size_t ss) {
  return arixv_pack_decode_stage2(p.data(), p.size(), i, o, s, ss, nullptr);
}

// Serial convenience path and the explicit two-stage path (stage 2 in
// reverse order, misaligned dirty scratch reused across passes) must both
// reproduce every module.
void MustRoundTrip(const std::vector<const Bytes*>& ms, const Bytes& pack) {
  {
    Bufs b = MakeBufs(pack);
    CHECK(arixv_pack_decode(pack.data(), pack.size(), &b.view, nullptr) ==
          ARIXV_STATUS_OK);
    for (size_t i = 0; i < ms.size(); ++i) CHECK(b.data[i] == *ms[i]);
  }
  const arixv_pack_info info = Inspect(pack);
  CHECK(info.module_count == ms.size());
  CHECK((info.scratch_size == 0) == (info.stage1_block_count == 0));
  const size_t ss = static_cast<size_t>(info.scratch_size);
  Bytes scratch(ss + 5, 0xCD);
  void* sp = ss ? scratch.data() + 5 : nullptr;  // misaligned
  for (int pass = 0; pass < 2; ++pass) {
    Bufs b = MakeBufs(pack);
    CHECK(arixv_pack_reset_scratch(pack.data(), pack.size(), sp, ss) ==
          ARIXV_STATUS_OK);
    for (uint32_t i = 0; i < info.stage1_block_count; ++i) {
      CHECK(Stage1(pack, i, &b.view, sp, ss) == ARIXV_STATUS_OK);
    }
    for (uint32_t j = info.stage2_block_count; j-- > 0;) {
      CHECK(Stage2(pack, j, &b.view, sp, ss) == ARIXV_STATUS_OK);
    }
    for (size_t i = 0; i < ms.size(); ++i) CHECK(b.data[i] == *ms[i]);
  }
  // The frozen ARX2 decoder recognizes the family but not packs.
  size_t n = 0;
  CHECK(arixv_decode(pack.data(), pack.size(), nullptr, &n, nullptr) ==
        ARIXV_STATUS_UNSUPPORTED_FORMAT);
}

void Resign(Bytes& t, size_t header_len) {
  const uint64_t h = pk::Fnv1a(t.data(), header_len - 8);
  for (int i = 0; i < 8; ++i) {
    t[header_len - 8 + i] = static_cast<uint8_t>(h >> (8 * i));
  }
}

// Runs every public decode entry point on (possibly hostile) bytes.
void DecodeAnything(const Bytes& t, size_t max_bytes) {
  arixv_pack_info ti = {};
  ti.struct_size = sizeof ti;
  if (arixv_pack_inspect(t.data(), t.size(), &ti) != ARIXV_STATUS_OK) return;
  if (ti.total_output_size > max_bytes || ti.scratch_size > 4 * max_bytes) {
    return;
  }
  Bufs b = MakeBufs(t);
  (void)arixv_pack_decode(t.data(), t.size(), &b.view, nullptr);
  Bytes sc(static_cast<size_t>(ti.scratch_size));
  (void)arixv_pack_reset_scratch(t.data(), t.size(), sc.data(), sc.size());
  for (uint32_t i = 0; i < ti.stage1_block_count; ++i) {
    (void)Stage1(t, i, &b.view, sc.data(), sc.size());
  }
  for (uint32_t j = 0; j < ti.stage2_block_count; ++j) {
    (void)Stage2(t, j, &b.view, sc.data(), sc.size());
  }
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------
std::vector<Bytes> ApiModules(const std::vector<Bytes>& fx) {
  std::vector<Bytes> mods;
  for (const Bytes& m : fx) mods.push_back(m);
  for (uint32_t d : {3u, 7u, 11u}) {
    for (const Bytes& m : fx) {
      if (Renumberable(m)) mods.push_back(Renumber(m, d));
    }
  }
  mods.push_back(mods[0]);  // exact duplicate
  mods.push_back({1, 2, 3});
  mods.push_back({});
  return mods;
}

void TestLayouts(const std::vector<Bytes>& fx, Bytes* two_stage_out) {
  const std::vector<Bytes> mods = ApiModules(fx);
  std::vector<const Bytes*> ms;
  for (const Bytes& m : mods) ms.push_back(&m);
  Bytes two_stage;
  for (uint32_t par : {1u, 2u, 4u, 8u, 0u}) {
    for (uint64_t cap : {uint64_t{0}, uint64_t{4096}}) {
      pk::EncodeParams p = Par(par);
      p.max_block_bytes = cap;
      pk::EncodeStats st;
      const Bytes pack = MustEncode(ms, p, &st);
      if (par == 1 && cap == 0) {
        CHECK(st.stage1_blocks == 0 && st.stage2_blocks == 1);
      }
      if (par >= 4) CHECK(st.stage1_blocks > 0);
      MustRoundTrip(ms, pack);
      if (st.stage1_blocks > 0 && two_stage.empty()) two_stage = pack;
    }
  }
  CHECK(!two_stage.empty());
  *two_stage_out = two_stage;
}

void TestInspection(const Bytes& pk_bytes) {
  const Bytes& p = pk_bytes;
  arixv_pack_info info = Inspect(p);
  CHECK(info.magic == 0x50585241u && info.flags == 0);
  // Versioned struct: v1 size, larger with a zero-filled tail, too small or
  // too large rejected and left untouched.
  struct Big {
    arixv_pack_info i;
    uint8_t tail[24];
  } big;
  std::memset(&big, 0xEE, sizeof big);
  big.i.struct_size = sizeof big;
  CHECK(arixv_pack_inspect(p.data(), p.size(), &big.i) == ARIXV_STATUS_OK);
  for (uint8_t c : big.tail) CHECK(c == 0);
  CHECK(big.i.module_count == info.module_count);
  arixv_pack_info bad;
  std::memset(&bad, 0xEE, sizeof bad);
  bad.struct_size = ARIXV_PACK_INFO_V1_SIZE - 1;
  CHECK(arixv_pack_inspect(p.data(), p.size(), &bad) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  CHECK(bad.module_count == 0xEEEEEEEEu);
  bad.struct_size = 4097;
  CHECK(arixv_pack_inspect(p.data(), p.size(), &bad) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  CHECK(arixv_pack_inspect(nullptr, 0, &info) == ARIXV_STATUS_INVALID_ARGUMENT);
  CHECK(arixv_pack_inspect(p.data(), p.size(), nullptr) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  // Untouched on a header error.
  arixv_pack_info keep;
  std::memset(&keep, 0xEE, sizeof keep);
  keep.struct_size = sizeof keep;
  CHECK(arixv_pack_inspect(p.data(), p.size() - 1, &keep) != ARIXV_STATUS_OK);
  CHECK(keep.module_count == 0xEEEEEEEEu);
  // Module table: stage, index within stage, sizes summing to the total.
  const auto mi = Modules(p, info.module_count);
  uint32_t s1_seen = 0, s2_seen = 0;
  for (const auto& m : mi) {
    CHECK(m.stage == ARIXV_PACK_STAGE_1 || m.stage == ARIXV_PACK_STAGE_2);
    CHECK(m.reserved == 0 && m.flags == 0);
    if (m.stage == ARIXV_PACK_STAGE_1) {
      CHECK(m.block_index < info.stage1_block_count);
      s1_seen |= 1u << (m.block_index % 32);
    } else {
      CHECK(m.block_index < info.stage2_block_count);
      s2_seen |= 1u << (m.block_index % 32);
    }
  }
  CHECK(s1_seen != 0 && s2_seen != 0);
  std::vector<arixv_pack_module_info> wrong(info.module_count + 1);
  CHECK(arixv_pack_inspect_modules(p.data(), p.size(), wrong.data(),
                                   info.module_count + 1) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  CHECK(arixv_pack_inspect_modules(p.data(), p.size(), nullptr,
                                   info.module_count) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
}

void TestDecodeContract(const std::vector<Bytes>& fx, const Bytes& pkb) {
  const Bytes& p = pkb;
  const arixv_pack_info info = Inspect(p);
  const size_t ss = static_cast<size_t>(info.scratch_size);
  Bytes scratch(ss);
  // Stage 2 before stage 1 (fresh, reset scratch): dependency error.
  {
    Bufs b = MakeBufs(p);
    CHECK(arixv_pack_reset_scratch(p.data(), p.size(), scratch.data(), ss) ==
          ARIXV_STATUS_OK);
    CHECK(Stage2(p, 0, &b.view, scratch.data(), ss) ==
          ARIXV_STATUS_DEPENDENCY_INCOMPLETE);
  }
  // Argument and capacity errors; outputs untouched before step 6.
  {
    Bufs b = MakeBufs(p, 0x5A);
    CHECK(Stage1(p, 0, &b.view, scratch.data(), ss - 1) ==
          ARIXV_STATUS_BUFFER_TOO_SMALL);
    CHECK(Stage1(p, 0, &b.view, nullptr, 0) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(Stage1(p, 0, &b.view, nullptr, 8) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(Stage1(p, info.stage1_block_count, &b.view, scratch.data(), ss) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(Stage2(p, info.stage2_block_count, &b.view, scratch.data(), ss) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    // Header-independent errors win over header errors (step 1 first).
    CHECK(arixv_pack_decode_stage1(p.data(), 3, 0, nullptr, scratch.data(), ss,
                                   nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    arixv_pack_outputs wrong = b.view;
    wrong.module_count -= 1;
    CHECK(Stage1(p, 0, &wrong, scratch.data(), ss) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    arixv_pack_outputs small_struct = b.view;
    small_struct.struct_size = ARIXV_PACK_OUTPUTS_V1_SIZE - 1;
    CHECK(Stage1(p, 0, &small_struct, scratch.data(), ss) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    arixv_pack_outputs no_caps = b.view;
    no_caps.caps = nullptr;
    CHECK(arixv_pack_decode(p.data(), p.size(), &no_caps, nullptr) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    arixv_pack_decode_options opts = {sizeof opts, 1};
    CHECK(arixv_pack_decode(p.data(), p.size(), &b.view, &opts) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    opts = {ARIXV_PACK_DECODE_OPTIONS_V1_SIZE - 1, 0};
    CHECK(arixv_pack_decode(p.data(), p.size(), &b.view, &opts) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    // Larger options with a zero tail are accepted; a nonzero tail is not.
    struct BigOpts {
      arixv_pack_decode_options o;
      uint32_t tail[2];
    } bo = {{sizeof(BigOpts), 0}, {0, 0}};
    std::vector<size_t> small = b.caps;
    for (auto& c : small) c = c ? c - 1 : 0;
    arixv_pack_outputs tight = {sizeof tight, b.view.module_count,
                                b.ptrs.data(), small.data()};
    CHECK(arixv_pack_decode(p.data(), p.size(), &tight, &bo.o) ==
          ARIXV_STATUS_BUFFER_TOO_SMALL);
    bo.tail[1] = 1;
    CHECK(arixv_pack_decode(p.data(), p.size(), &b.view, &bo.o) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    // A NULL own output with nonzero size outranks a short capacity.
    std::vector<void*> nulls = b.ptrs;
    for (auto& q : nulls) q = nullptr;
    arixv_pack_outputs null_out = {sizeof null_out, b.view.module_count,
                                   nulls.data(), small.data()};
    CHECK(arixv_pack_decode(p.data(), p.size(), &null_out, nullptr) ==
          ARIXV_STATUS_INVALID_ARGUMENT);
    for (const Bytes& d : b.data) {
      for (uint8_t c : d) CHECK(c == 0x5A);
    }
  }
  // Scratch published for one pack is rejected by another pack's stage 2.
  {
    std::vector<Bytes> mods = ApiModules(fx);
    std::vector<const Bytes*> other;
    for (size_t i = 0; i < mods.size() / 2; ++i) other.push_back(&mods[i]);
    const Bytes op = MustEncode(other, Par(8));
    const arixv_pack_info oi = Inspect(op);
    Bufs b = MakeBufs(p);
    CHECK(arixv_pack_reset_scratch(p.data(), p.size(), scratch.data(), ss) ==
          ARIXV_STATUS_OK);
    for (uint32_t i = 0; i < info.stage1_block_count; ++i) {
      CHECK(Stage1(p, i, &b.view, scratch.data(), ss) == ARIXV_STATUS_OK);
    }
    if (oi.stage1_block_count && oi.scratch_size <= ss) {
      Bufs ob = MakeBufs(op);
      CHECK(Stage2(op, 0, &ob.view, scratch.data(), ss) ==
            ARIXV_STATUS_DEPENDENCY_INCOMPLETE);
    }
  }
  // Concurrent stages on one scratch buffer.
  {
    std::vector<Bytes> mods = ApiModules(fx);
    Bufs b = MakeBufs(p);
    CHECK(arixv_pack_reset_scratch(p.data(), p.size(), scratch.data(), ss) ==
          ARIXV_STATUS_OK);
    std::atomic<int> bad{0};
    auto run = [&](uint32_t n, bool stage1) {
      std::vector<std::thread> th;
      std::atomic<uint32_t> next{0};
      for (int t = 0; t < 4; ++t) {
        th.emplace_back([&] {
          for (uint32_t i; (i = next++) < n;) {
            const arixv_status r =
                stage1 ? Stage1(p, i, &b.view, scratch.data(), ss)
                       : Stage2(p, i, &b.view, scratch.data(), ss);
            if (r != ARIXV_STATUS_OK) ++bad;
          }
        });
      }
      for (auto& x : th) x.join();
    };
    run(info.stage1_block_count, true);
    run(info.stage2_block_count, false);
    CHECK(bad == 0);
    for (size_t i = 0; i < mods.size(); ++i) CHECK(b.data[i] == mods[i]);
  }
  // Jobs see only their own output slots (others NULL, capacity 0).
  {
    std::vector<Bytes> mods = ApiModules(fx);
    const auto mi = Modules(p, info.module_count);
    std::vector<Bytes> data(mi.size());
    for (size_t i = 0; i < mi.size(); ++i) {
      data[i].resize(static_cast<size_t>(mi[i].size));
    }
    CHECK(arixv_pack_reset_scratch(p.data(), p.size(), scratch.data(), ss) ==
          ARIXV_STATUS_OK);
    for (uint8_t stage : {uint8_t{ARIXV_PACK_STAGE_1},
                          uint8_t{ARIXV_PACK_STAGE_2}}) {
      const uint32_t n = stage == ARIXV_PACK_STAGE_1 ? info.stage1_block_count
                                                     : info.stage2_block_count;
      for (uint32_t b = 0; b < n; ++b) {
        std::vector<void*> ptrs(mi.size(), nullptr);
        std::vector<size_t> caps(mi.size(), 0);
        for (size_t i = 0; i < mi.size(); ++i) {
          if (mi[i].stage == stage && mi[i].block_index == b) {
            ptrs[i] = data[i].data();
            caps[i] = data[i].size();
          }
        }
        const arixv_pack_outputs v{sizeof v, info.module_count, ptrs.data(),
                                   caps.data()};
        CHECK((stage == ARIXV_PACK_STAGE_1
                   ? Stage1(p, b, &v, scratch.data(), ss)
                   : Stage2(p, b, &v, scratch.data(), ss)) == ARIXV_STATUS_OK);
      }
    }
    for (size_t i = 0; i < mods.size(); ++i) CHECK(data[i] == mods[i]);
  }
  // Failed stage-1 attempt, then reset and retry.
  {
    Bufs b = MakeBufs(p);
    CHECK(arixv_pack_reset_scratch(p.data(), p.size(), scratch.data(), ss) ==
          ARIXV_STATUS_OK);
    std::vector<size_t> zero(b.caps.size(), 0);
    const arixv_pack_outputs tight{sizeof tight, b.view.module_count,
                                   b.ptrs.data(), zero.data()};
    CHECK(Stage1(p, 0, &tight, scratch.data(), ss) ==
          ARIXV_STATUS_BUFFER_TOO_SMALL);
    CHECK(Stage2(p, 0, &b.view, scratch.data(), ss) ==
          ARIXV_STATUS_DEPENDENCY_INCOMPLETE);
    CHECK(arixv_pack_reset_scratch(p.data(), p.size(), scratch.data(), ss) ==
          ARIXV_STATUS_OK);
    for (uint32_t i = 0; i < info.stage1_block_count; ++i) {
      CHECK(Stage1(p, i, &b.view, scratch.data(), ss) == ARIXV_STATUS_OK);
    }
    for (uint32_t j = 0; j < info.stage2_block_count; ++j) {
      CHECK(Stage2(p, j, &b.view, scratch.data(), ss) == ARIXV_STATUS_OK);
    }
  }
  // Reset scratch rules.
  CHECK(arixv_pack_reset_scratch(p.data(), p.size(), nullptr, 1) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  CHECK(arixv_pack_reset_scratch(p.data(), p.size(), nullptr, 0) ==
        ARIXV_STATUS_INVALID_ARGUMENT);
  CHECK(arixv_pack_reset_scratch(p.data(), p.size(), scratch.data(), ss - 1) ==
        ARIXV_STATUS_BUFFER_TOO_SMALL);
}

void TestConformance(const std::vector<Bytes>& fx) {
  // Foreign, short and future-version inputs (1.0 status order).
  {
    const Bytes& m = fx[0];
    size_t cap = arixv_encode_bound(m.size(), nullptr);
    Bytes arx2(cap);
    CHECK(arixv_encode(m.data(), m.size(), arx2.data(), &cap, nullptr) ==
          ARIXV_STATUS_OK);
    arx2.resize(cap);
    arixv_pack_info info = {};
    info.struct_size = sizeof info;
    CHECK(arixv_pack_inspect(arx2.data(), arx2.size(), &info) ==
          ARIXV_STATUS_BAD_CONTAINER);
    const Bytes tiny = {'A', 'R', 'X'};
    CHECK(arixv_pack_inspect(tiny.data(), tiny.size(), &info) ==
          ARIXV_STATUS_BAD_CONTAINER);
    const Bytes magic = {'A', 'R', 'X', 'P'};
    CHECK(arixv_pack_inspect(magic.data(), magic.size(), &info) ==
          ARIXV_STATUS_TRUNCATED);
    const Bytes future = {'A', 'R', 'X', 'P', 0x99};
    CHECK(arixv_pack_inspect(future.data(), future.size(), &info) ==
          ARIXV_STATUS_UNSUPPORTED_FORMAT);
    CHECK(arixv_status_string(ARIXV_STATUS_DEPENDENCY_INCOMPLETE) !=
          std::string("unknown status"));
  }
  std::vector<Bytes> mods;
  for (const Bytes& m : fx) mods.push_back(m);
  for (const Bytes& m : fx) {
    if (Renumberable(m)) mods.push_back(Renumber(m, 9));
  }
  mods.push_back(HeaderOnly());
  mods.push_back(HeaderOnly());
  std::vector<const Bytes*> ms;
  std::vector<size_t> sizes;
  for (const Bytes& m : mods) {
    ms.push_back(&m);
    sizes.push_back(m.size());
  }
  const Bytes p = MustEncode(ms, Par(4));
  MustRoundTrip(ms, p);
  const uint32_t n = static_cast<uint32_t>(ms.size());
  CHECK(p.size() <= arixv_pack_encode_bound(sizes.data(), n, nullptr));
  arixv_pack_info info = Inspect(p);
  CHECK(info.stage1_block_count > 0);
  const size_t hl = pk::HeaderBytes(p.data(), p.size());
  CHECK(hl > 8);
  // Varints: non-minimal and overflowing encodings are malformed; a cut
  // inside one is truncated.
  {
    CHECK(p[8] < 0x80);  // module_count fits one byte here
    Bytes t(p.begin(), p.begin() + 8);
    t.push_back(static_cast<uint8_t>(p[8] | 0x80));
    t.push_back(0x00);
    t.insert(t.end(), p.begin() + 9, p.end());
    Resign(t, hl + 1);
    CHECK(arixv_pack_inspect(t.data(), t.size(), &info) ==
          ARIXV_STATUS_BAD_CONTAINER);
    Bytes o(p.begin(), p.begin() + 8);
    for (int i = 0; i < 10; ++i) o.push_back(0xff);
    o.push_back(0x01);
    o.insert(o.end(), p.begin() + 9, p.end());
    CHECK(arixv_pack_inspect(o.data(), o.size(), &info) ==
          ARIXV_STATUS_BAD_CONTAINER);
    const Bytes cut(p.begin(), p.begin() + 9);
    CHECK(arixv_pack_inspect(cut.data(), cut.size(), &info) ==
          ARIXV_STATUS_TRUNCATED);
    // Header checksum, reserved byte and flag bits.
    Bytes c = p;
    c[hl - 1] ^= 1;
    CHECK(arixv_pack_inspect(c.data(), c.size(), &info) ==
          ARIXV_STATUS_BAD_CONTAINER);
    for (size_t at : {size_t{5}, size_t{6}, size_t{7}}) {
      Bytes r = p;
      r[at] |= 0x80;
      Resign(r, hl);
      CHECK(arixv_pack_inspect(r.data(), r.size(), &info) ==
            ARIXV_STATUS_BAD_CONTAINER);
    }
    Bytes trail = p;
    trail.push_back(0);
    CHECK(arixv_pack_inspect(trail.data(), trail.size(), &info) ==
          ARIXV_STATUS_BAD_CONTAINER);
  }
  // Scratch-free (solid) pack: (NULL, 0) and pooled buffers accepted.
  {
    const Bytes sp = MustEncode(ms, Par(1));
    CHECK(Inspect(sp).scratch_size == 0);
    Bufs b = MakeBufs(sp);
    Bytes pool(64);
    CHECK(arixv_pack_reset_scratch(sp.data(), sp.size(), nullptr, 0) ==
          ARIXV_STATUS_OK);
    CHECK(arixv_pack_reset_scratch(sp.data(), sp.size(), pool.data(),
                                   pool.size()) == ARIXV_STATUS_OK);
    CHECK(Stage2(sp, 0, &b.view, nullptr, 1) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(Stage2(sp, 0, &b.view, pool.data(), pool.size()) == ARIXV_STATUS_OK);
    for (size_t i = 0; i < mods.size(); ++i) CHECK(b.data[i] == mods[i]);
  }
  // Explicit coding order (header positions) and FAST_DECODE.
  {
    pk::EncodeParams rev = Par(4);
    rev.reverse_coding_order = true;
    const Bytes rp2 = MustEncode(ms, rev);
    CHECK(rp2[5] == 0);  // pack flags: explicit order
    MustRoundTrip(ms, rp2);
    pk::EncodeParams fast = Par(4);
    fast.codec_flags |= pk::kCodecFastDecode;
    const Bytes fp = MustEncode(ms, fast);
    CHECK(Inspect(fp).flags == ARIXV_PACK_INFO_FAST_DECODE);
    MustRoundTrip(ms, fp);
  }
  // Stored blocks, edge-case sets, extreme parallelism, budgets.
  {
    std::vector<Bytes> noise;
    for (int i = 0; i < 6; ++i) {
      Bytes r(3000 + 7 * i);
      for (auto& c : r) c = static_cast<uint8_t>(Rand());
      noise.push_back(r);
    }
    std::vector<const Bytes*> nm;
    std::vector<size_t> nsz;
    for (const Bytes& r : noise) {
      nm.push_back(&r);
      nsz.push_back(r.size());
    }
    pk::EncodeStats st;
    const Bytes np = MustEncode(nm, Par(4), &st);
    CHECK(st.stored_blocks > 0);
    CHECK(np.size() <= arixv_pack_encode_bound(
                           nsz.data(), static_cast<uint32_t>(nsz.size()),
                           nullptr));
    MustRoundTrip(nm, np);
    const Bytes empty;
    std::vector<const Bytes*> empties(5, &empty);
    MustRoundTrip(empties, MustEncode(empties, Par(4)));
    // 100% means no stage 1, even when every module fits a zero allowance.
    {
      pk::EncodeStats es;
      MustRoundTrip(empties, MustEncode(empties, Par(4, 100), &es));
      CHECK(es.stage1_blocks == 0);
    }
    std::vector<const Bytes*> one(1, &mods[0]);
    MustRoundTrip(one, MustEncode(one, Par(0)));
    std::vector<const Bytes*> dups(10, &mods[0]);
    MustRoundTrip(dups, MustEncode(dups, Par(4)));
    MustRoundTrip(ms, MustEncode(ms, Par(0xffffffffu)));
    for (uint32_t budget : {100u, 150u, 300u, 1000u, 100000u}) {
      pk::EncodeStats bs;
      MustRoundTrip(ms, MustEncode(ms, Par(4, budget), &bs));
      if (budget == 100) CHECK(bs.stage1_blocks == 0);
    }
  }
  // Encode argument validation.
  {
    std::vector<const void*> ptrs;
    for (const Bytes& m : mods) ptrs.push_back(m.data());
    size_t cap = arixv_pack_encode_bound(sizes.data(), n, nullptr);
    CHECK(cap > 0);
    Bytes out(cap);
    auto enc = [&](const arixv_pack_encode_options* o) {
      size_t c = out.size();
      return arixv_pack_encode(ptrs.data(), sizes.data(), n, out.data(), &c, o);
    };
    arixv_pack_encode_options o = {sizeof o, 0, 4, 0, 0};
    CHECK(enc(&o) == ARIXV_STATUS_OK);
    for (uint32_t budget : {1u, 99u, 100001u}) {
      o.latency_budget_pct = budget;
      CHECK(enc(&o) == ARIXV_STATUS_INVALID_ARGUMENT);
      CHECK(arixv_pack_encode_bound(sizes.data(), n, &o) == 0);
    }
    o = {sizeof o, 2, 4, 0, 0};
    CHECK(enc(&o) == ARIXV_STATUS_INVALID_ARGUMENT);
    o = {ARIXV_PACK_ENCODE_OPTIONS_V1_SIZE - 1, 0, 4, 0, 0};
    CHECK(enc(&o) == ARIXV_STATUS_INVALID_ARGUMENT);
    size_t c = out.size();
    CHECK(arixv_pack_encode(ptrs.data(), sizes.data(), 0, out.data(), &c,
                            nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(arixv_pack_encode(nullptr, sizes.data(), n, out.data(), &c,
                            nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    CHECK(arixv_pack_encode(ptrs.data(), sizes.data(), n, nullptr, &c,
                            nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    std::vector<const void*> holes = ptrs;
    holes[0] = nullptr;
    CHECK(arixv_pack_encode(holes.data(), sizes.data(), n, out.data(), &c,
                            nullptr) == ARIXV_STATUS_INVALID_ARGUMENT);
    // BUFFER_TOO_SMALL reports the exact size and leaves out untouched.
    const size_t need = p.size();
    Bytes small(need - 1, 0x77);
    size_t sc = small.size();
    o = {sizeof o, 0, 4, 0, 0};
    CHECK(arixv_pack_encode(ptrs.data(), sizes.data(), n, small.data(), &sc,
                            &o) == ARIXV_STATUS_BUFFER_TOO_SMALL);
    CHECK(sc == need);
    for (uint8_t b : small) CHECK(b == 0x77);
    CHECK(arixv_pack_encode_bound(nullptr, n, nullptr) == 0);
    CHECK(arixv_pack_encode_bound(sizes.data(), 0, nullptr) == 0);
    const size_t huge = size_t{UINT32_MAX} + (sizeof(size_t) > 4 ? 1 : 0);
    if (sizeof(size_t) > 4) {
      CHECK(arixv_pack_encode_bound(&huge, 1, nullptr) == 0);
    }
  }
}

// The planner's integer log2 is within 2/65536 bit of floor(log2(x)*2^16).
void TestLog2Q16() {
  std::vector<uint64_t> xs = {1, 2, 3, 4, 5, 7, 8, 9, 1000, 4095, 4096, 65535,
                              65536, 1ull << 31, (1ull << 32) - 1, 1ull << 40,
                              ~0ull};
  for (int i = 0; i < 20000; ++i) {
    xs.push_back(1 + (uint64_t{Rand()} << (Rand() % 32)));
  }
  for (uint64_t x : xs) {
    const int64_t got = pk::Log2Q16(x);
    const double want = std::floor(std::log2(static_cast<double>(x)) * 65536.0);
    CHECK(std::fabs(static_cast<double>(got) - want) <= 2.0);
  }
  CHECK(pk::Log2Q16(1) == 0 && pk::Log2Q16(2) == 65536 &&
        pk::Log2Q16(1ull << 32) == (int64_t{32} << 16));
}

void TestHostile(const Bytes& p) {
  const size_t hl = pk::HeaderBytes(p.data(), p.size());
  CHECK(hl > 8);
  for (int it = 0; it < 3000; ++it) {
    Bytes t = p;
    const int muts = 1 + static_cast<int>(Rand() % 3);
    const bool header_only = it % 2 == 0;
    for (int k = 0; k < muts; ++k) {
      const size_t at = header_only ? Rand() % (hl - 8) : Rand() % t.size();
      t[at] ^= static_cast<uint8_t>(1u << (Rand() % 8));
    }
    if (header_only) Resign(t, hl);
    if (Rand() % 4 == 0) t.resize(Rand() % t.size());
    DecodeAnything(t, size_t{1} << 26);
  }
}

// Random module sets (with renumbered variants, bit flips and duplicates)
// must round-trip under random layouts; their mutated packs must decode
// safely.
void Fuzz(const std::vector<Bytes>& fx, int iters) {
  for (int it = 0; it < iters; ++it) {
    const size_t n = 1 + Rand() % 7;
    std::vector<Bytes> mods;
    for (size_t k = 0; k < n; ++k) {
      Bytes m = fx[Rand() % fx.size()];
      if (Rand() % 3 == 0 && Renumberable(m)) m = Renumber(m, Rand() % 64);
      const int flips = static_cast<int>(Rand() % 3);
      for (int f = 0; f < flips && m.size() > 4; ++f) {
        const size_t at = 4 + Rand() % (m.size() - 4);
        m[at] = static_cast<uint8_t>(m[at] ^ (1u << (Rand() % 8)));
      }
      if (Rand() % 5 == 0 && !mods.empty()) m = mods[Rand() % mods.size()];
      mods.push_back(m);
    }
    std::vector<const Bytes*> ptrs;
    for (const Bytes& m : mods) ptrs.push_back(&m);
    pk::EncodeParams p = Par(Rand() % 6, Rand() % 2 ? 0 : 100 + Rand() % 400);
    if (Rand() % 3 == 0) p.max_block_bytes = 64 + Rand() % 4096;
    if (Rand() % 4 == 0) p.codec_flags &= static_cast<uint8_t>(~pk::kCodecWarm);
    if (Rand() % 4 == 0) p.codec_flags |= pk::kCodecFastDecode;
    p.reverse_coding_order = Rand() % 4 == 0;
    Bytes pack = MustEncode(ptrs, p);
    MustRoundTrip(ptrs, pack);
    const int muts = 1 + static_cast<int>(Rand() % 4);
    for (int k = 0; k < muts && !pack.empty(); ++k) {
      switch (Rand() % 3) {
        case 0: pack[Rand() % pack.size()] ^= static_cast<uint8_t>(1u << (Rand() % 8)); break;
        case 1: pack.resize(Rand() % pack.size()); break;
        case 2: pack[Rand() % pack.size()] = static_cast<uint8_t>(Rand()); break;
      }
    }
    DecodeAnything(pack, size_t{1} << 22);
  }
}

// ---- ARXP v1 golden vectors (tests/golden/pack, docs/format-arxp.md) ----
// Inputs are generated deterministically from in-tree material: the decoded
// ARX2 golden modules, ShaderLike shaders and their renumbered variants, and
// a fixed xorshift stream for incompressible data. Each case's encoding must
// equal the committed file byte for byte, and the committed file must decode
// to the inputs.
struct PackGoldenCase {
  const char* file;
  int inputs;  // 0: ARX2 goldens; 1: variant families; 2: mixed; 3: noise
  uint32_t parallelism, budget;
  bool reverse, warm, fast;
  // Expected: file size, FNV-1a 64 of the file and of the concatenated
  // inputs, module count, stage block counts, stored-block count.
  size_t size;
  uint64_t file_fnv, input_fnv;
  uint32_t modules, s1, s2, stored;
};

constexpr PackGoldenCase kPackGoldens[] = {
#include "golden/pack/expected.inc"
};

std::vector<Bytes> PackGoldenInputs(int which, const std::vector<Bytes>& g) {
  std::vector<Bytes> v;
  if (which == 0) return g;
  // g: [empty, random, bytes, minimal, swapped, rich, unknown, large]
  for (uint32_t seed = 0; seed < 4; ++seed) {
    const Bytes base = ShaderLike(seed);
    v.push_back(base);
    for (uint32_t d : {3u, 7u, 11u}) v.push_back(Renumber(base, d));
  }
  v.push_back(g[5]);
  v.push_back(g[7]);
  v.push_back(Renumber(g[5], 5));
  v.push_back(Renumber(g[7], 9));
  if (which == 1) return v;
  std::vector<Bytes> noise;
  uint64_t x = 0x9e3779b97f4a7c15ull;
  for (int i = 0; i < 4; ++i) {
    Bytes r(2000 + 64 * i);
    for (auto& c : r) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      c = static_cast<uint8_t>(x >> 32);
    }
    noise.push_back(r);
  }
  if (which == 3) return noise;
  std::vector<Bytes> m(noise.begin(), noise.begin() + 3);
  for (const Bytes& b : g) m.push_back(b);
  m.push_back(g[5]);
  m.push_back(g[5]);
  m.push_back(HeaderOnly());
  m.push_back(HeaderOnly());
  m.push_back(v[1]);
  m.push_back(v[0]);
  return m;
}

pk::EncodeParams PackGoldenParams(const PackGoldenCase& c) {
  pk::EncodeParams p = Par(c.parallelism, c.budget);
  p.reverse_coding_order = c.reverse;
  p.codec_flags = static_cast<uint8_t>((c.warm ? pk::kCodecWarm : 0) |
                                       (c.fast ? pk::kCodecFastDecode : 0));
  return p;
}

// With `write`, regenerates the files into dir/pack and prints the
// expectation table; otherwise checks every case.
void TestPackGoldens(const std::string& golden_dir, bool write) {
  const std::vector<Bytes> g = AllGoldenArx2(golden_dir);
  CHECK(g.size() == 8);
  const std::filesystem::path pack_dir =
      std::filesystem::path(golden_dir) / "pack";
  if (write) {
    std::printf(
        "    // Generated by pack_test --golden-dir tests/golden "
        "--write-pack-goldens.\n"
        "    // file, inputs, parallelism, budget, reverse, warm, fast, size, "
        "file FNV-1a,\n"
        "    // input FNV-1a, modules, stage-1 blocks, stage-2 blocks, stored "
        "blocks\n");
  }
  for (const PackGoldenCase& c : kPackGoldens) {
    const std::vector<Bytes> in = PackGoldenInputs(c.inputs, g);
    std::vector<const Bytes*> ms;
    uint64_t in_fnv = 0xcbf29ce484222325ull;
    for (const Bytes& b : in) {
      ms.push_back(&b);
      in_fnv = pk::Fnv1a(b.data(), b.size(), in_fnv);
    }
    pk::EncodeStats st;
    const Bytes enc = MustEncode(ms, PackGoldenParams(c), &st);
    const arixv_pack_info info = Inspect(enc);
    if (write) {
      std::filesystem::create_directories(pack_dir);
      std::ofstream f(pack_dir / c.file, std::ios::binary);
      f.write(reinterpret_cast<const char*>(enc.data()),
              static_cast<std::streamsize>(enc.size()));
      std::printf(
          "    {\"%s\", %d, %u, %u, %s, %s, %s, %zu, 0x%016llxull, "
          "0x%016llxull, %u, %u, %u, %u},\n",
          c.file, c.inputs, c.parallelism, c.budget,
          c.reverse ? "true" : "false", c.warm ? "true" : "false",
          c.fast ? "true" : "false", enc.size(),
          static_cast<unsigned long long>(pk::Fnv1a(enc.data(), enc.size())),
          static_cast<unsigned long long>(in_fnv), info.module_count,
          info.stage1_block_count, info.stage2_block_count, st.stored_blocks);
      continue;
    }
    // Inputs and encoder are unchanged, and the committed file decodes.
    CHECK(in_fnv == c.input_fnv);
    Bytes file;
    CHECK(ReadFile(pack_dir / c.file, &file));
    CHECK(file.size() == c.size);
    CHECK(pk::Fnv1a(file.data(), file.size()) == c.file_fnv);
    if (enc != file) {
      std::fprintf(stderr, "golden %s: encoder output differs\n", c.file);
      CHECK(false);
    }
    CHECK(info.module_count == c.modules && info.stage1_block_count == c.s1 &&
          info.stage2_block_count == c.s2 && st.stored_blocks == c.stored);
    MustRoundTrip(ms, file);
  }
  if (!write) {
    std::printf("pack_test: %zu ARXP golden vectors ok\n",
                sizeof(kPackGoldens) / sizeof(kPackGoldens[0]));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string golden_dir;
  int fuzz_iters = 200;
  bool write_pack_goldens = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--golden-dir" && i + 1 < argc) golden_dir = argv[++i];
    else if (a == "--write-pack-goldens") write_pack_goldens = true;
    else if (a == "--fuzz-iters" && i + 1 < argc) fuzz_iters = std::atoi(argv[++i]);
  }
  if (write_pack_goldens) {
    CHECK(!golden_dir.empty());
    TestPackGoldens(golden_dir, true);
    return 0;
  }
  std::vector<Bytes> fx;
  for (const Bytes& m : AllGoldenArx2(golden_dir)) {
    if (arixv::IsSpirvModelEligible(m.data(), m.size())) fx.push_back(m);
  }
  if (!golden_dir.empty()) CHECK(fx.size() >= 4);
  fx.push_back(ShaderLike(0));
  fx.push_back(ShaderLike(1));
  fx.push_back(ShaderLike(2));
  fx.push_back(Swapped(ShaderLike(4)));
  std::printf("pack_test: %zu fixture modules\n", fx.size());

  Bytes two_stage;
  TestLayouts(fx, &two_stage);
  TestInspection(two_stage);
  TestDecodeContract(fx, two_stage);
  TestConformance(fx);
  TestLog2Q16();
  TestHostile(two_stage);
  if (!golden_dir.empty()) TestPackGoldens(golden_dir, false);
  Fuzz(fx, fuzz_iters);
  std::printf("pack_test: ok (%d fuzz iterations)\n", fuzz_iters);
  return 0;
}
