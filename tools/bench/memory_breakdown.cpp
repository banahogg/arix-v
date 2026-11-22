// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// Per-decode memory breakdown (docs/development/performance.md). Build with
// -DARIXV_MEMSTATS (make build/memory_breakdown). Encodes the given files,
// then decodes each once and reports, per decode: total heap requested
// (malloc-level, glibc-specific __libc_malloc hook), arena block bytes, every
// fixed model table (allocated vs. touched — deliberately uninitialized
// arrays only fault in where written), and every arena object type.
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "arena.h"
#include "arixv/arixv.h"
namespace arixv {
inline namespace v1 {
extern std::map<std::string, std::pair<uint64_t, uint64_t>> g_model_members;
extern uint64_t g_model_decodes;
}
}
extern "C" void* __libc_malloc(size_t);
static bool g_on = false; static uint64_t g_malloc_bytes = 0;
extern "C" void* malloc(size_t n) { if (g_on) g_malloc_bytes += n; return __libc_malloc(n); }
static std::string Short(std::string s) {
  auto p = s.find("T = "); if (p != std::string::npos) s = s.substr(p + 4);
  if (!s.empty() && s.back() == ']') s.pop_back();
  for (const char* junk : {"arixv::", "(anonymous namespace)::"}) for (size_t q; (q = s.find(junk)) != std::string::npos;) s.erase(q, strlen(junk));
  return s;
}
int main(int argc, char** argv) {
  std::vector<std::vector<uint8_t>> enc; size_t out_bytes = 0, max_out = 1;
  for (int i = 1; i < argc; ++i) {
    std::ifstream f(argv[i], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() % 4 || d.size() < 20) continue;
    out_bytes += d.size();
    if (d.size() > max_out) max_out = d.size();
    std::vector<uint8_t> buf(arixv_encode_bound(d.size(), nullptr));
    size_t sz = buf.size();
    if (arixv_encode(d.data(), d.size(), buf.data(), &sz, nullptr) != ARIXV_STATUS_OK) continue;
    buf.resize(sz);
    enc.push_back(std::move(buf));
  }
  arixv::g_arena_objects.clear(); arixv::g_arena_block_bytes = 0;
  arixv::g_model_members.clear(); arixv::g_model_decodes = 0;
  std::vector<uint8_t> out(max_out);
  g_on = true;
  for (auto& e : enc) { size_t sz = out.size(); (void)arixv_decode(e.data(), e.size(), out.data(), &sz, nullptr); }
  g_on = false;
  const double n = arixv::g_model_decodes;
  std::printf("decodes=%.0f  avg output=%.0f B  malloc-level heap/decode=%.0f B  arena blocks/decode=%.0f B\n",
              n, out_bytes / n, g_malloc_bytes / n, arixv::g_arena_block_bytes / n);
  std::vector<std::pair<double, std::string>> rows;
  double tot_alloc = 0, tot_touch = 0;
  for (auto& [k, v] : arixv::g_model_members) { rows.push_back({v.first / n, "model  " + k + "|" + std::to_string(v.second / n)}); tot_alloc += v.first / n; tot_touch += v.second / n; }
  double arena_obj = 0;
  for (auto& [k, v] : arixv::g_arena_objects) { char buf[64]; std::snprintf(buf, sizeof buf, " x%.1f", v.first / n); rows.push_back({v.second / n, "arena  " + Short(k) + buf + "|" + std::to_string(v.second / n)}); arena_obj += v.second / n; }
  std::sort(rows.rbegin(), rows.rend());
  std::printf("%10s %10s  %s\n", "bytes", "touched", "component (per decode)");
  for (auto& [b, name] : rows) { auto bar = name.find('|'); if (b >= 64) std::printf("%10.0f %10.0f  %s\n", b, std::atof(name.substr(bar + 1).c_str()), name.substr(0, bar).c_str()); }
  std::printf("model fixed: %.0f B allocated, %.0f B touched; arena objects: %.0f B (all touched)\n", tot_alloc, tot_touch, arena_obj);
}
