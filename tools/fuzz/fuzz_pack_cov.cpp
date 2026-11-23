// Coverage-guided ARXP shader-pack harness (libFuzzer entry point). Build
// with clang++ -fsanitize=fuzzer[,address,undefined]. One source, two modes:
//
// ARIXV_PACK_FUZZ_RT (round trip): the input is split at 4-byte-aligned
//   SPIR-V magic words into modules (any bytes: non-SPIR-V segments are
//   modules too); the last input byte picks the layout options. Checks that
//   encoding is deterministic, fits the public bound, and that the
//   single-call and two-stage decodes both reproduce every module.
//
// ARIXV_PACK_FUZZ_DEC (hostile decode): arbitrary bytes as a pack. Checks
//   that nothing crashes or writes outside its outputs, that inspection and
//   decode agree, and that the single-call and two-stage paths accept the
//   same packs and produce the same bytes. The fuzz library skips the header
//   checksum (ARIXV_FUZZ_NO_PACK_CHECKSUM) so mutations reach the structural
//   validation and the block decoder behind it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "arixv/arixv.h"
#include "pack.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace pk = arixv::pack;

constexpr uint64_t kMaxOutput = uint64_t{1} << 24;

[[noreturn]] void Fail(const char* what, arixv_status st) {
  std::fprintf(stderr, "FAIL: %s (status=%s)\n", what,
               arixv_status_string(st));
  std::abort();
}

struct Outputs {
  std::vector<Bytes> data;
  std::vector<void*> ptrs;
  std::vector<size_t> caps;
  arixv_pack_outputs view{};
};

Outputs Make(const std::vector<arixv_pack_module_info>& mi) {
  Outputs o;
  o.data.resize(mi.size());
  for (size_t i = 0; i < mi.size(); ++i) {
    o.data[i].assign(static_cast<size_t>(mi[i].size), 0xA5);
    o.ptrs.push_back(o.data[i].data());
    o.caps.push_back(o.data[i].size());
  }
  o.view = {sizeof(arixv_pack_outputs), static_cast<uint32_t>(mi.size()),
            o.ptrs.data(), o.caps.data()};
  return o;
}

// Decodes through both paths. Returns false if the pack is out of budget
// or invalid; on success `single` holds the modules.
bool DecodeBoth(const uint8_t* data, size_t size, Outputs* single,
                arixv_status* st_single, arixv_status* st_staged) {
  arixv_pack_info info{};
  info.struct_size = sizeof(info);
  if (arixv_pack_inspect(data, size, &info) != ARIXV_STATUS_OK) return false;
  if (info.total_output_size > kMaxOutput ||
      info.scratch_size > 4 * kMaxOutput + (uint64_t{64} << 20)) {
    return false;
  }
  std::vector<arixv_pack_module_info> mi(info.module_count);
  if (arixv_pack_inspect_modules(data, size, mi.data(), info.module_count) !=
      ARIXV_STATUS_OK) {
    Fail("inspect_modules failed after inspect succeeded", ARIXV_STATUS_OK);
  }
  uint64_t sum = 0;
  for (const auto& m : mi) sum += m.size;
  if (sum != info.total_output_size) Fail("module sizes != total", 0);
  *single = Make(mi);
  *st_single = arixv_pack_decode(data, size, &single->view, nullptr);
  Outputs staged = Make(mi);
  Bytes scratch(static_cast<size_t>(info.scratch_size) + 3);
  void* sp = info.scratch_size ? scratch.data() + 3 : nullptr;
  const size_t ss = static_cast<size_t>(info.scratch_size);
  arixv_status st = arixv_pack_reset_scratch(data, size, sp, ss);
  for (uint32_t b = 0; st == ARIXV_STATUS_OK && b < info.stage1_block_count;
       ++b) {
    st = arixv_pack_decode_stage1(data, size, b, &staged.view, sp, ss, nullptr);
  }
  for (uint32_t b = info.stage2_block_count;
       st == ARIXV_STATUS_OK && b-- > 0;) {
    st = arixv_pack_decode_stage2(data, size, b, &staged.view, sp, ss, nullptr);
  }
  if (st == ARIXV_STATUS_DEPENDENCY_INCOMPLETE) {
    Fail("dependency incomplete after every stage-1 block succeeded", st);
  }
  *st_staged = st;
  if ((*st_single == ARIXV_STATUS_OK) != (st == ARIXV_STATUS_OK)) {
    Fail("single-call and two-stage decode disagree", *st_single);
  }
  if (st == ARIXV_STATUS_OK && staged.data != single->data) {
    Fail("single-call and two-stage outputs differ", st);
  }
  return true;
}

}  // namespace

#if defined(ARIXV_PACK_FUZZ_RT)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  const uint8_t sel = data[size - 1];
  std::vector<Bytes> mods;
  size_t start = 0;
  for (size_t i = 4; i + 4 <= size - 1 && mods.size() < 15; i += 4) {
    const uint32_t w = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16) |
                       (static_cast<uint32_t>(data[i + 3]) << 24);
    if (w == 0x07230203u || w == 0x03022307u) {
      mods.emplace_back(data + start, data + i);
      start = i;
    }
  }
  mods.emplace_back(data + start, data + size - 1);
  if (sel & 0x80) mods.push_back(mods[0]);  // an exact duplicate
  std::vector<pk::ModuleView> views;
  for (const Bytes& m : mods) views.push_back({m.data(), m.size()});
  pk::EncodeParams p;
  p.parallelism = sel & 7;
  p.latency_budget_pct = (sel & 8) ? 100 + 50u * ((sel >> 4) & 7) : 0;
  p.max_block_bytes = (sel & 0x40) ? 1024 : 0;
  if (sel & 0x10) p.codec_flags |= pk::kCodecFastDecode;
  if ((sel & 0x24) == 0x24) p.codec_flags &= ~pk::kCodecWarm;
  p.reverse_coding_order = (sel & 0x21) == 0x21;
  Bytes pack, again;
  arixv_status st = pk::Encode(views.data(), views.size(), p, &pack, nullptr);
  if (st != ARIXV_STATUS_OK) Fail("encode failed", st);
  std::vector<size_t> sizes;
  for (const Bytes& m : mods) sizes.push_back(m.size());
  const size_t bound = arixv_pack_encode_bound(
      sizes.data(), static_cast<uint32_t>(sizes.size()), nullptr);
  if (pack.size() > bound) Fail("pack exceeds arixv_pack_encode_bound", st);
  if ((sel & 3) == 0) {
    st = pk::Encode(views.data(), views.size(), p, &again, nullptr);
    if (st != ARIXV_STATUS_OK || again != pack) Fail("encode not deterministic", st);
  }
  Outputs out;
  arixv_status s1 = ARIXV_STATUS_OK, s2 = ARIXV_STATUS_OK;
  if (!DecodeBoth(pack.data(), pack.size(), &out, &s1, &s2)) {
    Fail("encoded pack rejected by inspection", ARIXV_STATUS_OK);
  }
  if (s1 != ARIXV_STATUS_OK) Fail("round-trip decode failed", s1);
  if (out.data != mods) Fail("round trip changed the modules", s1);
  return 0;
}
#elif defined(ARIXV_PACK_FUZZ_DEC)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Outputs out;
  arixv_status s1 = ARIXV_STATUS_OK, s2 = ARIXV_STATUS_OK;
  (void)DecodeBoth(data, size, &out, &s1, &s2);
  return 0;
}
#else
#error "define ARIXV_PACK_FUZZ_RT or ARIXV_PACK_FUZZ_DEC"
#endif
