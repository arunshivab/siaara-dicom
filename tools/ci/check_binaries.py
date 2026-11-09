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

  hardening  Checks the security properties of the given binaries (review R-014): Linux (readelf):
           full RELRO (GNU_RELRO + BIND_NOW), non-executable stack, PIE/DSO, stack protector
           references; with --fortify, _FORTIFY_SOURCE wrappers (__*_chk) referenced. Windows
           (dumpbin /headers /loadconfig): DYNAMICBASE, NX, Control Flow Guard, and on x64 high
           entropy VA and CET compatibility. macOS (otool): PIE executables, no executable stack.

               python3 tools/ci/check_binaries.py hardening build/libsiaara.so build/tools/siaara-dump

  --profile release (deps): additionally denies the C/C++ runtime libraries that release builds
           link statically (libstdc++/libgcc_s on Linux, the MSVC runtime DLLs on Windows).

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
    allow, deny = cfg["allow"], list(cfg.get("deny", []))
    if args.profile == "release":
        deny += cfg.get("deny_release", [])
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


def hardening_problems(path, osname, fortify):
    problems = []
    if osname == "linux":
        hdr = run(["readelf", "-hlW", path])
        dyn = run(["readelf", "-dW", path])
        syms = run(["readelf", "-sW", path])  # .dynsym and .symtab (i386 PIC uses __stack_chk_fail_local)
        if "GNU_RELRO" not in hdr:
            problems.append("no GNU_RELRO segment (-z relro)")
        if not re.search(r"\(FLAGS\).*BIND_NOW|\(FLAGS_1\).*NOW", dyn):
            problems.append("no BIND_NOW (-z now): only partial RELRO")
        m = re.search(r"GNU_STACK\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)", hdr)
        if not m:
            problems.append("no GNU_STACK header (stack executable by default)")
        elif "E" in m.group(1):
            problems.append("executable stack (GNU_STACK %s)" % m.group(1))
        if not re.search(r"Type:\s+DYN", hdr):
            problems.append("not position independent (ELF type is not DYN)")
        if "__stack_chk_fail" not in syms:
            problems.append("no stack protector references (__stack_chk_fail)")
        if fortify and not re.search(r"__\w+_chk\b", syms.replace("__stack_chk_fail", "")):
            problems.append("no _FORTIFY_SOURCE wrappers (__*_chk) referenced")
    elif osname == "windows":
        out = run([dumpbin(), "/nologo", "/headers", "/loadconfig", path])
        machine_x64 = "8664 machine (x64)" in out
        if "Dynamic base" not in out:
            problems.append("not /DYNAMICBASE")
        if "NX compatible" not in out:
            problems.append("not /NXCOMPAT")
        if "Control Flow Guard" not in out and "Guard" not in out:
            problems.append("no Control Flow Guard (/guard:cf)")
        if machine_x64 and "High Entropy Virtual Addresses" not in out:
            problems.append("not /HIGHENTROPYVA")
        if machine_x64 and "CET compatible" not in out:
            problems.append("not /CETCOMPAT")
    elif osname == "macos":
        out = run(["otool", "-hv", path])
        if "EXECUTE" in out and "PIE" not in out:
            problems.append("executable is not PIE")
        if "ALLOW_STACK_EXECUTION" in out:
            problems.append("stack execution allowed")
    return problems


def cmd_hardening(args):
    osname = args.os or host_os()
    bad = 0
    for path in args.binaries:
        p = hardening_problems(path, osname, args.fortify)
        print("%s: %s" % (path, "ok" if not p else "; ".join(p)))
        bad += len(p)
    return 1 if bad else 0


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
    d.add_argument("--profile", choices=["default", "release"], default="default")
    d.add_argument("binaries", nargs="+")
    h = sub.add_parser("hardening")
    h.add_argument("--fortify", action="store_true", help="require _FORTIFY_SOURCE wrappers (Linux)")
    h.add_argument("binaries", nargs="+")
    s = sub.add_parser("symbols")
    s.add_argument("--update", action="store_true")
    s.add_argument("--header", help="also check that every SIAARA_API function declared here is exported")
    s.add_argument("library")
    lst = sub.add_parser("list")
    lst.add_argument("binaries", nargs="+")
    args = ap.parse_args()
    try:
        return {"deps": cmd_deps, "symbols": cmd_symbols, "hardening": cmd_hardening, "list": cmd_list}[args.cmd](args)
    except RuntimeError as e:
        print(str(e))
        return 2


if __name__ == "__main__":
    sys.exit(main())
