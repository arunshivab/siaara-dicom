#!/usr/bin/env python3
"""Runs the libFuzzer targets for a time budget each and collects findings.

    python3 fuzz/run_fuzzers.py --bin build-fuzz/fuzz --seconds 60 --jobs 4 --work /tmp/fuzz-work
    python3 fuzz/run_fuzzers.py --bin build-fuzz/fuzz --seconds 1800 --targets part10,pdu --keep-going

For each target this runs build-fuzz/fuzz/fuzz_<target> with
    -max_total_time=<seconds> -timeout=<hang seconds> -rss_limit_mb=<mb>
over a writable corpus folder (<work>/<target>/corpus, where new inputs go) plus the read-only
seed folders: fuzz/corpus/<target>, tests/regress/<target> and any --extra folders. Crashes,
hangs (timeout-*), out-of-memory and leak reports land in <work>/<target>/artifacts/.

--keep-going uses libFuzzer's fork mode (-fork=1 -ignore_crashes=1 -ignore_timeouts=1
-ignore_ooms=1) so one finding does not end the run; otherwise a target stops at its first
finding. The exit status is 1 when any target produced an artifact or failed to start.
Standard library only.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def all_targets():
    with open(os.path.join(HERE, "CMakeLists.txt"), encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"set\(SIAARA_FUZZ_TARGETS\s+(.*?)\)", text, re.S)
    return m.group(1).split()


def run_one(args, target):
    exe = os.path.join(args.bin, "fuzz_" + target + (".exe" if os.name == "nt" else ""))
    work = os.path.join(args.work, target)
    corpus = os.path.join(work, "corpus")
    artifacts = os.path.join(work, "artifacts")
    os.makedirs(corpus, exist_ok=True)
    os.makedirs(artifacts, exist_ok=True)
    seeds = [os.path.join(ROOT, "fuzz", "corpus", target), os.path.join(ROOT, "tests", "regress", target)]
    seeds += [os.path.join(e, target) for e in args.extra]
    seeds = [s for s in seeds if os.path.isdir(s)]
    cmd = [exe, "-max_total_time=%d" % args.seconds, "-timeout=%d" % args.hang_seconds,
           "-rss_limit_mb=%d" % args.rss_mb, "-max_len=%d" % args.max_len, "-print_final_stats=1",
           "-artifact_prefix=" + artifacts + os.sep]
    if args.keep_going:
        cmd += ["-fork=1", "-ignore_crashes=1", "-ignore_timeouts=1", "-ignore_ooms=1"]
    cmd += [corpus] + seeds
    log = os.path.join(work, "fuzz.log")
    t0 = time.time()
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1")
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
    with open(log, "wb") as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)
    elapsed = time.time() - t0
    with open(log, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    stats = dict(re.findall(r"stat::(\w+):\s*(\d+)", text))
    cov = re.findall(r"cov: (\d+) ft: (\d+)", text)
    found = sorted(x for x in os.listdir(artifacts) if not x.startswith("."))
    return {
        "target": target, "rc": rc, "seconds": elapsed, "execs": int(stats.get("number_of_executed_units", 0)),
        "new": int(stats.get("new_units_added", 0)), "cov": cov[-1][0] if cov else "?",
        "corpus": len(os.listdir(corpus)), "artifacts": found, "log": log,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="folder with the fuzz_<target> executables")
    ap.add_argument("--targets", default="all", help="comma separated, or 'all'")
    ap.add_argument("--seconds", type=int, default=60, help="time budget per target")
    ap.add_argument("--hang-seconds", type=int, default=2, help="an input slower than this is a hang")
    ap.add_argument("--rss-mb", type=int, default=2560)
    ap.add_argument("--max-len", type=int, default=262144)
    ap.add_argument("--jobs", type=int, default=1, help="targets run in parallel")
    ap.add_argument("--work", default=os.path.join(ROOT, "build-fuzz", "fuzz-work"))
    ap.add_argument("--extra", action="append", default=[], help="extra seed root (with <target>/ folders)")
    ap.add_argument("--keep-going", action="store_true", help="fork mode: continue after findings")
    ap.add_argument("--shard", default="1/1", help="i/n: run every n-th target starting at the i-th")
    args = ap.parse_args()
    targets = all_targets() if args.targets == "all" else [t for t in args.targets.split(",") if t]
    i, n = (int(x) for x in args.shard.split("/"))
    targets = targets[i - 1::n]
    os.makedirs(args.work, exist_ok=True)
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for r in pool.map(lambda t: run_one(args, t), targets):
            results.append(r)
            print("%-14s %6.0f s  %10d execs  cov %-6s new %-5d corpus %-5d rc %d %s" % (
                r["target"], r["seconds"], r["execs"], r["cov"], r["new"], r["corpus"], r["rc"],
                ("FINDINGS: " + " ".join(r["artifacts"])) if r["artifacts"] else ""), flush=True)
    bad = [r for r in results if r["artifacts"] or (r["rc"] != 0 and not args.keep_going)]
    for r in bad:
        print("see %s" % r["log"])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
