// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// E-A0 (docs/development/performance.md): bounds what replacing the binary
// range coder with adaptive binary rANS could buy, before touching the model.
//
// The corpus is encoded once with the real model while recording every
// adaptive decision as (context, bit). That trace is then replayed — same
// contexts, same adaptive Prob updates, fresh contexts per module — through:
//   range      the LZMA-style binary range decoder (state in locals, as E1)
//   rans       adaptive binary rANS, 32-bit state, 16-bit renormalization
//   rans2      the same with two interleaved states in one stream
// Each runs in two shapes: "indep", where the next context is known without
// the decoded bit (coder-throughput bound), and "dep", where the next
// context index carries a true data dependency on the decoded bit, as in a
// bit tree or a model branch (latency bound — the realistic case).
//
// Not replayed: TrainBitTree shadow updates, so probabilities differ slightly
// from M2; this affects range and rANS equally.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "arixv.h"
#include "prob.h"
#include "range_coder.h"

#ifndef ARIXV_TRACE_DECISIONS
#error "build with -DARIXV_TRACE_DECISIONS"
#endif

using arixv::Prob;

namespace {

struct Decision {
  uint32_t ctx;
  uint32_t bit;
};

struct Module {
  std::vector<Decision> trace;
  uint32_t num_ctx = 0;
  std::vector<uint8_t> range_payload;
  std::vector<uint16_t> rans_payload;   // in decoder read order
  std::vector<uint16_t> rans2_payload;  // in decoder read order
};

bool ReadFile(const std::string& path, std::vector<uint8_t>& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  data.assign(std::istreambuf_iterator<char>(in), {});
  return true;
}

// ---- rANS (binary, adaptive) -----------------------------------------------

constexpr uint32_t kRansL = 1u << 16;
constexpr uint32_t kScaleBits = arixv::kProbBits;  // 12
constexpr uint32_t kScale = 1u << kScaleBits;

// Probability of a 0 bit for every decision, from a forward model pass.
std::vector<uint16_t> ForwardP0(const Module& m) {
  std::vector<Prob> probs(m.num_ctx);
  std::vector<uint16_t> p0(m.trace.size());
  for (size_t i = 0; i < m.trace.size(); ++i) {
    Prob& p = probs[m.trace[i].ctx];
    p0[i] = p.value();
    p.Update(static_cast<int>(m.trace[i].bit));
  }
  return p0;
}

template <int kStates>
std::vector<uint16_t> RansEncode(const Module& m) {
  const std::vector<uint16_t> p0 = ForwardP0(m);
  std::vector<uint16_t> out;
  uint32_t x[kStates];
  for (int s = 0; s < kStates; ++s) x[s] = kRansL;
  for (size_t i = m.trace.size(); i-- > 0;) {
    uint32_t& st = x[i % kStates];
    const uint32_t bit = m.trace[i].bit;
    const uint32_t f = bit ? kScale - p0[i] : p0[i];
    const uint32_t c = bit ? p0[i] : 0;
    if (st >= ((kRansL >> kScaleBits) << 16) * f) {
      out.push_back(static_cast<uint16_t>(st));
      st >>= 16;
    }
    st = ((st / f) << kScaleBits) + (st % f) + c;
  }
  for (int s = kStates - 1; s >= 0; --s) {
    out.push_back(static_cast<uint16_t>(x[s]));
    out.push_back(static_cast<uint16_t>(x[s] >> 16));
  }
  return std::vector<uint16_t>(out.rbegin(), out.rend());
}

// ---- range coder ------------------------------------------------------------

std::vector<uint8_t> RangeEncode(const Module& m) {
  std::vector<uint8_t> out;
  arixv::RangeEncoder enc(out);
  std::vector<Prob> probs(m.num_ctx);
  for (const Decision& d : m.trace) {
    enc.EncodeBit(probs[d.ctx], static_cast<int>(d.bit));
  }
  enc.Flush();
  return out;
}

// ---- decoders ---------------------------------------------------------------
// kDep: next context = trace ctx + (decoded_prev ^ traced_prev), which is the
// same index but cannot be computed before the previous bit is decoded.

template <bool kDep>
uint32_t RangeDecode(const Module& m, std::vector<Prob>& probs) {
  const uint8_t* data = m.range_payload.data();
  const size_t size = m.range_payload.size();
  size_t pos = 1;  // dummy first byte
  uint32_t code = 0;
  uint32_t range = 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) code = (code << 8) | (pos < size ? data[pos++] : 0);
  uint32_t errors = 0;
  uint32_t prev = 0;
  uint32_t traced_prev = 0;
  for (const Decision& d : m.trace) {
    const uint32_t ctx = kDep ? d.ctx + (prev ^ traced_prev) : d.ctx;
    Prob& p = probs[ctx];
    const uint16_t raw = p.raw();
    const uint32_t bound = (range >> kScaleBits) * Prob::ValueFromRaw(raw);
    uint32_t bit;
    if (code < bound) {
      range = bound;
      bit = 0;
    } else {
      code -= bound;
      range -= bound;
      bit = 1;
    }
    p.UpdateFromRaw(raw, static_cast<int>(bit));
    while (range < arixv::kRangeTop) {
      range <<= 8;
      code = (code << 8) | (pos < size ? data[pos++] : 0);
    }
    errors += bit ^ d.bit;
    prev = bit;
    traced_prev = d.bit;
  }
  return errors;
}

template <int kStates, bool kDep>
uint32_t RansDecode(const Module& m, const std::vector<uint16_t>& payload,
                    std::vector<Prob>& probs) {
  const uint16_t* in = payload.data();
  const uint16_t* end = in + payload.size();
  uint32_t x[kStates];
  for (int s = 0; s < kStates; ++s) {
    x[s] = static_cast<uint32_t>(in[0]) << 16 | in[1];
    in += 2;
  }
  uint32_t errors = 0;
  uint32_t prev = 0;
  uint32_t traced_prev = 0;
  for (size_t i = 0; i < m.trace.size(); ++i) {
    const Decision& d = m.trace[i];
    const uint32_t ctx = kDep ? d.ctx + (prev ^ traced_prev) : d.ctx;
    Prob& p = probs[ctx];
    const uint16_t raw = p.raw();
    const uint32_t p0 = Prob::ValueFromRaw(raw);
    uint32_t& st = x[kStates == 1 ? 0 : i % kStates];
    const uint32_t slot = st & (kScale - 1);
    const uint32_t bit = slot >= p0;
    const uint32_t f = bit ? kScale - p0 : p0;
    const uint32_t c = bit ? p0 : 0;
    st = f * (st >> kScaleBits) + slot - c;
    if (st < kRansL) {
      st = (st << 16) | (in < end ? *in++ : 0);
    }
    p.UpdateFromRaw(raw, static_cast<int>(bit));
    errors += bit ^ d.bit;
    prev = bit;
    traced_prev = d.bit;
  }
  return errors;
}

template <class F>
double TimeNsPerDecision(std::vector<Module>& mods, size_t decisions, int iters,
                         F&& decode) {
  uint32_t errors = 0;
  std::vector<Prob> probs;
  const auto start = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    for (Module& m : mods) {
      probs.assign(m.num_ctx + 1, Prob{});
      errors += decode(m, probs);
    }
  }
  const auto end = std::chrono::steady_clock::now();
  if (errors != 0) {
    std::fprintf(stderr, "decode mismatch: %u errors\n", errors);
    std::exit(1);
  }
  const double ns =
      std::chrono::duration<double, std::nano>(end - start).count();
  return ns / (static_cast<double>(decisions) * iters);
}

}  // namespace

int main(int argc, char** argv) {
  int iters = 20;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--iters=", 0) == 0) {
      iters = std::atoi(arg.c_str() + 8);
    } else {
      paths.push_back(arg);
    }
  }

  std::vector<Module> mods;
  size_t decisions = 0;
  size_t range_bytes = 0, rans_bytes = 0, rans2_bytes = 0;
  std::vector<arixv::TracedDecision> raw_trace;
  for (const std::string& path : paths) {
    std::vector<uint8_t> input;
    if (!ReadFile(path, input)) {
      std::fprintf(stderr, "failed to read %s\n", path.c_str());
      return 1;
    }
    raw_trace.clear();
    arixv::g_decision_trace = &raw_trace;
    (void)arixv::Encode(input.data(), input.size());
    arixv::g_decision_trace = nullptr;
    Module m;
    std::unordered_map<const Prob*, uint32_t> ids;
    m.trace.reserve(raw_trace.size());
    for (const arixv::TracedDecision& t : raw_trace) {
      auto it = ids.emplace(t.ctx, static_cast<uint32_t>(ids.size())).first;
      m.trace.push_back({it->second, t.bit ? 1u : 0u});
    }
    m.num_ctx = static_cast<uint32_t>(ids.size());
    m.range_payload = RangeEncode(m);
    m.rans_payload = RansEncode<1>(m);
    m.rans2_payload = RansEncode<2>(m);
    decisions += m.trace.size();
    range_bytes += m.range_payload.size();
    rans_bytes += m.rans_payload.size() * 2;
    rans2_bytes += m.rans2_payload.size() * 2;
    mods.push_back(std::move(m));
  }

  std::printf("modules=%zu decisions=%zu iters=%d\n", mods.size(), decisions,
              iters);
  std::printf("payload bytes: range=%zu rans=%zu (%+.2f%%) rans2=%zu (%+.2f%%)\n",
              range_bytes, rans_bytes,
              100.0 * (static_cast<double>(rans_bytes) / range_bytes - 1),
              rans2_bytes,
              100.0 * (static_cast<double>(rans2_bytes) / range_bytes - 1));

  const auto report = [&](const char* name, auto&& fn) {
    const double ns = TimeNsPerDecision(mods, decisions, iters, fn);
    std::printf("  %-12s %6.3f ns/decision\n", name, ns);
  };
  report("range-indep", [](const Module& m, std::vector<Prob>& p) {
    return RangeDecode<false>(m, p);
  });
  report("range-dep", [](const Module& m, std::vector<Prob>& p) {
    return RangeDecode<true>(m, p);
  });
  report("rans-indep", [](const Module& m, std::vector<Prob>& p) {
    return RansDecode<1, false>(m, m.rans_payload, p);
  });
  report("rans-dep", [](const Module& m, std::vector<Prob>& p) {
    return RansDecode<1, true>(m, m.rans_payload, p);
  });
  report("rans2-indep", [](const Module& m, std::vector<Prob>& p) {
    return RansDecode<2, false>(m, m.rans2_payload, p);
  });
  report("rans2-dep", [](const Module& m, std::vector<Prob>& p) {
    return RansDecode<2, true>(m, m.rans2_payload, p);
  });
  return 0;
}
