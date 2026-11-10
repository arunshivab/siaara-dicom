#!/usr/bin/env python3
"""Gates clang-tidy findings against a committed baseline (standard library only).

    run-clang-tidy -p build-tidy -quiet "src/|tools/" > tidy.log 2>&1 || true
    python3 tools/ci/clang_tidy_gate.py --compile-commands build-tidy/compile_commands.json \
        --files "src/|tools/" tidy.log                          # fail on findings not in the baseline
    python3 tools/ci/clang_tidy_gate.py --update tidy.log       # rewrite the baseline

The log must be a complete run (review R-141): the gate fails when the log holds no clang-tidy
invocation at all, when it has a crash or an error line that is not a finding (clang-tidy could not
analyse a file), and - with --compile-commands - when a translation unit of the compile database
that matches --files (the same regular expression run-clang-tidy was given) has no invocation in
the log. An empty or cut-off log therefore never passes.

The checks are those of the committed .clang-tidy. Findings that existed when the gate was
introduced are listed in tools/ci/clang_tidy_baseline.txt as "<path>|<check>|<message>" (no line
numbers, so unrelated edits do not churn it); a finding not in the baseline fails the job, and a
baseline entry that no longer occurs is reported so the baseline can shrink as code is fixed.
"""
import argparse
import collections
import json
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


# run-clang-tidy prints one line per translation unit: the clang-tidy command, ending in the file.
INVOCATION = re.compile(r"^\S*clang-tidy\S*\s.*-p[= ]\S+.*\s(?P<file>\S+)$")
# Signs that clang-tidy did not analyse something. Only at the start of a line: the source lines
# clang-tidy echoes under a finding are indented and may contain any text.
BROKEN = re.compile(
    r"^(?:error: |fatal error: |Error while processing |Error: |LLVM ERROR|PLEASE submit a bug report|Stack dump:|"
    r"Segmentation fault|clang-tidy\S*: error|\S.*: terminated by signal |\S.*clang-tidy\S*: not found|"
    r"\S+ errors? generated|Traceback \(most recent call last\)|"
    r"Found compiler error\(s\)|[^:\s][^:]*:\d+:\d+: (?:fatal )?error: (?!.*\[[^\]]+\]$))")


def run_problems(log, compile_commands, files_re):
    """Why the log is not a complete clang-tidy run (empty list: it is)."""
    problems = []
    analysed = set()
    with open(log, encoding="utf-8", errors="replace") as f:
        for n, ln in enumerate(f, 1):
            ln = ln.rstrip("\n")
            m = INVOCATION.match(ln)
            if m:
                analysed.add(os.path.realpath(m.group("file")))
                continue
            if BROKEN.match(ln) and len(problems) < 20:
                problems.append("line %d: %s" % (n, ln[:200]))
    if not analysed:
        problems.append("the log holds no clang-tidy invocation: clang-tidy did not run")
    if compile_commands:
        with open(compile_commands, encoding="utf-8") as f:
            db = json.load(f)
        pattern = re.compile(files_re)
        expected = set()
        for entry in db:
            path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
            if pattern.search(path):
                expected.add(os.path.realpath(path))
        if not expected:
            problems.append("no translation unit of %s matches '%s'" % (compile_commands, files_re))
        missing = sorted(expected - analysed)
        if missing:
            problems.append("%d of %d translation units were not analysed, e.g. %s" % (
                len(missing), len(expected), ", ".join(os.path.relpath(p, ROOT) for p in missing[:5])))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--compile-commands", help="compile_commands.json the run used: every matching translation unit must be in the log")
    ap.add_argument("--files", default=".*", help="regular expression given to run-clang-tidy (default: every file)")
    ap.add_argument("log")
    args = ap.parse_args()
    broken = run_problems(args.log, args.compile_commands, args.files)
    if broken:
        print("clang-tidy did not complete; the log cannot be gated:")
        for b in broken:
            print("  " + b)
        return 2
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
