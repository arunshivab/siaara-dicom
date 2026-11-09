#!/usr/bin/env python3
"""Gates clang-tidy findings against a committed baseline (standard library only).

    run-clang-tidy -p build-tidy -quiet "src/|tools/" > tidy.log 2>&1 || true
    python3 tools/ci/clang_tidy_gate.py tidy.log                # fail on findings not in the baseline
    python3 tools/ci/clang_tidy_gate.py --update tidy.log       # rewrite the baseline

The checks are those of the committed .clang-tidy. Findings that existed when the gate was
introduced are listed in tools/ci/clang_tidy_baseline.txt as "<path>|<check>|<message>" (no line
numbers, so unrelated edits do not churn it); a finding not in the baseline fails the job, and a
baseline entry that no longer occurs is reported so the baseline can shrink as code is fixed.
"""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BASELINE = os.path.join(HERE, "clang_tidy_baseline.txt")
LINE = re.compile(r"^(?P<path>[^:\s][^:]*):(?P<line>\d+):(?P<col>\d+): (?:warning|error): (?P<msg>.*?) \[(?P<checks>[^\]]+)\]$")


def findings(log):
    out = collections.Counter()
    seen = set()
    with open(log, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = LINE.match(ln.rstrip("\n"))
            if not m:
                continue
            path = os.path.relpath(os.path.abspath(m.group("path")), ROOT).replace(os.sep, "/")
            if path.startswith(".."):
                continue  # system headers
            check = [c for c in m.group("checks").split(",") if c and not c.startswith("-warnings")][0]
            msg = re.sub(r"'[^']*'", "'_'", m.group("msg"))
            key = (path, m.group("line"), m.group("col"), check)
            if key in seen:  # the same header finding reported from several translation units
                continue
            seen.add(key)
            out["%s|%s|%s" % (path, check, msg)] += 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("log")
    args = ap.parse_args()
    now = findings(args.log)
    if args.update:
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
            f.write("# clang-tidy findings accepted when the gate was introduced (tools/ci/clang_tidy_gate.py).\n")
            f.write("# Remove entries as the code is fixed; never add entries for new code.\n")
            for k in sorted(now):
                for _ in range(now[k]):
                    f.write(k + "\n")
        print("baseline: %d findings" % sum(now.values()))
        return 0
    base = collections.Counter()
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding="utf-8") as f:
            for ln in f:
                ln = ln.rstrip("\n")
                if ln and not ln.startswith("#"):
                    base[ln] += 1
    new = now - base
    fixed = base - now
    for k in sorted(new):
        print("NEW: " + k)
    for k in sorted(fixed):
        print("fixed (remove from the baseline): " + k)
    print("%d findings, %d in the baseline, %d new" % (sum(now.values()), sum(base.values()), sum(new.values())))
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
