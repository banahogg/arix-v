// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// RESEARCH ONLY (Track B gate 1b): multi-module sequence size probe.
// See docs/development/multi-module-gate1b.md. Built only with
// -DARIXV_BUILD_RESEARCH_PACK=ON. Prints one JSON object per command on
// stdout. Inputs are explicit ordered file paths; grouping, ordering, and
// prefix selection come from the caller (scripts/bench/multi_module_probe.py
// reads them verbatim from a frozen manifest).
//
// Every reported research-stream size is decoded and byte-compared against
// all members before it is reported as verified; a failed verification is
// reported (and makes the process exit non-zero), never silently dropped.
//
//   arixv_pack_probe info
//   arixv_pack_probe pack [--id ID] [--configs LIST] [--curve] [--self-dup] FILE...
//   arixv_pack_probe case [--id ID] [--configs LIST] --target FILE [--prefix FILE]...
//   arixv_pack_probe emit --config CFG --out OUT FILE...
//   arixv_pack_probe decode STREAM
//
// CFG is <sep|cont>:<none|families>:<carry|reset|mid>, families a subset of
// "oce" (opcode, count, enum). LIST is comma-separated CFGs or "default".

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "arixv/arixv.h"
#include "model.h"
#include "research_pack.h"

namespace {

using arixv::research::AgePolicy;
using arixv::research::EncodedSequence;
using arixv::research::Framing;
using arixv::research::ModuleView;
using arixv::research::SequenceError;
using arixv::research::SequencePolicy;

bool g_failed = false;

struct Config {
  std::string name;
  SequencePolicy policy;
};

bool ParseConfig(const std::string& text, Config* out) {
  const size_t a = text.find(':');
  const size_t b = a == std::string::npos ? a : text.find(':', a + 1);
  if (b == std::string::npos) return false;
  const std::string f = text.substr(0, a);
  const std::string fam = text.substr(a + 1, b - a - 1);
  const std::string age = text.substr(b + 1);
  SequencePolicy p;
  if (f == "sep") {
    p.framing = Framing::kSeparate;
  } else if (f == "cont") {
    p.framing = Framing::kContinuous;
  } else {
    return false;
  }
  p.families = 0;
  if (fam != "none") {
    for (char c : fam) {
      if (c == 'o') p.families |= arixv::research::kFamilyOpcode;
      else if (c == 'c') p.families |= arixv::research::kFamilyCount;
      else if (c == 'e') p.families |= arixv::research::kFamilyEnum;
      else return false;
    }
    if (p.families == 0) return false;
  }
  if (age == "carry") p.age = AgePolicy::kCarry;
  else if (age == "reset") p.age = AgePolicy::kReset;
  else if (age == "mid") p.age = AgePolicy::kIntermediate;
  else return false;
  if (p.families == 0 && p.age != AgePolicy::kCarry) return false;
  out->name = text;
  out->policy = p;
  return true;
}

// Predeclared gate-1b configuration set (order is the report order):
// control 2 (separate, empty transfer), control 3 (continuous, fresh), then
// opcode+count+enum transfer under each age policy, continuous and separate.
const char* const kDefaultConfigs[] = {
    "sep:none:carry",  "cont:none:carry", "cont:oce:carry", "cont:oce:reset",
    "cont:oce:mid",    "sep:oce:carry",   "sep:oce:reset",  "sep:oce:mid",
};

bool ParseConfigList(const std::string& list, std::vector<Config>* out) {
  out->clear();
  if (list == "default") {
    for (const char* c : kDefaultConfigs) {
      Config cfg;
      ParseConfig(c, &cfg);
      out->push_back(cfg);
    }
    return true;
  }
  std::stringstream ss(list);
  std::string item;
  while (std::getline(ss, item, ',')) {
    Config cfg;
    if (!ParseConfig(item, &cfg)) return false;
    out->push_back(cfg);
  }
  return !out->empty();
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  data->assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
  return !in.bad();
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

template <typename T>
std::string JsonList(const std::vector<T>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ",";
    out += std::to_string(v[i]);
  }
  return out + "]";
}

const char* Bool(bool b) { return b ? "true" : "false"; }

const char* SeqErrorName(SequenceError e) {
  switch (e) {
    case SequenceError::kOk: return "ok";
    case SequenceError::kEmpty: return "empty";
    case SequenceError::kTooMany: return "too_many_modules";
    case SequenceError::kNotModelEligible: return "not_model_eligible";
    case SequenceError::kTooLarge: return "too_large";
  }
  return "unknown";
}

// --- Baselines (narrow wrapper over the public single-module API) ---------

struct Baseline {
  size_t arx2_standard_bytes = 0;      // arixv::Encode default (mode choice)
  size_t arx2_forced_bytes = 0;        // force_coded ARX2 blob
  size_t structural_payload_bytes = 0; // bare EncodeModule payload
  uint8_t arx2_mode = 0xff;
  bool eligible = false;
  bool verified = false;
  std::vector<uint8_t> structural_payload;
};

bool Arx2RoundTrips(const std::vector<uint8_t>& blob,
                    const std::vector<uint8_t>& original) {
  std::vector<uint8_t> out;
  return arixv::research::Arx2Decode(blob.data(), blob.size(), out) == ARIXV_STATUS_OK &&
         out == original;
}

Baseline RunBaseline(const std::vector<uint8_t>& m) {
  Baseline b;
  const std::vector<uint8_t> std_blob = arixv::research::Arx2Encode(m.data(), m.size());
  const std::vector<uint8_t> forced_blob =
      arixv::research::Arx2Encode(m.data(), m.size(), true);
  b.arx2_standard_bytes = std_blob.size();
  b.arx2_forced_bytes = forced_blob.size();
  b.arx2_mode = std_blob.size() > 4 ? std_blob[4] : 0xff;
  b.verified = Arx2RoundTrips(std_blob, m) && Arx2RoundTrips(forced_blob, m);
  b.eligible = arixv::IsSpirvModelEligible(m.data(), m.size());
  if (b.eligible) {
    arixv::EncodeModule(m.data(), m.size(), b.structural_payload);
    b.structural_payload_bytes = b.structural_payload.size();
  }
  if (!b.verified) g_failed = true;
  return b;
}

// --- Research sequence encode + exact verification -----------------------

struct SeqResult {
  SequenceError error = SequenceError::kOk;
  size_t failed_index = 0;
  EncodedSequence enc;
  bool verified = false;
  std::string decode_status;
};

SeqResult RunSequence(const std::vector<const std::vector<uint8_t>*>& members,
                      const SequencePolicy& policy) {
  SeqResult r;
  std::vector<ModuleView> views;
  for (const auto* m : members) views.push_back({m->data(), m->size()});
  r.error = arixv::research::EncodeSequence(views.data(), views.size(), policy,
                                            &r.enc, &r.failed_index);
  if (r.error != SequenceError::kOk) return r;
  std::vector<std::vector<uint8_t>> decoded;
  SequencePolicy parsed;
  const arixv_status st = arixv::research::DecodeSequence(
      r.enc.stream.data(), r.enc.stream.size(), decoded, &parsed);
  r.decode_status = arixv_status_string(st);
  r.verified = st == ARIXV_STATUS_OK && parsed == policy &&
               decoded.size() == members.size();
  for (size_t k = 0; r.verified && k < members.size(); ++k) {
    r.verified = decoded[k] == *members[k];
  }
  // Cross-check the real 1.0 decoder rejects research streams.
  std::vector<uint8_t> ignored;
  if (arixv::research::Arx2Decode(r.enc.stream.data(), r.enc.stream.size(), ignored) !=
      ARIXV_STATUS_BAD_CONTAINER) {
    r.verified = false;
    r.decode_status = "arx2_decoder_accepted_research_stream";
  }
  if (!r.verified) g_failed = true;
  return r;
}

std::string SeqJson(const Config& cfg, const SeqResult& r) {
  std::string o = "{\"config\":" + JsonString(cfg.name);
  if (r.error != SequenceError::kOk) {
    o += ",\"status\":\"skip\",\"reason\":" +
         JsonString(SeqErrorName(r.error)) +
         ",\"failed_index\":" + std::to_string(r.failed_index) + "}";
    return o;
  }
  o += ",\"status\":\"ok\"";
  o += ",\"stream_bytes\":" + std::to_string(r.enc.stream.size());
  o += ",\"header_bytes\":" + std::to_string(r.enc.header_bytes);
  o += ",\"payload_bytes\":" + std::to_string(r.enc.payload_bytes);
  if (!r.enc.member_payload_bytes.empty()) {
    o += ",\"member_payload_bytes\":" + JsonList(r.enc.member_payload_bytes);
  }
  o += ",\"verified\":" + std::string(Bool(r.verified));
  o += ",\"decode_status\":" + JsonString(r.decode_status) + "}";
  return o;
}

struct Args {
  std::string id;
  std::string configs = "default";
  std::string config;
  std::string out;
  std::string target;
  std::vector<std::string> prefixes;
  std::vector<std::string> files;
  bool curve = false;
  bool self_dup = false;
};

bool ParseArgs(int argc, char** argv, int start, Args* a) {
  for (int i = start; i < argc; ++i) {
    const std::string s = argv[i];
    auto value = [&](std::string* dst) {
      if (i + 1 >= argc) return false;
      *dst = argv[++i];
      return true;
    };
    if (s == "--id") {
      if (!value(&a->id)) return false;
    } else if (s == "--configs") {
      if (!value(&a->configs)) return false;
    } else if (s == "--config") {
      if (!value(&a->config)) return false;
    } else if (s == "--out") {
      if (!value(&a->out)) return false;
    } else if (s == "--target") {
      if (!value(&a->target)) return false;
    } else if (s == "--prefix") {
      std::string p;
      if (!value(&p)) return false;
      a->prefixes.push_back(p);
    } else if (s == "--curve") {
      a->curve = true;
    } else if (s == "--self-dup") {
      a->self_dup = true;
    } else if (s == "--") {
      for (++i; i < argc; ++i) a->files.push_back(argv[i]);
    } else if (s.size() > 1 && s[0] == '-' && s[1] == '-') {
      std::fprintf(stderr, "unknown option %s\n", s.c_str());
      return false;
    } else {
      a->files.push_back(s);
    }
  }
  return true;
}

bool LoadAll(const std::vector<std::string>& paths,
             std::vector<std::vector<uint8_t>>* data) {
  data->resize(paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    if (!ReadFile(paths[i], &(*data)[i])) {
      std::fprintf(stderr, "cannot read %s\n", paths[i].c_str());
      return false;
    }
  }
  return true;
}

std::string BaselineJson(const std::vector<Baseline>& bs) {
  size_t std_total = 0, forced_total = 0, payload_total = 0;
  bool all_verified = true, all_eligible = true;
  std::vector<size_t> std_sizes, payload_sizes;
  std::vector<int> modes;
  for (const Baseline& b : bs) {
    std_total += b.arx2_standard_bytes;
    forced_total += b.arx2_forced_bytes;
    payload_total += b.structural_payload_bytes;
    all_verified = all_verified && b.verified;
    all_eligible = all_eligible && b.eligible;
    std_sizes.push_back(b.arx2_standard_bytes);
    payload_sizes.push_back(b.structural_payload_bytes);
    modes.push_back(b.arx2_mode);
  }
  std::string o = "{\"arx2_standard_bytes\":" + std::to_string(std_total);
  o += ",\"arx2_forced_structural_bytes\":" + std::to_string(forced_total);
  o += ",\"structural_payload_bytes\":" + std::to_string(payload_total);
  o += ",\"member_arx2_standard_bytes\":" + JsonList(std_sizes);
  o += ",\"member_arx2_mode\":" + JsonList(modes);
  o += ",\"member_structural_payload_bytes\":" + JsonList(payload_sizes);
  o += ",\"all_model_eligible\":" + std::string(Bool(all_eligible));
  o += ",\"verified\":" + std::string(Bool(all_verified)) + "}";
  return o;
}

// Control-2 identity: separately framed empty transfer must reproduce every
// existing EncodeModule payload bit-for-bit.
bool EmptyTransferIdentity(const std::vector<Baseline>& bs,
                           const SeqResult& sep_empty) {
  if (sep_empty.error != SequenceError::kOk) return false;
  const auto& stream = sep_empty.enc.stream;
  size_t off = sep_empty.enc.header_bytes;
  for (size_t k = 0; k < bs.size(); ++k) {
    const size_t n = sep_empty.enc.member_payload_bytes[k];
    if (n != bs[k].structural_payload.size() ||
        std::memcmp(stream.data() + off, bs[k].structural_payload.data(), n) !=
            0) {
      return false;
    }
    off += n;
  }
  return off == stream.size();
}

int CmdPack(const Args& a) {
  std::vector<Config> configs;
  if (!ParseConfigList(a.configs, &configs) || a.files.empty()) {
    std::fprintf(stderr, "pack: need FILE... and valid --configs\n");
    return 2;
  }
  std::vector<std::vector<uint8_t>> data;
  if (!LoadAll(a.files, &data)) return 2;
  std::vector<const std::vector<uint8_t>*> members;
  size_t original = 0;
  std::vector<size_t> sizes;
  for (const auto& d : data) {
    members.push_back(&d);
    original += d.size();
    sizes.push_back(d.size());
  }
  std::vector<Baseline> bs;
  for (const auto& d : data) bs.push_back(RunBaseline(d));

  std::string o = "{\"kind\":\"pack\",\"id\":" + JsonString(a.id);
  o += ",\"members\":" + std::to_string(data.size());
  o += ",\"original_bytes\":" + std::to_string(original);
  o += ",\"member_bytes\":" + JsonList(sizes);
  o += ",\"baseline\":" + BaselineJson(bs);
  o += ",\"configs\":[";
  bool identity_checked = false, identity_ok = false;
  for (size_t c = 0; c < configs.size(); ++c) {
    const SeqResult r = RunSequence(members, configs[c].policy);
    if (c) o += ",";
    o += SeqJson(configs[c], r);
    if (configs[c].policy.framing == Framing::kSeparate &&
        configs[c].policy.families == 0 && r.error == SequenceError::kOk) {
      identity_checked = true;
      identity_ok = EmptyTransferIdentity(bs, r);
      if (!identity_ok) g_failed = true;
    }
  }
  o += "]";
  o += ",\"empty_transfer_identity\":" +
       std::string(identity_checked ? Bool(identity_ok) : "null");

  if (a.curve) {
    // Marginal costs from complete, finalized prefix encodes (both coders
    // defer output, so in-flight buffer growth is not a module's cost).
    o += ",\"curve\":[";
    bool first = true;
    for (const Config& cfg : configs) {
      std::vector<size_t> totals;
      bool ok = true;
      for (size_t k = 1; k <= members.size() && ok; ++k) {
        std::vector<const std::vector<uint8_t>*> prefix(members.begin(),
                                                        members.begin() + k);
        const SeqResult r = RunSequence(prefix, cfg.policy);
        ok = r.error == SequenceError::kOk && r.verified;
        if (ok) totals.push_back(r.enc.stream.size());
      }
      if (!first) o += ",";
      first = false;
      o += "{\"config\":" + JsonString(cfg.name) +
           ",\"verified\":" + Bool(ok) + ",\"prefix_stream_bytes\":" +
           JsonList(totals) + "}";
    }
    o += "]";
  }

  if (a.self_dup) {
    // Self-duplicate capability diagnostic: [m] vs [m, m] per member.
    o += ",\"self_dup\":[";
    for (size_t k = 0; k < members.size(); ++k) {
      if (k) o += ",";
      o += "{\"index\":" + std::to_string(k) + ",\"results\":[";
      for (size_t c = 0; c < configs.size(); ++c) {
        const SeqResult one = RunSequence({members[k]}, configs[c].policy);
        const SeqResult two =
            RunSequence({members[k], members[k]}, configs[c].policy);
        if (c) o += ",";
        o += "{\"config\":" + JsonString(configs[c].name);
        if (one.error != SequenceError::kOk) {
          o += ",\"status\":\"skip\",\"reason\":" +
               JsonString(SeqErrorName(one.error)) + "}";
          continue;
        }
        o += ",\"single_stream_bytes\":" +
             std::to_string(one.enc.stream.size()) +
             ",\"double_stream_bytes\":" +
             std::to_string(two.enc.stream.size());
        if (!two.enc.member_payload_bytes.empty()) {
          o += ",\"second_member_payload_bytes\":" +
               std::to_string(two.enc.member_payload_bytes[1]);
        }
        o += ",\"verified\":" +
             std::string(Bool(one.verified && two.verified)) + "}";
      }
      o += "]}";
    }
    o += "]";
  }
  o += ",\"verified_all\":" + std::string(Bool(!g_failed)) + "}";
  std::printf("%s\n", o.c_str());
  return g_failed ? 1 : 0;
}

int CmdCase(const Args& a) {
  std::vector<Config> configs;
  if (!ParseConfigList(a.configs, &configs) || a.target.empty() ||
      !a.files.empty()) {
    std::fprintf(stderr, "case: need --target FILE [--prefix FILE]...\n");
    return 2;
  }
  std::vector<std::vector<uint8_t>> pre;
  std::vector<uint8_t> target;
  if (!LoadAll(a.prefixes, &pre) || !ReadFile(a.target, &target)) return 2;
  std::vector<const std::vector<uint8_t>*> prefix, whole;
  size_t prefix_original = 0;
  for (const auto& p : pre) {
    prefix.push_back(&p);
    whole.push_back(&p);
    prefix_original += p.size();
  }
  whole.push_back(&target);
  const Baseline tb = RunBaseline(target);

  std::string o = "{\"kind\":\"case\",\"id\":" + JsonString(a.id);
  o += ",\"prefix_members\":" + std::to_string(pre.size());
  o += ",\"prefix_original_bytes\":" + std::to_string(prefix_original);
  o += ",\"target_original_bytes\":" + std::to_string(target.size());
  o += ",\"target_baseline\":" + BaselineJson({tb});
  o += ",\"configs\":[";
  for (size_t c = 0; c < configs.size(); ++c) {
    const Config& cfg = configs[c];
    const SeqResult alone = RunSequence({&target}, cfg.policy);
    const SeqResult w = RunSequence(whole, cfg.policy);
    if (c) o += ",";
    o += "{\"config\":" + JsonString(cfg.name);
    if (w.error != SequenceError::kOk || alone.error != SequenceError::kOk) {
      o += ",\"status\":\"skip\",\"reason\":" +
           JsonString(SeqErrorName(w.error != SequenceError::kOk
                                       ? w.error
                                       : alone.error)) +
           ",\"failed_index\":" + std::to_string(w.failed_index) + "}";
      continue;
    }
    bool verified = alone.verified && w.verified;
    o += ",\"status\":\"ok\"";
    o += ",\"target_alone_stream_bytes\":" +
         std::to_string(alone.enc.stream.size());
    o += ",\"whole_stream_bytes\":" + std::to_string(w.enc.stream.size());
    if (!prefix.empty()) {
      const SeqResult p = RunSequence(prefix, cfg.policy);
      verified = verified && p.error == SequenceError::kOk && p.verified;
      o += ",\"prefix_stream_bytes\":" + std::to_string(p.enc.stream.size());
      // Conditional target cost: whole minus finalized prefix stream. Not a
      // deployable total: whole_stream_bytes already charges the prefix.
      o += ",\"conditional_target_bytes\":" +
           std::to_string(static_cast<long long>(w.enc.stream.size()) -
                          static_cast<long long>(p.enc.stream.size()));
    } else {
      o += ",\"prefix_stream_bytes\":0";
      o += ",\"conditional_target_bytes\":" +
           std::to_string(w.enc.stream.size());
    }
    if (!w.enc.member_payload_bytes.empty()) {
      o += ",\"target_member_payload_bytes\":" +
           std::to_string(w.enc.member_payload_bytes.back());
      o += ",\"target_alone_payload_bytes\":" +
           std::to_string(alone.enc.member_payload_bytes.back());
    }
    o += ",\"verified\":" + std::string(Bool(verified)) + "}";
  }
  o += "],\"verified_all\":" + std::string(Bool(!g_failed)) + "}";
  std::printf("%s\n", o.c_str());
  return g_failed ? 1 : 0;
}

int CmdEmit(const Args& a) {
  Config cfg;
  if (!ParseConfig(a.config, &cfg) || a.out.empty() || a.files.empty()) {
    std::fprintf(stderr, "emit: need --config CFG --out OUT FILE...\n");
    return 2;
  }
  std::vector<std::vector<uint8_t>> data;
  if (!LoadAll(a.files, &data)) return 2;
  std::vector<const std::vector<uint8_t>*> members;
  for (const auto& d : data) members.push_back(&d);
  const SeqResult r = RunSequence(members, cfg.policy);
  if (r.error != SequenceError::kOk || !r.verified) {
    std::fprintf(stderr, "emit: %s\n",
                 r.error != SequenceError::kOk ? SeqErrorName(r.error)
                                               : "verification failed");
    return 1;
  }
  std::ofstream out(a.out, std::ios::binary);
  out.write(reinterpret_cast<const char*>(r.enc.stream.data()),
            static_cast<std::streamsize>(r.enc.stream.size()));
  return out ? 0 : 1;
}

int CmdDecode(const Args& a) {
  if (a.files.size() != 1) return 2;
  std::vector<uint8_t> data;
  if (!ReadFile(a.files[0], &data)) return 2;
  std::vector<std::vector<uint8_t>> modules;
  const arixv_status st =
      arixv::research::DecodeSequence(data.data(), data.size(), modules);
  std::printf("{\"kind\":\"decode\",\"status\":%s,\"modules\":%zu}\n",
              JsonString(arixv_status_string(st)).c_str(), modules.size());
  return st == ARIXV_STATUS_OK ? 0 : 1;
}

int CmdInfo() {
  std::string o = "{\"kind\":\"info\",\"research_magic\":\"AXRP\"";
  o += ",\"research_version\":" +
       std::to_string(arixv::research::kResearchVersion);
#if defined(__clang__)
  o += ",\"compiler\":" + JsonString(std::string("clang ") + __clang_version__);
#elif defined(__GNUC__)
  o += ",\"compiler\":" + JsonString(std::string("gcc ") + __VERSION__);
#else
  o += ",\"compiler\":\"unknown\"";
#endif
#ifdef NDEBUG
  o += ",\"ndebug\":true";
#else
  o += ",\"ndebug\":false";
#endif
  o += ",\"tuning\":\"frozen-default\"";
  o += ",\"intermediate_age_caps\":{\"prob\":" +
       std::to_string(arixv::research::kIntermediateProbAge) +
       ",\"cdf\":" + std::to_string(arixv::research::kIntermediateCdfAge) +
       ",\"opcode_order1_count\":" +
       std::to_string(arixv::research::kIntermediateOrder1Count) + "}";
  o += ",\"default_configs\":[";
  for (size_t i = 0; i < sizeof(kDefaultConfigs) / sizeof(*kDefaultConfigs);
       ++i) {
    if (i) o += ",";
    o += JsonString(kDefaultConfigs[i]);
  }
  o += "]}";
  std::printf("%s\n", o.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: arixv_pack_probe info|pack|case|emit|decode ...\n");
    return 2;
  }
  const std::string cmd = argv[1];
  Args a;
  if (!ParseArgs(argc, argv, 2, &a)) return 2;
  if (cmd == "info") return CmdInfo();
  if (cmd == "pack") return CmdPack(a);
  if (cmd == "case") return CmdCase(a);
  if (cmd == "emit") return CmdEmit(a);
  if (cmd == "decode") return CmdDecode(a);
  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
