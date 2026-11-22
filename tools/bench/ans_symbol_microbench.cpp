// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// E-A2 (docs/development/performance.md): what would multi-symbol adaptive
// coding buy at the bit-tree sites (opcode index, rank trees, uint length,
// enum indices, bytes — ~47% of M2 decisions)?
//
// The corpus is encoded once with the real model while recording every
// BitTree value as (tree, depth, value). The tree events are replayed, with
// fresh contexts per module, as:
//   binary      today's adaptive binary tree over the LZMA-style range coder
//   nib-range   the same tree split MSB-first into <=4-bit chunks, each chunk
//               an adaptive 2..16-symbol CDF selected by the prefix above it
//               (same context structure as the bit tree, fewer, wider
//               nodes), range-coded with one division per symbol
//   nib-rans    the same nibble model over rANS (no division)
//   nib-tight   nib-rans with the chunk walk resolved at compile time per
//               tree depth (the form a real implementation would take)
// "dep" makes the next event's context index data-dependent on the previous
// decoded value (as the real model's context selection is).
//
// CDF model: 15-bit total; cdf[i] = P(sym < i) * (32768 - N), adapted toward
// 0 (i <= sym) or 32768 - N (i > sym) by a count-warmed shift, stored as
// C[i] = cdf[i] + i so every symbol keeps frequency >= 1. SSE2 search and
// update (two 8-lane vectors per CDF).
//
// Timings include a per-module context reset (a memset for binary, a table
// copy for nibbles); the reset rows report it so it can be subtracted, since
// the real decoder resets lazily.

#include <emmintrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "arixv/arixv.h"
#include "prob.h"
#include "range_coder.h"

#ifndef ARIXV_TRACE_DECISIONS
#error "build with -DARIXV_TRACE_DECISIONS"
#endif

using arixv::Prob;

namespace {

int g_rate_cap = 5;  // --rate-cap=N

// Stored directly in the coding domain: c[i] = C[i] = cdf[i] + i, with
// c[n] == 32768 and unused lanes >= 32768, so lookups need no branches.
struct alignas(16) Cdf {
  uint16_t c[17];
  uint16_t count;
  uint16_t n;
};

struct Event {
  uint32_t tree;
  uint32_t value;
};

struct Module {
  std::vector<Event> events;
  std::vector<int> tree_bits;
  // binary layout
  std::vector<uint32_t> bin_offset;
  uint32_t bin_size = 0;
  // nibble layout: per tree, first table index of each chunk
  std::vector<uint32_t> nib_offset;
  uint32_t nib_tables = 0;
  std::vector<uint8_t> bin_payload;
  std::vector<uint8_t> nib_range_payload;
  std::vector<uint16_t> nib_rans_payload;
  std::vector<Cdf> nib_init;  // fresh tables, copied per decode (like a memset)
};

bool ReadFile(const std::string& path, std::vector<uint8_t>& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  data.assign(std::istreambuf_iterator<char>(in), {});
  return true;
}

// Chunks MSB first: the first takes kBits % 4 (or 4), the rest 4 each.
int FirstChunk(int bits) { return bits % 4 == 0 ? 4 : bits % 4; }
uint32_t NibbleTables(int bits) {
  uint32_t tables = 0;
  int prefix = 0;
  int chunk = FirstChunk(bits);
  while (prefix < bits) {
    tables += 1u << prefix;
    prefix += chunk;
    chunk = 4;
  }
  return tables;
}

// ---- adaptive CDF -----------------------------------------------------------


void InitCdf(Cdf& t, int n) {
  const uint32_t top = 32768u - static_cast<uint32_t>(n);
  for (int i = 0; i <= 16; ++i) {
    const uint32_t cdf = i < n ? (top * static_cast<uint32_t>(i)) / n : top;
    t.c[i] = static_cast<uint16_t>(cdf + static_cast<uint32_t>(i));
  }
  t.count = 0;
  t.n = static_cast<uint16_t>(n);
}

// Coding CDF value C[i] = cdf[i] + i; C[n] = 32768.
inline uint32_t CodeC(const Cdf& t, int i) { return t.c[i]; }

// Largest s with C[s] <= v (v < 32768).
inline int Search(const Cdf& t, uint32_t v) {
  const __m128i* p = reinterpret_cast<const __m128i*>(t.c);
  const __m128i c0 = _mm_load_si128(p);
  const __m128i c1 = _mm_load_si128(p + 1);
  const __m128i vv = _mm_set1_epi16(static_cast<short>(v));
  // C <= v  <=>  saturating C - v == 0 (unused lanes hold >= 32768).
  const __m128i z = _mm_setzero_si128();
  const __m128i le0 = _mm_cmpeq_epi16(_mm_subs_epu16(c0, vv), z);
  const __m128i le1 = _mm_cmpeq_epi16(_mm_subs_epu16(c1, vv), z);
  const uint32_t mask = static_cast<uint32_t>(
      _mm_movemask_epi8(_mm_packs_epi16(le0, le1)));
  // C is increasing, so the C <= v lanes are a prefix: count = ctz(~mask).
  // (popcount would be a libgcc call without -mpopcnt.)
  return __builtin_ctz(~mask) - 1;
}

inline void Adapt(Cdf& t, int s) {
  int rate = 32 - __builtin_clz(static_cast<unsigned>(t.count) + 1u);
  if (rate > g_rate_cap) rate = g_rate_cap;
  if (t.count < 255) ++t.count;
  const __m128i shift = _mm_cvtsi32_si128(rate);
  const __m128i top = _mm_set1_epi16(static_cast<short>(32768 - t.n));
  const __m128i sv = _mm_set1_epi16(static_cast<short>(s));
  const __m128i lane_lo = _mm_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7);
  const __m128i lane_hi = _mm_setr_epi16(8, 9, 10, 11, 12, 13, 14, 15);
  __m128i* p = reinterpret_cast<__m128i*>(t.c);
  for (int k = 0; k < 2; ++k) {
    const __m128i c = _mm_load_si128(p + k);
    const __m128i lane = k == 0 ? lane_lo : lane_hi;
    const __m128i up = _mm_cmpgt_epi16(lane, sv);
    const __m128i target = _mm_add_epi16(lane, _mm_and_si128(up, top));
    const __m128i delta = _mm_sra_epi16(_mm_sub_epi16(target, c), shift);
    _mm_store_si128(p + k, _mm_add_epi16(c, delta));
  }
}

// ---- range coder (encoder side, multi-symbol capable) -----------------------

class RangeEnc {
 public:
  explicit RangeEnc(std::vector<uint8_t>& out) : out_(out) {}
  void EncodeBit(Prob& p, int bit) {
    const uint32_t bound = (range_ >> arixv::kProbBits) * p.value();
    if (bit == 0) {
      range_ = bound;
    } else {
      low_ += bound;
      range_ -= bound;
    }
    p.Update(bit);
    Normalize();
  }
  void EncodeSym(const Cdf& t, int s) {
    const uint32_t r = range_ >> 15;
    const uint32_t lo = CodeC(t, s);
    low_ += static_cast<uint64_t>(lo) * r;
    range_ = s + 1 < t.n ? (CodeC(t, s + 1) - lo) * r : range_ - lo * r;
    Normalize();
  }
  void Flush() {
    for (int i = 0; i < 5; ++i) ShiftLow();
  }

 private:
  void Normalize() {
    while (range_ < arixv::kRangeTop) {
      ShiftLow();
      range_ <<= 8;
    }
  }
  void ShiftLow() {
    if (static_cast<uint32_t>(low_) < 0xFF000000u ||
        static_cast<uint32_t>(low_ >> 32) != 0) {
      uint8_t temp = cache_;
      do {
        out_.push_back(static_cast<uint8_t>(temp + static_cast<uint8_t>(low_ >> 32)));
        temp = 0xFF;
      } while (--cache_size_ != 0);
      cache_ = static_cast<uint8_t>(static_cast<uint32_t>(low_) >> 24);
    }
    ++cache_size_;
    low_ = static_cast<uint32_t>(low_) << 8;
  }
  std::vector<uint8_t>& out_;
  uint64_t low_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint8_t cache_ = 0;
  uint64_t cache_size_ = 1;
};

struct RangeDec {
  const uint8_t* data;
  size_t size;
  size_t pos = 1;
  uint32_t code = 0;
  uint32_t range = 0xFFFFFFFFu;
  RangeDec(const uint8_t* d, size_t n) : data(d), size(n) {
    for (int i = 0; i < 4; ++i) code = (code << 8) | Byte();
  }
  uint32_t Byte() { return pos < size ? data[pos++] : 0; }
  void Normalize() {
    while (range < arixv::kRangeTop) {
      range <<= 8;
      code = (code << 8) | Byte();
    }
  }
  int DecodeBit(Prob& p) {
    const uint16_t raw = p.raw();
    const uint32_t bound = (range >> arixv::kProbBits) * Prob::ValueFromRaw(raw);
    int bit = 0;
    if (code < bound) {
      range = bound;
    } else {
      code -= bound;
      range -= bound;
      bit = 1;
    }
    p.UpdateFromRaw(raw, bit);
    Normalize();
    return bit;
  }
  int DecodeSym(Cdf& t) {
    const uint32_t r = range >> 15;
    uint32_t v = code / r;
    if (v > 32767) v = 32767;
    const int s = Search(t, v);
    const uint32_t lo = CodeC(t, s);
    code -= lo * r;
    range = s + 1 < t.n ? (CodeC(t, s + 1) - lo) * r : range - lo * r;
    Normalize();
    Adapt(t, s);
    return s;
  }
};

// ---- per-module layouts ------------------------------------------------------

void Layout(Module& m) {
  m.bin_offset.resize(m.tree_bits.size());
  m.nib_offset.resize(m.tree_bits.size());
  for (size_t t = 0; t < m.tree_bits.size(); ++t) {
    m.bin_offset[t] = m.bin_size;
    m.bin_size += (1u << m.tree_bits[t]) - 1u;
    m.nib_offset[t] = m.nib_tables;
    m.nib_tables += NibbleTables(m.tree_bits[t]);
  }
}

void InitNibbleTables(const Module& m, std::vector<Cdf>& tables) {
  tables.resize(m.nib_tables + 1);
  for (size_t t = 0; t < m.tree_bits.size(); ++t) {
    const int bits = m.tree_bits[t];
    uint32_t base = m.nib_offset[t];
    int prefix = 0;
    int chunk = FirstChunk(bits);
    while (prefix < bits) {
      for (uint32_t i = 0; i < (1u << prefix); ++i) InitCdf(tables[base + i], 1 << chunk);
      base += 1u << prefix;
      prefix += chunk;
      chunk = 4;
    }
  }
  InitCdf(tables[m.nib_tables], 2);  // spare slot for an off-by-dep index
}

// Walks a tree's chunks: calls sym(table, chunk_bits, chunk_value) and
// returns the value (decoders pass chunk_value = -1 and return the symbol).
template <class F>
uint32_t WalkNibbles(uint32_t base, int bits, uint32_t value, F&& sym) {
  uint32_t prefix_value = 0;
  int prefix = 0;
  int chunk = FirstChunk(bits);
  while (prefix < bits) {
    const int shift = bits - prefix - chunk;
    const uint32_t want = (value >> shift) & ((1u << chunk) - 1u);
    const uint32_t got = sym(base + prefix_value, want);
    prefix_value = (prefix_value << chunk) | got;
    base += 1u << prefix;
    prefix += chunk;
    chunk = 4;
  }
  return prefix_value;
}

// ---- encoders ----------------------------------------------------------------

void EncodeBinary(Module& m) {
  RangeEnc enc(m.bin_payload);
  std::vector<Prob> probs(m.bin_size);
  for (const Event& e : m.events) {
    const int bits = m.tree_bits[e.tree];
    Prob* p = probs.data() + m.bin_offset[e.tree];
    uint32_t node = 1;
    for (int i = bits - 1; i >= 0; --i) {
      const int bit = static_cast<int>((e.value >> i) & 1u);
      enc.EncodeBit(p[node - 1], bit);
      node = (node << 1) | static_cast<uint32_t>(bit);
    }
  }
  enc.Flush();
}

void EncodeNibbleRange(Module& m) {
  RangeEnc enc(m.nib_range_payload);
  std::vector<Cdf> tables;
  InitNibbleTables(m, tables);
  for (const Event& e : m.events) {
    WalkNibbles(m.nib_offset[e.tree], m.tree_bits[e.tree], e.value,
                [&](uint32_t t, uint32_t s) {
                  enc.EncodeSym(tables[t], static_cast<int>(s));
                  Adapt(tables[t], static_cast<int>(s));
                  return s;
                });
  }
  enc.Flush();
}

constexpr uint32_t kRansL = 1u << 16;

void EncodeNibbleRans(Module& m) {
  // Forward pass records (lo, freq) per symbol; then encode in reverse.
  std::vector<Cdf> tables;
  InitNibbleTables(m, tables);
  std::vector<std::pair<uint16_t, uint16_t>> syms;
  for (const Event& e : m.events) {
    WalkNibbles(m.nib_offset[e.tree], m.tree_bits[e.tree], e.value,
                [&](uint32_t t, uint32_t s) {
                  const Cdf& c = tables[t];
                  const uint32_t lo = CodeC(c, static_cast<int>(s));
                  const uint32_t hi = CodeC(c, static_cast<int>(s) + 1);
                  syms.push_back({static_cast<uint16_t>(lo),
                                  static_cast<uint16_t>(hi - lo)});
                  Adapt(tables[t], static_cast<int>(s));
                  return s;
                });
  }
  std::vector<uint16_t> out;
  uint32_t x = kRansL;
  for (size_t i = syms.size(); i-- > 0;) {
    const uint32_t lo = syms[i].first;
    const uint32_t f = syms[i].second;
    if (x >= ((kRansL >> 15) << 16) * f) {
      out.push_back(static_cast<uint16_t>(x));
      x >>= 16;
    }
    x = ((x / f) << 15) + (x % f) + lo;
  }
  out.push_back(static_cast<uint16_t>(x));
  out.push_back(static_cast<uint16_t>(x >> 16));
  m.nib_rans_payload.assign(out.rbegin(), out.rend());
}

// ---- decoders ----------------------------------------------------------------

template <bool kDep>
uint32_t DecodeBinary(const Module& m, std::vector<Prob>& probs) {
  probs.assign(m.bin_size + 1, Prob{});
  RangeDec dec(m.bin_payload.data(), m.bin_payload.size());
  uint32_t errors = 0, prev = 0, traced_prev = 0;
  for (const Event& e : m.events) {
    const int bits = m.tree_bits[e.tree];
    Prob* p = probs.data() + m.bin_offset[e.tree] + (kDep ? static_cast<uint32_t>(prev != traced_prev) : 0u);
    uint32_t node = 1;
    for (int i = 0; i < bits; ++i) {
      node = (node << 1) | static_cast<uint32_t>(dec.DecodeBit(p[node - 1]));
    }
    const uint32_t v = node - (1u << bits);
    errors += v != e.value;
    prev = v;
    traced_prev = e.value;
  }
  return errors;
}

template <bool kDep>
uint32_t DecodeNibbleRange(const Module& m, std::vector<Cdf>& tables) {
  tables = m.nib_init;
  RangeDec dec(m.nib_range_payload.data(), m.nib_range_payload.size());
  uint32_t errors = 0, prev = 0, traced_prev = 0;
  for (const Event& e : m.events) {
    const uint32_t base = m.nib_offset[e.tree] + (kDep ? static_cast<uint32_t>(prev != traced_prev) : 0u);
    const uint32_t v = WalkNibbles(base, m.tree_bits[e.tree], 0,
                                   [&](uint32_t t, uint32_t) {
                                     return static_cast<uint32_t>(dec.DecodeSym(tables[t]));
                                   });
    errors += v != e.value;
    prev = v;
    traced_prev = e.value;
  }
  return errors;
}

template <bool kDep>
uint32_t DecodeNibbleRans(const Module& m, std::vector<Cdf>& tables) {
  tables = m.nib_init;
  const uint16_t* in = m.nib_rans_payload.data();
  const uint16_t* end = in + m.nib_rans_payload.size();
  uint32_t x = static_cast<uint32_t>(in[0]) << 16 | in[1];
  in += 2;
  uint32_t errors = 0, prev = 0, traced_prev = 0;
  for (const Event& e : m.events) {
    const uint32_t base = m.nib_offset[e.tree] + (kDep ? static_cast<uint32_t>(prev != traced_prev) : 0u);
    const uint32_t v = WalkNibbles(
        base, m.tree_bits[e.tree], 0, [&](uint32_t ti, uint32_t) {
          Cdf& t = tables[ti];
          const uint32_t slot = x & 32767u;
          const int s = Search(t, slot);
          const uint32_t lo = CodeC(t, s);
          const uint32_t f = CodeC(t, s + 1) - lo;
          x = f * (x >> 15) + slot - lo;
          if (x < kRansL) x = (x << 16) | (in < end ? *in++ : 0);
          Adapt(t, s);
          return static_cast<uint32_t>(s);
        });
    errors += v != e.value;
    prev = v;
    traced_prev = e.value;
  }
  return errors;
}

// Tight form: chunking resolved at compile time per tree depth.
template <int kBits, class Sym>
inline uint32_t WalkFixed(uint32_t base, Sym&& sym) {
  constexpr int kFirst = kBits % 4 == 0 ? 4 : kBits % 4;
  uint32_t v = sym(base, kFirst);
  base += 1;
  if constexpr (kBits > kFirst) {
    v = (v << 4) | sym(base + v, 4);
    base += 1u << kFirst;
  }
  if constexpr (kBits > kFirst + 4) {
    v = (v << 4) | sym(base + v, 4);
    base += 1u << (kFirst + 4);
  }
  if constexpr (kBits > kFirst + 8) {
    v = (v << 4) | sym(base + v, 4);
  }
  return v;
}

template <bool kDep>
uint32_t DecodeNibbleRansTight(const Module& m, std::vector<Cdf>& tables) {
  tables = m.nib_init;
  const uint16_t* in = m.nib_rans_payload.data();
  const uint16_t* end = in + m.nib_rans_payload.size();
  uint32_t x = static_cast<uint32_t>(in[0]) << 16 | in[1];
  in += 2;
  Cdf* tab = tables.data();
  const auto sym = [&](uint32_t ti, int) -> uint32_t {
    Cdf& t = tab[ti];
    const uint32_t slot = x & 32767u;
    const int s = Search(t, slot);
    const uint32_t lo = t.c[s];
    const uint32_t f = t.c[s + 1] - lo;
    x = f * (x >> 15) + slot - lo;
    if (x < kRansL) x = (x << 16) | (in < end ? *in++ : 0);
    Adapt(t, s);
    return static_cast<uint32_t>(s);
  };
  uint32_t errors = 0, prev = 0, traced_prev = 0;
  for (const Event& e : m.events) {
    const uint32_t base = m.nib_offset[e.tree] + (kDep ? static_cast<uint32_t>(prev != traced_prev) : 0u);
    uint32_t v;
    switch (m.tree_bits[e.tree]) {
      case 4: v = WalkFixed<4>(base, sym); break;
      case 6: v = WalkFixed<6>(base, sym); break;
      case 8: v = WalkFixed<8>(base, sym); break;
      case 10: v = WalkFixed<10>(base, sym); break;
      default:
        v = WalkNibbles(base, m.tree_bits[e.tree], 0,
                        [&](uint32_t t, uint32_t) { return sym(t, 0); });
    }
    errors += v != e.value;
    prev = v;
    traced_prev = e.value;
  }
  return errors;
}

template <class State, class F>
double Time(std::vector<Module>& mods, size_t denom, int iters, F&& decode) {
  State state;
  uint32_t errors = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    for (const Module& m : mods) errors += decode(m, state);
  }
  const auto end = std::chrono::steady_clock::now();
  if (errors != 0) {
    std::fprintf(stderr, "decode mismatch: %u errors\n", errors);
    std::exit(1);
  }
  return std::chrono::duration<double, std::nano>(end - start).count() /
         (static_cast<double>(denom) * iters);
}

}  // namespace

int main(int argc, char** argv) {
  int iters = 10;
  bool sizes_only = false;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--iters=", 0) == 0) {
      iters = std::atoi(arg.c_str() + 8);
    } else if (arg.rfind("--rate-cap=", 0) == 0) {
      g_rate_cap = std::atoi(arg.c_str() + 11);
    } else if (arg == "--sizes-only") {
      sizes_only = true;
    } else {
      paths.push_back(arg);
    }
  }

  std::vector<Module> mods;
  size_t events = 0, bin_decisions = 0, nib_symbols = 0;
  size_t bin_bytes = 0, nib_range_bytes = 0, nib_rans_bytes = 0;
  size_t by_bits_events[17] = {};
  std::vector<arixv::TracedTree> raw;
  for (const std::string& path : paths) {
    std::vector<uint8_t> input;
    if (!ReadFile(path, input)) {
      std::fprintf(stderr, "failed to read %s\n", path.c_str());
      return 1;
    }
    raw.clear();
    std::vector<uint8_t> scratch(arixv_encode_bound(input.size(), nullptr));
    size_t scratch_size = scratch.size();
    arixv::g_tree_trace = &raw;
    (void)arixv_encode(input.data(), input.size(), scratch.data(),
                       &scratch_size, nullptr);
    arixv::g_tree_trace = nullptr;
    Module m;
    std::unordered_map<const void*, uint32_t> ids;
    for (const arixv::TracedTree& t : raw) {
      auto [it, added] = ids.emplace(t.tree, static_cast<uint32_t>(ids.size()));
      if (added) m.tree_bits.push_back(t.bits);
      m.events.push_back({it->second, t.value});
      bin_decisions += static_cast<size_t>(t.bits);
      nib_symbols += static_cast<size_t>((t.bits + 3) / 4);
      ++by_bits_events[t.bits];
    }
    Layout(m);
    InitNibbleTables(m, m.nib_init);
    EncodeBinary(m);
    EncodeNibbleRange(m);
    EncodeNibbleRans(m);
    events += m.events.size();
    bin_bytes += m.bin_payload.size();
    nib_range_bytes += m.nib_range_payload.size();
    nib_rans_bytes += m.nib_rans_payload.size() * 2;
    mods.push_back(std::move(m));
  }

  std::printf("modules=%zu tree_events=%zu binary_decisions=%zu nibble_symbols=%zu "
              "rate_cap=%d\n",
              mods.size(), events, bin_decisions, nib_symbols, g_rate_cap);
  std::printf("events by depth:");
  for (int b = 1; b <= 16; ++b) {
    if (by_bits_events[b]) std::printf(" %d:%zu", b, by_bits_events[b]);
  }
  std::printf("\npayload bytes: binary=%zu nib-range=%zu (%+.2f%%) nib-rans=%zu (%+.2f%%)\n",
              bin_bytes, nib_range_bytes,
              100.0 * (static_cast<double>(nib_range_bytes) / bin_bytes - 1),
              nib_rans_bytes,
              100.0 * (static_cast<double>(nib_rans_bytes) / bin_bytes - 1));
  size_t bin_state = 0, nib_state = 0;
  for (const Module& m : mods) {
    bin_state += m.bin_size * sizeof(Prob);
    nib_state += m.nib_tables * sizeof(Cdf);
  }
  std::printf("state bytes reset per corpus pass: binary=%zu nibble=%zu\n",
              bin_state, nib_state);
  if (sizes_only) return 0;

  const auto report = [&](const char* name, double ns) {
    std::printf("  %-14s %7.2f ns/tree-event\n", name, ns);
  };
  using P = std::vector<Prob>;
  using C = std::vector<Cdf>;
  report("binary-reset", Time<P>(mods, events, iters, [](const Module& m, P& p) {
    p.assign(m.bin_size + 1, Prob{});
    return static_cast<uint32_t>(p[0].raw() != arixv::kProbInit);
  }));
  report("nib-reset", Time<C>(mods, events, iters, [](const Module& m, C& t) {
    t = m.nib_init;
    return static_cast<uint32_t>(t[0].n == 0);
  }));
  report("binary-indep", Time<P>(mods, events, iters, DecodeBinary<false>));
  report("binary-dep", Time<P>(mods, events, iters, DecodeBinary<true>));
  report("nib-range-ind", Time<C>(mods, events, iters, DecodeNibbleRange<false>));
  report("nib-range-dep", Time<C>(mods, events, iters, DecodeNibbleRange<true>));
  report("nib-rans-ind", Time<C>(mods, events, iters, DecodeNibbleRans<false>));
  report("nib-rans-dep", Time<C>(mods, events, iters, DecodeNibbleRans<true>));
  report("nib-tight-ind", Time<C>(mods, events, iters, DecodeNibbleRansTight<false>));
  report("nib-tight-dep", Time<C>(mods, events, iters, DecodeNibbleRansTight<true>));
  return 0;
}
