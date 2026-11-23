// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

// arixv_pack_api_bench: measurements of the two-stage shader-pack API
// (ARXP, include/arixv/arixv.h) through the public C entry points.
//
//   arixv_pack_api_bench [--parallelism 1,4,8,16] [--workers 1,4,8,16]
//                        [--runs N] [--budget PCT] [--fast-decode]
//                        [--id ID] -- A.spv ... | @list
//
// Per parallelism setting, one JSON line:
//  - size (pack bytes / original), stage-1/stage-2 block counts;
//  - scratch_size vs stage-1 decoded bytes;
//  - per-call full-header validation cost (arixv_pack_inspect_modules);
//  - per-job peak private heap (a counting global allocator, measured with
//    jobs run serially), worst job, and the worst job's block bytes and
//    stage-1 bytes, plus independent ARX2's peak for its largest module;
//  - full decode wall time per worker count, scheduled as a job system would
//    (reset on the caller, stage 1 jobs, join, stage 2 jobs; index order),
//    against independent ARX2 per module on the same pool. Outputs are
//    verified once outside timing.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "arixv/arixv.h"

// ---- counting allocator (per thread current / peak) -----------------------
namespace {
thread_local int64_t t_cur = 0;
thread_local int64_t t_peak = 0;
constexpr size_t kHdr = 16;
void* TryCountedAlloc(size_t n) noexcept {
  void* p = std::malloc(n + kHdr);
  if (!p) return nullptr;
  std::memcpy(p, &n, sizeof n);
  t_cur += static_cast<int64_t>(n);
  if (t_cur > t_peak) t_peak = t_cur;
  return static_cast<char*>(p) + kHdr;
}
void* CountedAlloc(size_t n) {
  void* p = TryCountedAlloc(n);
  if (!p) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::bad_alloc();
#else
    std::abort();
#endif
  }
  return p;
}
void CountedFree(void* q) {
  if (!q) return;
  char* p = static_cast<char*>(q) - kHdr;
  size_t n;
  std::memcpy(&n, p, sizeof n);
  t_cur -= static_cast<int64_t>(n);
  std::free(p);
}
void ResetPeak() { t_peak = t_cur; }
int64_t PeakSince(int64_t base) { return t_peak - base; }
}  // namespace
void* operator new(size_t n) { return CountedAlloc(n); }
void* operator new[](size_t n) { return CountedAlloc(n); }
void operator delete(void* p) noexcept { CountedFree(p); }
void operator delete[](void* p) noexcept { CountedFree(p); }
void operator delete(void* p, size_t) noexcept { CountedFree(p); }
void operator delete[](void* p, size_t) noexcept { CountedFree(p); }
// Every allocation form must go through the counting header, including the
// nothrow forms (std::stable_sort's temporary buffer uses them).
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  return TryCountedAlloc(n);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  return TryCountedAlloc(n);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { CountedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  CountedFree(p);
}

namespace {

double NowMs() {
  using C = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(
             C::now().time_since_epoch())
      .count();
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f),
              std::istreambuf_iterator<char>());
  return true;
}

std::vector<uint64_t> ParseList(const std::string& s) {
  std::vector<uint64_t> v;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) v.push_back(std::strtoull(tok.c_str(), nullptr, 10));
  }
  return v;
}

// Job-system stand-in (as in arixv_struct_parallel): W-1 threads + caller.
class Pool {
 public:
  explicit Pool(unsigned workers) {
    for (unsigned i = 1; i < workers; ++i) {
      threads_.emplace_back([this] { Loop(); });
    }
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> l(mu_);
      stop_ = true;
      ++gen_;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }
  void Run(size_t n, const std::function<void(size_t)>& task) {
    if (n == 0) return;
    {
      std::lock_guard<std::mutex> l(mu_);
      task_ = &task;
      n_ = n;
      next_.store(0);
      active_ = threads_.size();
      ++gen_;
    }
    cv_.notify_all();
    Work();
    std::unique_lock<std::mutex> l(mu_);
    done_cv_.wait(l, [this] { return active_ == 0; });
    task_ = nullptr;
  }

 private:
  void Work() {
    for (;;) {
      const size_t i = next_.fetch_add(1);
      if (i >= n_) return;
      (*task_)(i);
    }
  }
  void Loop() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [&] { return gen_ != seen; });
        seen = gen_;
        if (stop_) return;
      }
      Work();
      {
        std::lock_guard<std::mutex> l(mu_);
        if (--active_ == 0) done_cv_.notify_one();
      }
    }
  }
  std::vector<std::thread> threads_;
  std::mutex mu_;
  std::condition_variable cv_, done_cv_;
  uint64_t gen_ = 0;
  bool stop_ = false;
  const std::function<void(size_t)>* task_ = nullptr;
  size_t n_ = 0;
  size_t active_ = 0;
  std::atomic<size_t> next_{0};
};

struct Outputs {
  std::vector<std::vector<uint8_t>> data;
  std::vector<void*> ptrs;
  std::vector<size_t> caps;
  arixv_pack_outputs view{};
};
void MakeOutputs(const std::vector<arixv_pack_module_info>& mi, Outputs* o) {
  o->data.resize(mi.size());
  o->ptrs.resize(mi.size());
  o->caps.resize(mi.size());
  for (size_t i = 0; i < mi.size(); ++i) {
    o->data[i].resize(static_cast<size_t>(mi[i].size));
    o->ptrs[i] = o->data[i].data();
    o->caps[i] = o->data[i].size();
  }
  o->view = {sizeof(arixv_pack_outputs), static_cast<uint32_t>(mi.size()),
             o->ptrs.data(), o->caps.data()};
}

std::vector<uint8_t> Arx2Encode(const std::vector<uint8_t>& m) {
  size_t n = arixv_encode_bound(m.size(), nullptr);
  std::vector<uint8_t> out(n);
  if (arixv_encode(m.data(), m.size(), out.data(), &n, nullptr) !=
      ARIXV_STATUS_OK) {
    std::abort();
  }
  out.resize(n);
  return out;
}

arixv_status Arx2Decode(const std::vector<uint8_t>& blob,
                        std::vector<uint8_t>& out) {
  size_t n = 0;
  arixv_status s = arixv_decode(blob.data(), blob.size(), nullptr, &n, nullptr);
  if (s != ARIXV_STATUS_OK) return s;
  out.resize(n);
  return arixv_decode(blob.data(), blob.size(), out.data(), &n, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<uint64_t> pars = {1, 4, 8, 16}, workers = {1, 4, 8, 16};
  int runs = 5;
  uint32_t budget = 0;
  uint32_t flags = 0;

  std::string id;
  int i = 1;
  for (; i < argc; ++i) {
    const std::string s = argv[i];
    if (s == "--") {
      ++i;
      break;
    }
    if (s == "--fast-decode") {
      flags |= ARIXV_PACK_ENCODE_FAST_DECODE;
      continue;
    }
    if (i + 1 >= argc) return 2;
    if (s == "--parallelism") pars = ParseList(argv[++i]);
    else if (s == "--workers") workers = ParseList(argv[++i]);
    else if (s == "--runs") runs = std::atoi(argv[++i]);
    else if (s == "--id") id = argv[++i];
    else if (s == "--budget") budget = static_cast<uint32_t>(std::atoi(argv[++i]));
    else return 2;
  }
  std::vector<std::vector<uint8_t>> mods;
  for (; i < argc; ++i) {
    if (argv[i][0] == '@') {
      std::ifstream list(argv[i] + 1);
      std::string line;
      while (std::getline(list, line)) {
        if (line.empty()) continue;
        mods.emplace_back();
        if (!ReadFile(line, &mods.back())) return 2;
      }
      continue;
    }
    mods.emplace_back();
    if (!ReadFile(argv[i], &mods.back())) return 2;
  }
  if (mods.empty()) return 2;
  size_t original = 0, largest = 0, largest_k = 0;
  std::vector<const void*> ptrs;
  std::vector<size_t> sizes;
  for (size_t k = 0; k < mods.size(); ++k) {
    ptrs.push_back(mods[k].empty() ? nullptr : mods[k].data());
    sizes.push_back(mods[k].size());
    original += mods[k].size();
    if (mods[k].size() > largest) {
      largest = mods[k].size();
      largest_k = k;
    }
  }
  // Independent ARX2 baseline: blobs, peak for the largest module, timing.
  std::vector<std::vector<uint8_t>> blobs;
  size_t arx2_bytes = 0;
  for (const auto& m : mods) {
    blobs.push_back(Arx2Encode(m));
    arx2_bytes += blobs.back().size();
  }
  int64_t arx2_peak = 0;
  {
    std::vector<uint8_t> o;
    o.reserve(largest);
    const int64_t base = t_cur;
    ResetPeak();
    (void)Arx2Decode(blobs[largest_k], o);
    arx2_peak = PeakSince(base);
  }
  std::vector<std::unique_ptr<Pool>> pools;
  for (uint64_t w : workers) {
    pools.push_back(std::make_unique<Pool>(static_cast<unsigned>(w)));
  }
  std::vector<double> arx2_ms(workers.size(), 1e300);
  for (int run = 0; run < runs; ++run) {
    for (size_t wi = 0; wi < workers.size(); ++wi) {
      std::vector<std::vector<uint8_t>> outs(mods.size());
      const double t = NowMs();
      pools[wi]->Run(mods.size(), [&](size_t k) {
        (void)Arx2Decode(blobs[k], outs[k]);
      });
      arx2_ms[wi] = std::min(arx2_ms[wi], NowMs() - t);
    }
  }

  for (uint64_t par : pars) {
    const arixv_pack_encode_options opt = {
        sizeof(arixv_pack_encode_options), flags, static_cast<uint32_t>(par),
        budget, 0};
    const uint32_t count = static_cast<uint32_t>(mods.size());
    size_t pack_size = arixv_pack_encode_bound(sizes.data(), count, &opt);
    std::vector<uint8_t> pack(pack_size);
    const double te = NowMs();
    const arixv_status est = arixv_pack_encode(
        ptrs.data(), sizes.data(), count, pack.data(), &pack_size, &opt);
    if (est != ARIXV_STATUS_OK) {
      std::fprintf(stderr, "encode failed: %s\n", arixv_status_string(est));
      return 1;
    }
    const double encode_total_ms = NowMs() - te;
    pack.resize(pack_size);
    arixv_pack_info info{};
    info.struct_size = sizeof(info);
    if (arixv_pack_inspect(pack.data(), pack.size(), &info) != ARIXV_STATUS_OK) {
      return 1;
    }
    std::vector<arixv_pack_module_info> mi(info.module_count);
    arixv_pack_inspect_modules(pack.data(), pack.size(), mi.data(),
                               info.module_count);
    // Validation cost per call (whole-header parse).
    const int vreps = 200;
    double tv = NowMs();
    for (int k = 0; k < vreps; ++k) {
      arixv_pack_inspect_modules(pack.data(), pack.size(), mi.data(),
                                 info.module_count);
    }
    const double validate_us = (NowMs() - tv) * 1000.0 / vreps;
    // Block bytes per job.
    std::vector<uint64_t> s1_bytes(info.stage1_block_count, 0),
        s2_bytes(info.stage2_block_count, 0);
    for (const auto& m : mi) {
      (m.stage == ARIXV_PACK_STAGE_1 ? s1_bytes : s2_bytes)[m.block_index] +=
          m.size;
    }
    uint64_t stage1_bytes = 0, largest_block_bytes = 0;
    for (uint64_t b : s1_bytes) {
      stage1_bytes += b;
      largest_block_bytes = std::max(largest_block_bytes, b);
    }
    for (uint64_t b : s2_bytes) {
      largest_block_bytes = std::max(largest_block_bytes, b);
    }
    std::vector<uint8_t> scratch(static_cast<size_t>(info.scratch_size));
    const size_t ss = scratch.size();
    void* sp = ss ? scratch.data() : nullptr;
    // Serial pass: verification and per-job private peaks.
    Outputs out;
    MakeOutputs(mi, &out);
    bool ok = arixv_pack_reset_scratch(pack.data(), pack.size(), sp, ss) ==
              ARIXV_STATUS_OK;
    int64_t worst_peak = 0;
    uint64_t worst_block = 0;
    double worst_per_block = 0;
    auto measure = [&](arixv_status st, int64_t peak, uint64_t block_bytes) {
      ok = ok && st == ARIXV_STATUS_OK;
      if (peak > worst_peak) {
        worst_peak = peak;
        worst_block = block_bytes;
      }
      if (block_bytes) {
        worst_per_block =
            std::max(worst_per_block, static_cast<double>(peak) / block_bytes);
      }
    };
    std::vector<double> t1(info.stage1_block_count), t2(info.stage2_block_count);
    for (uint32_t b = 0; b < info.stage1_block_count; ++b) {
      const int64_t base = t_cur;
      ResetPeak();
      const double t = NowMs();
      const arixv_status st = arixv_pack_decode_stage1(
          pack.data(), pack.size(), b, &out.view, sp, ss, nullptr);
      t1[b] = NowMs() - t;
      measure(st, PeakSince(base), s1_bytes[b]);
    }
    for (uint32_t b = 0; b < info.stage2_block_count; ++b) {
      const int64_t base = t_cur;
      ResetPeak();
      const double t = NowMs();
      const arixv_status st = arixv_pack_decode_stage2(
          pack.data(), pack.size(), b, &out.view, sp, ss, nullptr);
      t2[b] = NowMs() - t;
      measure(st, PeakSince(base), s2_bytes[b]);
    }
    // Critical-path model from serial job times: each stage is list-
    // scheduled in index order on W workers.
    auto makespan = [](const std::vector<double>& jobs, size_t w) {
      std::vector<double> load(std::max<size_t>(w, 1), 0);
      for (double j : jobs) {
        *std::min_element(load.begin(), load.end()) += j;
      }
      return jobs.empty() ? 0.0 : *std::max_element(load.begin(), load.end());
    };
    double max1 = 0, max2 = 0, sum = 0;
    for (double x : t1) { max1 = std::max(max1, x); sum += x; }
    for (double x : t2) { max2 = std::max(max2, x); sum += x; }
    for (size_t k = 0; ok && k < mods.size(); ++k) ok = out.data[k] == mods[k];
    // Timed: reset, stage 1, join, stage 2 (index order = largest first).
    std::vector<double> ms(workers.size(), 1e300);
    for (int run = 0; ok && run < runs; ++run) {
      for (size_t wi = 0; wi < workers.size(); ++wi) {
        Outputs o;
        MakeOutputs(mi, &o);
        const double t = NowMs();
        arixv_pack_reset_scratch(pack.data(), pack.size(), sp, ss);
        pools[wi]->Run(info.stage1_block_count, [&](size_t b) {
          arixv_pack_decode_stage1(pack.data(), pack.size(),
                                   static_cast<uint32_t>(b), &o.view, sp, ss,
                                   nullptr);
        });
        pools[wi]->Run(info.stage2_block_count, [&](size_t b) {
          arixv_pack_decode_stage2(pack.data(), pack.size(),
                                   static_cast<uint32_t>(b), &o.view, sp, ss,
                                   nullptr);
        });
        ms[wi] = std::min(ms[wi], NowMs() - t);
      }
    }
    std::printf(
        "{\"id\":\"%s\",\"fast_decode\":%d,\"budget\":%u,\"parallelism\":%llu,\"members\":%zu,"
        "\"original_bytes\":%zu,\"arx2_bytes\":%zu,\"pack_bytes\":%zu,"
        "\"stage1_blocks\":%u,\"stage2_blocks\":%u,\"stage1_bytes\":%llu,"
        "\"scratch_size\":%llu,\"largest_block_bytes\":%llu,"
        "\"validate_us\":%.2f,\"worst_job_peak\":%lld,"
        "\"worst_job_block_bytes\":%llu,\"worst_peak_per_block_byte\":%.2f,"
        "\"arx2_largest_module\":%zu,\"arx2_peak\":%lld,"
        "\"encode_ms\":%.1f,\"verified\":%s,\"job_ms_sum\":%.3f,"
        "\"stage1_job_ms_max\":%.3f,\"stage2_job_ms_max\":%.3f,"
        "\"workers\":[",
        id.c_str(), flags ? 1 : 0, budget, static_cast<unsigned long long>(par), mods.size(),
        original, arx2_bytes, pack.size(), info.stage1_block_count,
        info.stage2_block_count,
        static_cast<unsigned long long>(stage1_bytes),
        static_cast<unsigned long long>(info.scratch_size),
        static_cast<unsigned long long>(largest_block_bytes), validate_us,
        static_cast<long long>(worst_peak),
        static_cast<unsigned long long>(worst_block), worst_per_block, largest,
        static_cast<long long>(arx2_peak), encode_total_ms,
        ok ? "true" : "false", sum, max1, max2);
    for (size_t wi = 0; wi < workers.size(); ++wi) {
      std::printf("%s{\"w\":%llu,\"pack_ms\":%.3f,\"arx2_ms\":%.3f,"
                  "\"model_ms\":%.3f}",
                  wi ? "," : "", static_cast<unsigned long long>(workers[wi]),
                  ms[wi], arx2_ms[wi],
                  makespan(t1, workers[wi]) + makespan(t2, workers[wi]));
    }
    std::printf("]}\n");
    if (!ok) return 1;
  }
  return 0;
}
