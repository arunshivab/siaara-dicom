#!/usr/bin/env python3
"""Release-gate checks on built binaries (standard library only).

  deps     Every runtime dependency of the given binaries must be on the per-OS allow-list in
           tools/ci/deps_allowlist.json (readelf -d on Linux, otool -L on macOS, dumpbin
           /dependents on Windows). Entries on the deny list (on Linux: libssl, libcrypto - the
           system OpenSSL is loaded with dlopen at run time, never linked) always fail.

               python3 tools/ci/check_binaries.py deps build/libsiaara.so build/tools/siaara-dump

  symbols  The shared library may export only C API symbols (siaara_*), and the exported set must
           equal the committed baseline tools/ci/abi/exported_symbols.txt: a removal breaks
           callers, an addition must be planned (update the baseline in the same change with
           --update and review the diff).

               python3 tools/ci/check_binaries.py symbols build/libsiaara.so
               python3 tools/ci/check_binaries.py symbols --update build/libsiaara.so

  list     Prints the dependencies of the given binaries (for the SBOM script).
"""
import argparse
import fnmatch
import json
import os
import platform
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOWLIST = os.path.join(HERE, "deps_allowlist.json")
BASELINE = os.path.join(HERE, "abi", "exported_symbols.txt")
EXPORT_PREFIX = "siaara_"


def host_os():
    s = platform.system()
    return {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(s, s.lower())


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    out = p.stdout.decode("utf-8", "replace")
    if p.returncode != 0:
        raise RuntimeError("%s failed (%d):\n%s" % (" ".join(cmd), p.returncode, out))
    return out


def dumpbin():
    exe = shutil.which("dumpbin") or shutil.which("dumpbin.exe")
    if not exe:
        raise RuntimeError("dumpbin not found: run from a Visual Studio developer environment")
    return exe


def dependencies(path, osname):
    """Runtime library dependencies recorded in the binary."""
    if osname == "linux":
        out = run(["readelf", "-d", "-W", path])
        return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)
    if osname == "macos":
        out = run(["otool", "-L", path])
        lines = out.splitlines()[1:]
        deps = [ln.strip().split(" (")[0] for ln in lines if ln.strip()]
        # A dylib lists its own install name first.
        install_name = run(["otool", "-D", path]).splitlines()[1:]
        own = install_name[0].strip() if install_name else None
        return [d for d in deps if d != own]
    if osname == "windows":
        out = run([dumpbin(), "/nologo", "/dependents", path])
        deps = []
        started = False
        for ln in out.splitlines():
            s = ln.strip()
            if "has the following dependencies" in s:
                started = True
                continue
            if started:
                if s.lower().startswith("summary"):
                    break
                if s.lower().endswith(".dll"):
                    deps.append(s)
        return deps
    raise RuntimeError("unsupported OS " + osname)


def exported_symbols(path, osname):
    if osname == "linux":
        out = run(["nm", "-D", "--defined-only", "--format=posix", path])
        return sorted({ln.split()[0].split("@")[0] for ln in out.splitlines() if ln.strip()})
    if osname == "macos":
        out = run(["nm", "-gU", "-j", path])
        return sorted({s[1:] if s.startswith("_") else s for s in out.split()})
    if osname == "windows":
        out = run([dumpbin(), "/nologo", "/exports", path])
        names = set()
        in_table = False
        for ln in out.splitlines():
            if re.match(r"\s*ordinal\s+hint\s+RVA\s+name", ln):
                in_table = True
                continue
            if in_table:
                if ln.strip().lower().startswith("summary"):
                    break
                m = re.match(r"\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)", ln)
                if m:
                    names.add(m.group(1))
        return sorted(names)
    raise RuntimeError("unsupported OS " + osname)


def cmd_deps(args):
    osname = args.os or host_os()
    with open(ALLOWLIST, encoding="utf-8") as f:
        cfg = json.load(f)[osname]
    allow, deny = cfg["allow"], cfg.get("deny", [])
    fold = (lambda s: s.lower()) if osname == "windows" else (lambda s: s)
    bad = 0
    for path in args.binaries:
        deps = dependencies(path, osname)
        print("%s:" % path)
        for d in deps:
            name = fold(d)
            denied = any(fnmatch.fnmatchcase(name, fold(p)) or fnmatch.fnmatchcase(os.path.basename(name), fold(p)) for p in deny)
            allowed = any(fnmatch.fnmatchcase(name, fold(p)) for p in allow)
            verdict = "DENIED" if denied else ("ok" if allowed else "NOT ALLOWED")
            print("  %-60s %s" % (d, verdict))
            if denied or not allowed:
                bad += 1
    if bad:
        print("%d dependency problem(s); allow-list: %s" % (bad, os.path.relpath(ALLOWLIST)))
        return 1
    return 0


def cmd_symbols(args):
    osname = args.os or host_os()
    syms = exported_symbols(args.library, osname)
    foreign = [s for s in syms if not s.startswith(EXPORT_PREFIX)]
    if args.update:
        if foreign:
            print("refusing to write a baseline with non-%s symbols: %s" % (EXPORT_PREFIX, foreign[:20]))
            return 1
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
            f.write("# Exported symbols of the siaara shared library (C ABI). Generated by\n")
            f.write("# tools/ci/check_binaries.py symbols --update; review every change.\n")
            for s in syms:
                f.write(s + "\n")
        print("wrote %d symbols to %s" % (len(syms), BASELINE))
        return 0
    with open(BASELINE, encoding="utf-8") as f:
        base = sorted(ln.strip() for ln in f if ln.strip() and not ln.startswith("#"))
    removed = sorted(set(base) - set(syms))
    added = sorted(set(syms) - set(base))
    rc = 0
    if foreign:
        print("exported symbols outside the C API (%d):" % len(foreign))
        for s in foreign[:50]:
            print("  " + s)
        rc = 1
    if removed:
        print("symbols removed from the C ABI (breaks existing callers):")
        for s in removed:
            print("  - " + s)
        rc = 1
    if added:
        print("symbols added that are not in the baseline (update %s if planned):" % os.path.relpath(BASELINE))
        for s in added:
            print("  + " + s)
        rc = 1
    if args.header:
        with open(args.header, encoding="utf-8") as f:
            declared = set(re.findall(r"SIAARA_CALL\s+(" + EXPORT_PREFIX + r"\w+)\s*\(", f.read()))
        missing = sorted(declared - set(syms))
        if missing:
            print("declared in %s but not exported:" % args.header)
            for s in missing:
                print("  ! " + s)
            rc = 1
    if rc == 0:
        print("%d exported symbols match the baseline" % len(syms))
    return rc


def cmd_list(args):
    osname = args.os or host_os()
    result = {p: dependencies(p, osname) for p in args.binaries}
    print(json.dumps(result, indent=2))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--os", choices=["linux", "macos", "windows"], help="default: the host OS")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("deps")
    d.add_argument("binaries", nargs="+")
    s = sub.add_parser("symbols")
    s.add_argument("--update", action="store_true")
    s.add_argument("--header", help="also check that every SIAARA_API function declared here is exported")
    s.add_argument("library")
    lst = sub.add_parser("list")
    lst.add_argument("binaries", nargs="+")
    args = ap.parse_args()
    try:
        return {"deps": cmd_deps, "symbols": cmd_symbols, "list": cmd_list}[args.cmd](args)
    except RuntimeError as e:
        print(str(e))
        return 2


if __name__ == "__main__":
    sys.exit(main())
