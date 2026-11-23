#!/usr/bin/env python3
# Copyright (c) 2026, Eric Werness
# SPDX-License-Identifier: BSD-2-Clause
"""RESEARCH ONLY (Track B gate 1b): drive arixv_pack_probe over a frozen
multi-module manifest (contract version 1).

Python stdlib only. Grouping, member order, and prefix cases are consumed
verbatim from the manifest produced by gate 1a; this script never invents
groups, family labels, or substitute prefixes. Unavailable cases stay
unavailable. Every probe result is decoded and byte-compared by the probe
before it is reported as verified.

Subcommands:
  validate   check manifest.ready.json, manifest SHA256, schema, and every
             module's content hash and byte size
  run        validate, then run every pack and prefix case; write
             results.json, packs.csv, cases.csv, summary.txt
  fixtures   run an explicit ordered file list as one diagnostic sequence
             (tool validation only; never a representative pack)
  fuzz-smoke coverage-guided research-path fuzz smoke (libFuzzer harnesses)

See docs/development/multi-module-gate1b.md.
"""

import argparse
import collections
import concurrent.futures
import csv
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time

CONTRACT_VERSION = 1
SCHEMA_VERSION = 1
PACK_TIERS = {"same_collection", "stage_pair", "variant_family", "control",
              "unclassified"}
CASE_TIERS = {"none", "unrelated", "unrelated_toolchain_matched",
              "same_collection", "stage_pair", "variant_family"}
SNAPSHOT_KEYS = ("source_lock_sha256", "manifest_sha256", "duplicates_sha256",
                 "sources_sha256")
MODULE_FIELDS = {
    "path": str, "sha256": str, "byte_size": int, "logical_key": str,
    "origin_id": (str, type(None)), "collection_id": str, "fold_id": str,
    "stage": (str, type(None)), "spirv_version": int, "generator_word": int,
}
DEFAULT_CONFIGS = "default"


class ManifestError(Exception):
    pass


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_hex64(value):
    return (isinstance(value, str) and len(value) == 64 and
            all(c in "0123456789abcdef" for c in value))


def require(cond, msg):
    if not cond:
        raise ManifestError(msg)


def load_ready(ready_path, manifest_bytes):
    require(os.path.isfile(ready_path),
            f"ready marker absent: {ready_path} (manifest not published)")
    with open(ready_path, "rb") as f:
        ready = json.loads(f.read().decode("utf-8"))
    require(isinstance(ready, dict), "ready marker is not an object")
    require(ready.get("contract_version") == CONTRACT_VERSION,
            f"ready contract_version {ready.get('contract_version')!r} != 1")
    digest = sha256_bytes(manifest_bytes)
    require(ready.get("manifest_sha256") == digest,
            f"manifest SHA256 {digest} != ready marker "
            f"{ready.get('manifest_sha256')!r}")
    require(isinstance(ready.get("baseline_identity_verified"), bool),
            "ready marker baseline_identity_verified must be a bool")
    return ready, digest


def validate_manifest(manifest, check_files=True):
    require(isinstance(manifest, dict), "manifest is not an object")
    require(manifest.get("schema_version") == SCHEMA_VERSION,
            f"schema_version {manifest.get('schema_version')!r} != 1")
    snap = manifest.get("snapshot")
    require(isinstance(snap, dict), "snapshot missing")
    for key in SNAPSHOT_KEYS:
        require(is_hex64(snap.get(key)), f"snapshot.{key} is not hex SHA256")
    modules = manifest.get("modules")
    require(isinstance(modules, dict) and modules, "modules missing/empty")
    for mid, m in modules.items():
        require(isinstance(m, dict), f"module {mid} is not an object")
        for field, typ in MODULE_FIELDS.items():
            require(field in m, f"module {mid} lacks {field}")
            require(isinstance(m[field], typ) and
                    not (typ is int and isinstance(m[field], bool)),
                    f"module {mid}.{field} has wrong type")
        require(is_hex64(m["sha256"]), f"module {mid}.sha256 not hex SHA256")
        require(os.path.isabs(m["path"]), f"module {mid}.path not absolute")
        if check_files:
            require(os.path.isfile(m["path"]),
                    f"module {mid} file missing: {m['path']}")
            size = os.path.getsize(m["path"])
            require(size == m["byte_size"],
                    f"module {mid} size {size} != {m['byte_size']}")
            digest = sha256_file(m["path"])
            require(digest == m["sha256"],
                    f"module {mid} SHA256 {digest} != {m['sha256']}")
    packs = manifest.get("packs")
    require(isinstance(packs, list), "packs missing")
    pack_ids = set()
    for p in packs:
        require(isinstance(p, dict), "pack is not an object")
        for field in ("id", "module_ids", "tier", "collection_id", "fold_id",
                      "representative", "evidence"):
            require(field in p, f"pack {p.get('id')!r} lacks {field}")
        require("origin_id" in p, f"pack {p['id']} lacks origin_id")
        require(p["id"] not in pack_ids, f"duplicate pack id {p['id']}")
        pack_ids.add(p["id"])
        require(p["tier"] in PACK_TIERS, f"pack {p['id']} tier {p['tier']!r}")
        require(isinstance(p["representative"], bool),
                f"pack {p['id']} representative not bool")
        require(isinstance(p["module_ids"], list) and p["module_ids"],
                f"pack {p['id']} has no modules")
        for mid in p["module_ids"]:
            require(mid in modules, f"pack {p['id']} unknown module {mid}")
    cases = manifest.get("prefix_cases", [])
    require(isinstance(cases, list), "prefix_cases is not a list")
    case_ids = set()
    for c in cases:
        require(isinstance(c, dict), "prefix case is not an object")
        for field in ("id", "target_id", "prefix_ids", "tier", "availability",
                      "reason", "seed"):
            require(field in c, f"case {c.get('id')!r} lacks {field}")
        require(c["id"] not in case_ids, f"duplicate case id {c['id']}")
        case_ids.add(c["id"])
        require(c["tier"] in CASE_TIERS, f"case {c['id']} tier {c['tier']!r}")
        require(c["availability"] in ("available", "unavailable"),
                f"case {c['id']} availability {c['availability']!r}")
        require(isinstance(c["prefix_ids"], list),
                f"case {c['id']} prefix_ids not a list")
        if c["availability"] == "available":
            require(c["target_id"] in modules,
                    f"case {c['id']} unknown target {c['target_id']}")
            for mid in c["prefix_ids"]:
                require(mid in modules, f"case {c['id']} unknown prefix {mid}")
            if c["tier"] == "none":
                require(not c["prefix_ids"],
                        f"case {c['id']} tier none has prefixes")
        else:
            require(isinstance(c["reason"], str) and c["reason"],
                    f"unavailable case {c['id']} lacks a reason")
    return {"modules": len(modules), "packs": len(packs), "cases": len(cases)}


def check_snapshot_sources(manifest, corpus_dir, source_lock):
    """Optionally re-hash the provenance inputs the snapshot names."""
    snap = manifest["snapshot"]
    out = {}
    if corpus_dir:
        for key, name in (("manifest_sha256", "manifest.tsv"),
                          ("duplicates_sha256", "duplicates.tsv"),
                          ("sources_sha256", "sources.tsv")):
            path = os.path.join(corpus_dir, name)
            ok = os.path.isfile(path) and sha256_file(path) == snap[key]
            out[key] = "match" if ok else "MISMATCH"
    if source_lock:
        ok = (os.path.isfile(source_lock) and
              sha256_file(source_lock) == snap["source_lock_sha256"])
        out["source_lock_sha256"] = "match" if ok else "MISMATCH"
    return out


def load_validated(args):
    with open(args.manifest, "rb") as f:
        manifest_bytes = f.read()
    ready_path = args.ready or os.path.join(
        os.path.dirname(os.path.abspath(args.manifest)), "manifest.ready.json")
    ready, digest = load_ready(ready_path, manifest_bytes)
    manifest = json.loads(manifest_bytes.decode("utf-8"))
    counts = validate_manifest(manifest)
    snapshot_check = check_snapshot_sources(manifest, args.corpus_dir,
                                            args.source_lock)
    if any(v != "match" for v in snapshot_check.values()):
        raise ManifestError(f"snapshot source mismatch: {snapshot_check}")
    return manifest, ready, digest, counts, snapshot_check, ready_path


def run_probe(probe, argv):
    proc = subprocess.run([probe] + argv, capture_output=True, text=True)
    out = proc.stdout.strip()
    try:
        result = json.loads(out) if out else None
    except json.JSONDecodeError:
        result = None
    return proc.returncode, result, proc.stderr.strip()


def tool_info(probe):
    code, info, err = run_probe(probe, ["info"])
    repo = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    def git(*a):
        try:
            return subprocess.run(["git", "-C", repo] + list(a),
                                  capture_output=True, text=True,
                                  check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return None
    status = git("status", "--porcelain")
    return {
        "probe": os.path.abspath(probe),
        "probe_sha256": sha256_file(probe),
        "probe_info": info,
        "python": platform.python_version(),
        "platform": platform.platform(),
        "source_git_head": git("rev-parse", "HEAD"),
        "source_git_dirty_entries": (len(status.splitlines())
                                     if status is not None else None),
        "script_sha256": sha256_file(os.path.abspath(__file__)),
    }


def pack_task(probe, pack, modules, configs, curve, self_dup):
    argv = ["pack", "--id", pack["id"], "--configs", configs]
    if curve:
        argv.append("--curve")
    if self_dup:
        argv.append("--self-dup")
    argv.append("--")
    argv += [modules[mid]["path"] for mid in pack["module_ids"]]
    code, result, err = run_probe(probe, argv)
    return pack, code, result, err


def case_task(probe, case, modules, configs):
    argv = ["case", "--id", case["id"], "--configs", configs,
            "--target", modules[case["target_id"]]["path"]]
    for mid in case["prefix_ids"]:
        argv += ["--prefix", modules[mid]["path"]]
    code, result, err = run_probe(probe, argv)
    return case, code, result, err


def config_bytes(result):
    return {c["config"]: c.get("stream_bytes")
            for c in result.get("configs", []) if c.get("status") == "ok"}


def write_outputs(out_dir, doc):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "results.json"), "w") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
    configs = doc["config_names"]
    with open(os.path.join(out_dir, "packs.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["pack_id", "tier", "representative", "origin_id",
                    "collection_id", "fold_id", "members", "original_bytes",
                    "status", "verified", "empty_transfer_identity",
                    "arx2_standard_bytes", "structural_payload_bytes"] +
                   configs)
        for p in doc["packs"]:
            r = p.get("result") or {}
            cb = config_bytes(r) if r else {}
            base = r.get("baseline", {})
            w.writerow([p["id"], p["tier"], p["representative"],
                        p["origin_id"], p["collection_id"], p["fold_id"],
                        len(p["module_ids"]), r.get("original_bytes"),
                        p["status"], r.get("verified_all"),
                        r.get("empty_transfer_identity"),
                        base.get("arx2_standard_bytes"),
                        base.get("structural_payload_bytes")] +
                       [cb.get(c) for c in configs])
    with open(os.path.join(out_dir, "cases.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["case_id", "tier", "availability", "seed", "target_id",
                    "prefix_members", "prefix_original_bytes", "config",
                    "status", "target_alone_stream_bytes",
                    "prefix_stream_bytes", "whole_stream_bytes",
                    "conditional_target_bytes", "target_member_payload_bytes",
                    "verified"])
        for c in doc["cases"]:
            r = c.get("result")
            if not r:
                w.writerow([c["id"], c["tier"], c["availability"], c["seed"],
                            c["target_id"], len(c["prefix_ids"]), None, None,
                            c["status"]] + [None] * 6)
                continue
            for cfg in r["configs"]:
                w.writerow([c["id"], c["tier"], c["availability"], c["seed"],
                            c["target_id"], r["prefix_members"],
                            r["prefix_original_bytes"], cfg["config"],
                            cfg.get("status"),
                            cfg.get("target_alone_stream_bytes"),
                            cfg.get("prefix_stream_bytes"),
                            cfg.get("whole_stream_bytes"),
                            cfg.get("conditional_target_bytes"),
                            cfg.get("target_member_payload_bytes"),
                            cfg.get("verified")])
    with open(os.path.join(out_dir, "summary.txt"), "w") as f:
        f.write(summarize(doc))


def summarize(doc):
    lines = [f"manifest_sha256 {doc['manifest_sha256']}",
             f"representative status per pack is copied from the manifest; "
             f"non-representative rows are diagnostics", ""]
    configs = doc["config_names"]
    groups = {}
    for p in doc["packs"]:
        r = p.get("result")
        key = (p["representative"], p["tier"])
        g = groups.setdefault(key, {"packs": 0, "ok": 0, "skipped": 0,
                                    "original": 0, "arx2": 0,
                                    "cfg": {c: 0 for c in configs}})
        g["packs"] += 1
        if p["status"] != "ok" or not r:
            g["skipped"] += 1
            continue
        cb = config_bytes(r)
        if any(cb.get(c) is None for c in configs):
            g["skipped"] += 1
            continue
        g["ok"] += 1
        g["original"] += r["original_bytes"]
        g["arx2"] += r["baseline"]["arx2_standard_bytes"]
        for c in configs:
            g["cfg"][c] += cb[c]
    for (rep, tier), g in sorted(groups.items(), key=lambda kv: (
            not kv[0][0], kv[0][1])):
        lines.append(f"[{'representative' if rep else 'diagnostic'} / {tier}] "
                     f"packs={g['packs']} measured={g['ok']} "
                     f"skipped={g['skipped']} original={g['original']} "
                     f"arx2_standard={g['arx2']}")
        for c in configs:
            v = g["cfg"][c]
            rel = (f"{100.0 * (v - g['arx2']) / g['arx2']:+.2f}% vs arx2"
                   if g["arx2"] else "n/a")
            lines.append(f"    {c:18s} {v:12d}  {rel}")
    lines.append("")
    avail = sum(1 for c in doc["cases"] if c["availability"] == "available")
    lines.append(f"prefix cases: {len(doc['cases'])} total, {avail} available,"
                 f" {len(doc['cases']) - avail} unavailable (kept unavailable)")
    failures = [p["id"] for p in doc["packs"] if p["status"] == "error"]
    failures += [c["id"] for c in doc["cases"] if c["status"] == "error"]
    lines.append(f"verification failures: {len(failures)} {failures[:20]}")
    return "\n".join(lines) + "\n"


def cmd_validate(args):
    try:
        manifest, ready, digest, counts, snap, ready_path = load_validated(args)
    except (ManifestError, OSError, json.JSONDecodeError) as e:
        print(f"invalid: {e}", file=sys.stderr)
        return 3
    print(json.dumps({"manifest_sha256": digest, "ready": ready_path,
                      "counts": counts, "snapshot_sources": snap,
                      "baseline_identity_verified":
                          ready["baseline_identity_verified"]}, indent=1))
    return 0


def resolve_configs(probe, configs):
    if configs != "default":
        return configs.split(",")
    _, info, _ = run_probe(probe, ["info"])
    return info["default_configs"]


def cmd_run(args):
    try:
        manifest, ready, digest, counts, snap, ready_path = load_validated(args)
    except (ManifestError, OSError, json.JSONDecodeError) as e:
        print(f"not ready / invalid: {e}", file=sys.stderr)
        return 3
    modules = manifest["modules"]
    packs = manifest["packs"]
    if args.pack_id:
        wanted = set(args.pack_id)
        packs = [p for p in packs if p["id"] in wanted]
    jobs = max(1, min(args.jobs, 4))
    started = time.time()
    doc = {
        "kind": "gate1b_multi_module_probe",
        "contract_version": CONTRACT_VERSION,
        "manifest_path": os.path.abspath(args.manifest),
        "manifest_sha256": digest,
        "ready_path": ready_path,
        "ready": ready,
        "snapshot": manifest["snapshot"],
        "snapshot_source_check": snap or "not checked (no --corpus-dir/"
                                         "--source-lock given)",
        "counts": counts,
        "tool": tool_info(args.probe),
        "configs": args.configs,
        "config_names": resolve_configs(args.probe, args.configs),
        "options": {"curve": args.curve, "self_dup": args.self_dup,
                    "jobs": jobs, "pack_filter": args.pack_id},
        "packs": [], "cases": [],
    }
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futures = [ex.submit(pack_task, args.probe, p, modules, args.configs,
                             args.curve, args.self_dup) for p in packs]
        for fut in futures:
            pack, code, result, err = fut.result()
            entry = {k: pack[k] for k in ("id", "module_ids", "tier",
                                          "origin_id", "collection_id",
                                          "fold_id", "representative",
                                          "evidence")}
            entry["result"] = result
            if result is None:
                entry["status"] = "error"
                entry["stderr"] = err
            elif code != 0 or not result.get("verified_all"):
                entry["status"] = "error"
            elif any(c.get("status") == "skip" for c in result["configs"]):
                entry["status"] = "skip"
            else:
                entry["status"] = "ok"
            doc["packs"].append(entry)
        cases = manifest.get("prefix_cases", []) if not args.pack_id else []
        futures = []
        for c in cases:
            if c["availability"] != "available":
                doc["cases"].append(dict(c, status="unavailable", result=None))
                continue
            futures.append(ex.submit(case_task, args.probe, c, modules,
                                     args.configs))
        for fut in futures:
            case, code, result, err = fut.result()
            entry = dict(case, result=result)
            if result is None or code != 0 or not result.get("verified_all"):
                entry["status"] = "error"
                entry["stderr"] = err
            elif any(c.get("status") == "skip" for c in result["configs"]):
                entry["status"] = "skip"
            else:
                entry["status"] = "ok"
            doc["cases"].append(entry)
    doc["elapsed_seconds"] = round(time.time() - started, 1)
    write_outputs(args.out, doc)
    sys.stdout.write(summarize(doc))
    bad = [p for p in doc["packs"] if p["status"] == "error"]
    bad += [c for c in doc["cases"] if c["status"] == "error"]
    return 1 if bad else 0


def cmd_fixtures(args):
    argv = ["pack", "--id", args.id, "--configs", args.configs]
    if args.curve:
        argv.append("--curve")
    if args.self_dup:
        argv.append("--self-dup")
    argv += ["--"] + args.files
    code, result, err = run_probe(args.probe, argv)
    doc = {"kind": "gate1b_fixture_diagnostic",
           "note": "explicit file list; tool validation only, not a pack "
                   "grouping and not representative",
           "tool": tool_info(args.probe),
           "files": [{"path": os.path.abspath(p), "sha256": sha256_file(p),
                      "byte_size": os.path.getsize(p)} for p in args.files],
           "result": result, "exit_code": code, "stderr": err}
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
    print(json.dumps(result if result else doc, indent=1))
    return 0 if code == 0 and result and result.get("verified_all") else 1


def cmd_fuzz_smoke(args):
    build = os.path.abspath(args.build_dir)
    rt = os.path.join(build, "fuzz_research_rt_lf")
    dec = os.path.join(build, "fuzz_research_dec_lf")
    probe = os.path.join(build, "arixv_pack_probe")
    for b in (rt, dec, probe):
        if not os.access(b, os.X_OK):
            print(f"missing {b} (configure with -DARIXV_BUILD_LIBFUZZER=ON "
                  f"-DARIXV_BUILD_RESEARCH_PACK=ON)", file=sys.stderr)
            return 2
    out = os.path.join(build, "fuzz-research")
    files = sorted(os.path.join(args.corpus, f) for f in os.listdir(args.corpus)
                   if f.endswith(".spv"))
    files = [f for f in files if os.path.getsize(f) <= args.seed_max_bytes]
    files = files[:args.seed_limit]
    for name in ("rt", "dec"):
        for sub in ("seeds", "corpus", "artifacts"):
            os.makedirs(os.path.join(out, name, sub), exist_ok=True)
        seeds = os.path.join(out, name, "seeds")
        for f in os.listdir(seeds):
            os.remove(os.path.join(seeds, f))
        arts = os.path.join(out, name, "artifacts")
        for f in os.listdir(arts):
            os.remove(os.path.join(arts, f))
    # Round-trip seeds: single modules (harness duplicates them) and ordered
    # pairs (concatenated; the harness splits at SPIR-V magic words).
    rt_seeds = os.path.join(out, "rt", "seeds")
    for i, f in enumerate(files):
        shutil.copyfile(f, os.path.join(rt_seeds, f"m{i:04d}.spv"))
        if i + 1 < len(files) and i % 4 == 0:
            with open(os.path.join(rt_seeds, f"p{i:04d}.bin"), "wb") as o:
                for g in (f, files[i + 1]):
                    with open(g, "rb") as src:
                        o.write(src.read())
    # Decode seeds: valid research streams under several policies.
    dec_seeds = os.path.join(out, "dec", "seeds")
    cfgs = ["sep:none:carry", "cont:none:carry", "cont:oce:carry",
            "sep:oce:reset", "cont:oc:mid", "sep:e:carry"]
    for i in range(0, max(0, len(files) - 1), 2):
        cfg = cfgs[(i // 2) % len(cfgs)]
        subprocess.run([probe, "emit", "--config", cfg, "--out",
                        os.path.join(dec_seeds, f"s{i:04d}.axrp"), "--",
                        files[i], files[i + 1]], check=False)
    # ARXP pack seeds live in the production fuzz-smoke (scripts/fuzz/smoke.sh).
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    failed = False
    for name, binary in (("rt", rt), ("dec", dec)):
        d = os.path.join(out, name)
        log = os.path.join(d, "fuzz.log")
        print(f"== fuzz-research {name}: {args.seconds}s x {args.jobs} jobs "
              f"(log: {log})", flush=True)
        with open(log, "w") as lf:
            status = subprocess.run(
                [binary, f"-fork={args.jobs}",
                 f"-max_total_time={args.seconds}", "-timeout=20",
                 "-rss_limit_mb=4096", "-print_final_stats=1",
                 f"-artifact_prefix={os.path.join(d, 'artifacts')}/",
                 os.path.join(d, "corpus"), os.path.join(d, "seeds")],
                stdout=lf, stderr=subprocess.STDOUT, env=env).returncode
        crashes = [f for f in os.listdir(os.path.join(d, "artifacts"))
                   if f.startswith("crash-")]
        others = [f for f in os.listdir(os.path.join(d, "artifacts"))
                  if not f.startswith("crash-")]
        cov = [l for l in open(log, errors="replace") if " cov: " in l]
        if cov:
            print(cov[-1].rstrip())
        if status != 0 or crashes:
            print(f"FAIL: fuzz-research {name} (exit {status}, "
                  f"{len(crashes)} crash artifact(s))")
            failed = True
        else:
            print(f"ok: fuzz-research {name}")
        if others:
            print(f"note: {len(others)} timeout/oom artifact(s) (not fatal)")
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# analyze: reproducible aggregation of a `run` results.json (optionally joined
# with gate 1a's published pack_baselines.csv / conditional_costs.csv, read
# only). Groups come from the manifest fields carried in results.json.
# ---------------------------------------------------------------------------

def _q(values, q):
    v = sorted(values)
    if not v:
        return None
    pos = (len(v) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)


def _pct(a, b):
    return None if not b else 100.0 * (a - b) / b


def _fmt(x, nd=2):
    return "n/a" if x is None else f"{x:+.{nd}f}"


def cmd_analyze(args):
    with open(args.results) as f:
        doc = json.load(f)
    configs = doc["config_names"]
    dup_packs = set()
    if args.manifest:
        with open(args.manifest, "rb") as f:
            mb = f.read()
        if sha256_bytes(mb) != doc["manifest_sha256"]:
            print("manifest does not match results.json", file=sys.stderr)
            return 3
        man = json.loads(mb.decode("utf-8"))
        for p in man["packs"]:
            shas = [man["modules"][m]["sha256"] for m in p["module_ids"]]
            if len(shas) != len(set(shas)):
                dup_packs.add(p["id"])
    gate1a = {}
    if args.gate1a_baselines:
        with open(args.gate1a_baselines, newline="") as f:
            for row in csv.DictReader(f):
                if row["verified"] == "True" and row["total_bytes"]:
                    gate1a.setdefault(row["pack_id"], {})[row["method"]] = int(
                        row["total_bytes"])
    generic = ["arx2_shared_header", "arx2_dedup", "orig_perfile_zstd19",
               "orig_solid_zstd19", "orig_solid_zstd22w27", "orig_solid_xz9e",
               "orig_tar_zstd19", "smolv_solid_zstd19",
               "smolv_solid_zstd22w27", "smolv_solid_xz9e",
               "smolv_tar_zstd19"]
    out = {"manifest_sha256": doc["manifest_sha256"], "configs": configs,
           "pack_groups": {}, "case_tiers": {}, "curve": {}, "self_dup": {},
           "skipped_packs": [], "per_collection": {}}
    lines = [f"# gate 1b analysis (manifest {doc['manifest_sha256']})", ""]

    # --- packs -------------------------------------------------------------
    groups = collections.OrderedDict()
    for p in doc["packs"]:
        if p["status"] != "ok":
            skip = [c for c in (p.get("result") or {}).get("configs", [])
                    if c.get("status") == "skip"]
            out["skipped_packs"].append({
                "id": p["id"], "status": p["status"],
                "reason": skip[0]["reason"] if skip else None,
                "failed_index": skip[0].get("failed_index") if skip else None})
            continue
        key = f"{'rep' if p['representative'] else 'diag'}/{p['tier']}"
        groups.setdefault(key, []).append(p)
        if args.manifest:
            groups.setdefault(key + ("/with-exact-dups" if p["id"] in dup_packs
                                     else "/no-exact-dups"), []).append(p)
    for key, packs in sorted(groups.items()):
        g = {"packs": len(packs), "members": 0, "original": 0, "arx2": 0,
             "structural_payload": 0, "cfg": {c: 0 for c in configs},
             "generic": {m: 0 for m in generic}, "generic_complete": True,
             "median_pct_vs_arx2": {}, "median_modeling_pct": {},
             "packs_modeling_regress": {}, "best_age_counts": {}}
        rel = {c: [] for c in configs}
        model_rel = {c: [] for c in configs}
        regress = {c: 0 for c in configs}
        best = collections.Counter()
        beats_generic = 0
        generic_packs = 0
        for p in packs:
            r = p["result"]
            cb = config_bytes(r)
            g["members"] += r["members"]
            g["original"] += r["original_bytes"]
            g["arx2"] += r["baseline"]["arx2_standard_bytes"]
            g["structural_payload"] += r["baseline"]["structural_payload_bytes"]
            fresh = cb.get("cont:none:carry")
            for c in configs:
                g["cfg"][c] += cb[c]
                rel[c].append(_pct(cb[c], r["baseline"]["arx2_standard_bytes"]))
                if fresh:
                    model_rel[c].append(_pct(cb[c], fresh))
                    if cb[c] > fresh:
                        regress[c] += 1
            ages = {a: cb.get(f"cont:oce:{a}") for a in ("carry", "reset",
                                                         "mid")}
            if all(v is not None for v in ages.values()) and r["members"] > 1:
                m = min(ages.values())
                best["+".join(a for a, v in ages.items() if v == m)] += 1
            gb = gate1a.get(p["id"], {})
            gvals = [gb[m] for m in generic if m in gb and
                     not m.startswith("arx2")]
            if gvals:
                generic_packs += 1
                beats_generic += cb["cont:oce:carry"] < min(gvals)
            for m in generic:
                if m in gb:
                    g["generic"][m] += gb[m]
                else:
                    g["generic_complete"] = False
        for c in configs:
            g["median_pct_vs_arx2"][c] = _q(rel[c], 0.5)
            g["median_modeling_pct"][c] = _q(model_rel[c], 0.5)
            g["packs_modeling_regress"][c] = regress[c]
        g["best_age_counts"] = dict(best)
        g["cont_oce_carry_beats_best_generic_packs"] = [beats_generic,
                                                        generic_packs]
        out["pack_groups"][key] = g

    lines.append("## Packs (whole-set totals; every row decode-verified)")
    lines.append("")
    hdr = ("| group | packs | original B | ARX2 | " + " | ".join(configs) +
           " |")
    lines.append(hdr)
    lines.append("|" + "---|" * (4 + len(configs)))
    for key, g in out["pack_groups"].items():
        cells = [f"{g['cfg'][c]} ({_fmt(_pct(g['cfg'][c], g['arx2']))}%)"
                 for c in configs]
        lines.append(f"| {key} | {g['packs']} | {g['original']} | {g['arx2']} "
                     f"| " + " | ".join(cells) + " |")
    lines.append("")
    lines.append("Per-pack medians (% vs ARX2 / modeling % vs cont:none) and "
                 "packs where the config is larger than cont:none:")
    lines.append("")
    lines.append("| group | config | median vs ARX2 | median vs cont:none | "
                 "packs > cont:none |")
    lines.append("|---|---|---|---|---|")
    for key, g in out["pack_groups"].items():
        for c in configs:
            lines.append(f"| {key} | {c} | {_fmt(g['median_pct_vs_arx2'][c])}"
                         f" | {_fmt(g['median_modeling_pct'][c])} | "
                         f"{g['packs_modeling_regress'][c]}/{g['packs']} |")
    lines.append("")
    lines.append("Best continuous age policy per multi-member pack: " +
                 "; ".join(f"{k}: {g['best_age_counts']}"
                           for k, g in out["pack_groups"].items()))
    if gate1a:
        lines.append("")
        lines.append("Gate 1a generic/wrapper totals over the same packs "
                     "(read from pack_baselines.csv):")
        lines.append("")
        lines.append("| group | " + " | ".join(generic) + " |")
        lines.append("|" + "---|" * (1 + len(generic)))
        for key, g in out["pack_groups"].items():
            note = "" if g["generic_complete"] else " (incomplete)"
            lines.append(f"| {key}{note} | " + " | ".join(
                f"{g['generic'][m]} ({_fmt(_pct(g['generic'][m], g['arx2']))}%)"
                for m in generic) + " |")
    if gate1a:
        lines.append("")
        lines.append("Packs where cont:oce:carry is smaller than the best "
                     "generic solid/per-file method of gate 1a: " + "; ".join(
                         f"{k}: {g['cont_oce_carry_beats_best_generic_packs'][0]}"
                         f"/{g['cont_oce_carry_beats_best_generic_packs'][1]}"
                         for k, g in out["pack_groups"].items()))
    lines.append("")
    lines.append("Skipped packs: " + json.dumps(out["skipped_packs"]))

    # per collection (representative same_collection)
    for p in groups.get("rep/same_collection", []):
        r = p["result"]
        cb = config_bytes(r)
        e = out["per_collection"].setdefault(p["collection_id"], {
            "packs": 0, "original": 0, "arx2": 0, "cont_fresh": 0,
            "cont_carry": 0})
        e["packs"] += 1
        e["original"] += r["original_bytes"]
        e["arx2"] += r["baseline"]["arx2_standard_bytes"]
        e["cont_fresh"] += cb["cont:none:carry"]
        e["cont_carry"] += cb["cont:oce:carry"]
    top = sorted(out["per_collection"].items(), key=lambda kv: -kv[1]["arx2"])
    lines.append("")
    lines.append("## Representative same_collection packs by collection "
                 "(largest 15 by ARX2 bytes)")
    lines.append("")
    lines.append("| collection | packs | ARX2 | cont:none | cont:oce:carry |")
    lines.append("|---|---|---|---|---|")
    for cid, e in top[:15]:
        lines.append(f"| {cid} | {e['packs']} | {e['arx2']} | "
                     f"{_fmt(_pct(e['cont_fresh'], e['arx2']))}% | "
                     f"{_fmt(_pct(e['cont_carry'], e['arx2']))}% |")

    # --- prefix cases ------------------------------------------------------
    xz = {}
    if args.gate1a_cases:
        with open(args.gate1a_cases, newline="") as f:
            for row in csv.DictReader(f):
                if row["availability"] == "available" and row["verified"] == \
                        "True" and row["xz_conditional_target_bytes"]:
                    xz[row["case_id"]] = (int(row["xz_conditional_target_bytes"]),
                                          int(row["arx2_standalone_target_bytes"]))
    tiers = collections.OrderedDict()
    for c in doc["cases"]:
        tiers.setdefault(c["tier"], {"available": 0, "unavailable": 0,
                                     "skip": 0, "ok": 0, "error": 0,
                                     "ratios": collections.defaultdict(list),
                                     "prefix_bytes": [], "cases": []})
        t = tiers[c["tier"]]
        t["available" if c["availability"] == "available" else
          "unavailable"] += 1
        if c["status"] in ("skip", "ok", "error"):
            t[c["status"]] += 1
        if c["status"] != "ok":
            continue
        r = c["result"]
        t["prefix_bytes"].append(r["prefix_original_bytes"])
        for cfg in r["configs"]:
            if cfg.get("status") != "ok" or not cfg["target_alone_stream_bytes"]:
                continue
            name = cfg["config"]
            if name.startswith("cont"):
                t["ratios"][name].append(cfg["conditional_target_bytes"] /
                                         cfg["target_alone_stream_bytes"])
            elif "target_member_payload_bytes" in cfg:
                t["ratios"][name].append(cfg["target_member_payload_bytes"] /
                                         cfg["target_alone_payload_bytes"])
        if c["id"] in xz and xz[c["id"]][1]:
            t["ratios"]["gate1a:xz_conditional/arx2_alone"].append(
                xz[c["id"]][0] / xz[c["id"]][1])
    lines.append("")
    lines.append("## Prefix cases: conditional target cost / no-prefix target "
                 "cost (median [p25, p75], n)")
    lines.append("")
    lines.append("Continuous configs: (finalized whole - finalized prefix) / "
                 "target alone. Separate configs: exact target member payload "
                 "/ target-alone payload. Conditional costs are diagnostics: "
                 "deployable totals include the prefix (whole_stream_bytes).")
    lines.append("")
    show = ["cont:none:carry", "cont:oce:carry", "cont:oce:reset",
            "cont:oce:mid", "sep:oce:carry", "gate1a:xz_conditional/arx2_alone"]
    lines.append("| tier | avail | unavail | ok | skip | median prefix B | " +
                 " | ".join(show) + " |")
    lines.append("|" + "---|" * (6 + len(show)))
    for tier, t in tiers.items():
        summ = {}
        for name, vals in t["ratios"].items():
            summ[name] = {"n": len(vals), "median": _q(vals, 0.5),
                          "p25": _q(vals, 0.25), "p75": _q(vals, 0.75)}
        out["case_tiers"][tier] = {k: t[k] for k in ("available", "unavailable",
                                                     "ok", "skip", "error")}
        out["case_tiers"][tier]["median_prefix_original_bytes"] = _q(
            t["prefix_bytes"], 0.5)
        out["case_tiers"][tier]["ratios"] = summ
        cells = []
        for name in show:
            sm = summ.get(name)
            cells.append("n/a" if not sm or sm["median"] is None else
                         f"{sm['median']:.3f} [{sm['p25']:.3f}, "
                         f"{sm['p75']:.3f}] n={sm['n']}")
        mp = out["case_tiers"][tier]["median_prefix_original_bytes"]
        lines.append(f"| {tier} | {t['available']} | {t['unavailable']} | "
                     f"{t['ok']} | {t['skip']} | "
                     f"{'n/a' if mp is None else int(mp)} | " +
                     " | ".join(cells) + " |")

    # paired: same target, same_collection vs unrelated (cont:oce:carry)
    by_target = collections.defaultdict(dict)
    for c in doc["cases"]:
        if c["status"] != "ok":
            continue
        for cfg in c["result"]["configs"]:
            if cfg["config"] == "cont:oce:carry" and cfg.get("status") == "ok":
                by_target[c["target_id"]].setdefault(c["tier"], []).append(
                    cfg["conditional_target_bytes"] /
                    cfg["target_alone_stream_bytes"])
    pairs = {}
    for a, b in (("same_collection", "unrelated"),
                 ("stage_pair", "unrelated"),
                 ("variant_family", "unrelated"),
                 ("unrelated_toolchain_matched", "unrelated"),
                 ("stage_pair", "same_collection")):
        diffs = [sum(v[a]) / len(v[a]) - sum(v[b]) / len(v[b])
                 for v in by_target.values() if a in v and b in v]
        pairs[f"{a}-{b}"] = {"targets": len(diffs),
                             "median": _q(diffs, 0.5), "p25": _q(diffs, 0.25),
                             "p75": _q(diffs, 0.75)}
    out["paired_target_ratio_differences_cont_oce_carry"] = pairs
    lines.append("")
    lines.append("Paired per-target differences of the cont:oce:carry ratio "
                 "(mean over a target's cases per tier; negative = first tier "
                 "cheaper):")
    lines.append("")
    lines.append("| pair | targets | median | p25 | p75 |")
    lines.append("|---|---|---|---|---|")
    for k, v in pairs.items():
        lines.append(f"| {k} | {v['targets']} | {_fmt(v['median'], 3)} | "
                     f"{_fmt(v['p25'], 3)} | {_fmt(v['p75'], 3)} |")

    # --- incremental curves (representative same_collection) ----------------
    bins = [(1, 1), (2, 2), (3, 4), (5, 8), (9, 16), (17, 32), (33, 10**9)]
    curve = {}
    for key in ("rep/same_collection", "rep/variant_family", "rep/stage_pair"):
        acc = {}
        for p in groups.get(key, []):
            r = p["result"]
            alone = r["baseline"]["member_structural_payload_bytes"]
            for cv in r.get("curve", []):
                if not cv["verified"]:
                    continue
                tot = cv["prefix_stream_bytes"]
                for k in range(len(tot)):
                    marg = tot[k] - (tot[k - 1] if k else 0)
                    pos = k + 1
                    b = next(i for i, (lo, hi) in enumerate(bins)
                             if lo <= pos <= hi)
                    e = acc.setdefault(cv["config"], [[0, 0, 0] for _ in bins])
                    e[b][0] += marg
                    e[b][1] += alone[k]
                    e[b][2] += 1
        curve[key] = {c: [{"positions": f"{lo}-{hi if hi < 10**9 else 'inf'}",
                           "modules": e[2], "marginal_bytes": e[0],
                           "alone_payload_bytes": e[1],
                           "ratio": (e[0] / e[1]) if e[1] else None}
                          for (lo, hi), e in zip(bins, v)]
                      for c, v in acc.items()}
    out["curve"] = curve
    lines.append("")
    lines.append("## Incremental cost by position (finalized prefix encodes): "
                 "sum marginal bytes / sum standalone structural payload")
    for key, cs in curve.items():
        lines.append("")
        lines.append(f"{key}:")
        lines.append("")
        lines.append("| config | " + " | ".join(
            f"pos {b['positions']}" for b in next(iter(cs.values()))) + " |"
            if cs else "| (none) |")
        lines.append("|" + "---|" * (1 + len(bins)))
        for c in ("cont:none:carry", "cont:oce:carry", "cont:oce:reset",
                  "sep:none:carry"):
            if c in cs:
                lines.append(f"| {c} | " + " | ".join(
                    "n/a" if b["ratio"] is None else
                    f"{b['ratio']:.3f} (n={b['modules']})" for b in cs[c]) +
                    " |")

    # --- self duplicates (unique contents, representative + diagnostic) -----
    seen = set()
    agg = collections.defaultdict(lambda: [0, 0, 0])
    for p in doc["packs"]:
        r = p.get("result")
        if p["status"] != "ok" or not r or "self_dup" not in r:
            continue
        for sd in r["self_dup"]:
            mid = p["module_ids"][sd["index"]]
            if mid in seen:
                continue
            seen.add(mid)
            for res in sd["results"]:
                if "single_stream_bytes" not in res or not res["verified"]:
                    continue
                a = agg[res["config"]]
                a[0] += res["single_stream_bytes"]
                a[1] += res["double_stream_bytes"] - res["single_stream_bytes"]
                a[2] += 1
    out["self_dup"] = {c: {"modules": v[2], "single_bytes": v[0],
                           "second_copy_bytes": v[1],
                           "second_copy_fraction": v[1] / v[0] if v[0] else None}
                       for c, v in agg.items()}
    lines.append("")
    lines.append("## Self-duplicate diagnostic (synthetic; module ids "
                 "deduplicated; not representative)")
    lines.append("")
    lines.append("| config | modules | second copy / single |")
    lines.append("|---|---|---|")
    for c, v in out["self_dup"].items():
        lines.append(f"| {c} | {v['modules']} | "
                     f"{v['second_copy_fraction']:.3f} |")

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "analysis.json"), "w") as f:
        json.dump(out, f, indent=1, sort_keys=True)
    with open(os.path.join(args.out, "analysis.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def manifest_args(p):
        p.add_argument("--manifest", required=True)
        p.add_argument("--ready", help="default: manifest.ready.json beside "
                                       "the manifest")
        p.add_argument("--corpus-dir", help="optional: re-hash manifest.tsv, "
                       "duplicates.tsv, sources.tsv against the snapshot")
        p.add_argument("--source-lock", help="optional: re-hash "
                       "benchmark-sources.lock.json against the snapshot")

    p = sub.add_parser("validate")
    manifest_args(p)
    p.set_defaults(fn=cmd_validate)

    p = sub.add_parser("run")
    manifest_args(p)
    p.add_argument("--probe", required=True, help="arixv_pack_probe binary")
    p.add_argument("--out", required=True, help="output directory")
    p.add_argument("--configs", default=DEFAULT_CONFIGS)
    p.add_argument("--curve", action="store_true",
                   help="finalized prefix encodes for incremental costs")
    p.add_argument("--self-dup", action="store_true")
    p.add_argument("--pack-id", action="append",
                   help="restrict to these pack ids (diagnostic reruns)")
    p.add_argument("--jobs", type=int, default=1, help="max 4")
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("fixtures")
    p.add_argument("--probe", required=True)
    p.add_argument("--id", default="fixture")
    p.add_argument("--configs", default=DEFAULT_CONFIGS)
    p.add_argument("--curve", action="store_true")
    p.add_argument("--self-dup", action="store_true")
    p.add_argument("--out")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_fixtures)

    p = sub.add_parser("fuzz-smoke")
    p.add_argument("--build-dir", required=True)
    p.add_argument("--corpus", required=True, help="read-only .spv seed dir")
    p.add_argument("--seconds", type=int, default=150)
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--seed-limit", type=int, default=400)
    p.add_argument("--seed-max-bytes", type=int, default=100000)
    p.set_defaults(fn=cmd_fuzz_smoke)

    p = sub.add_parser("analyze")
    p.add_argument("--results", required=True, help="run's results.json")
    p.add_argument("--out", required=True)
    p.add_argument("--gate1a-baselines", help="gate 1a pack_baselines.csv")
    p.add_argument("--gate1a-cases", help="gate 1a conditional_costs.csv")
    p.add_argument("--manifest", help="frozen manifest (SHA256 must match "
                   "results.json) to split packs with exact duplicates")
    p.set_defaults(fn=cmd_analyze)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
