#!/usr/bin/env python3
"""DICOMDIR cross-check against DCMTK (dcmmkdir, dcmdump) and pydicom's FileSet.

Third-party tools are used as test oracles only.

    python3 -I tests/crosscheck/dicomdir_crosscheck.py <build>/tests/dicomdir_tool

1. SIAARA writes a DICOMDIR; dcmdump must parse it, pydicom's FileSet must load it with every
   instance, and dcmmkdir must be able to append to it (which re-walks every offset link).
2. dcmmkdir and pydicom write DICOMDIRs; SIAARA must read the same records and files as pydicom.
Exits non-zero on any mismatch; parts whose oracle is missing are reported as skipped.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile


def run(args, cwd=None):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, errors="replace")


def siaara_read(tool, dicomdir):
    p = run([tool, "read", dicomdir])
    if p.returncode != 0:
        raise SystemExit("siaara read failed: " + p.stderr)
    lines = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
    return lines[:-1], lines[-1]


def pydicom_view(dicomdir):
    from pydicom.fileset import FileSet
    fs = FileSet(dicomdir)
    out = {}
    for inst in fs:
        out[str(inst.SOPInstanceUID)] = os.path.realpath(inst.path)
    counts = {}
    for node in fs._tree:  # noqa: SLF001 (walk every record node)
        t = node.record_type
        counts[t] = counts.get(t, 0) + 1
    return out, counts


def counts_of(records):
    c = {}
    for r in records:
        c[r["type"]] = c.get(r["type"], 0) + 1
    return c


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    tool = os.path.abspath(sys.argv[1])
    have_dcmtk = shutil.which("dcmmkdir") is not None and shutil.which("dcmdump") is not None
    try:
        import pydicom  # noqa: F401
        have_pydicom = True
    except ImportError:
        have_pydicom = False
    failures = 0

    def check(cond, what):
        nonlocal failures
        print(("ok    " if cond else "FAIL  ") + what)
        if not cond:
            failures += 1

    work = tempfile.mkdtemp(prefix="siaara-ddx-")
    try:
        src = os.path.join(work, "src")
        n = 64
        if run([tool, "make", src, str(n)]).returncode != 0:
            raise SystemExit("make failed")
        files = sorted(os.path.join(src, "IMAGES", f) for f in os.listdir(os.path.join(src, "IMAGES")))

        # ---- 1. SIAARA writes
        media = os.path.join(work, "media")
        p = run([tool, "write", media] + files)
        check(p.returncode == 0, "siaara writes a DICOMDIR for %d files" % n)
        dd = os.path.join(media, "DICOMDIR")
        recs, summ = siaara_read(tool, dd)
        check(summ["files"] == n and summ["unreachable"] == 0 and not summ["fallback"],
              "siaara reads its own DICOMDIR back (%d records)" % summ["records"])
        if have_dcmtk:
            d = run(["dcmdump", "-q", dd])
            check(d.returncode == 0 and "E:" not in d.stderr, "dcmdump parses the SIAARA DICOMDIR")
        else:
            print("skip  dcmdump (DCMTK not installed)")
        if have_pydicom:
            inst, counts = pydicom_view(dd)
            mine = {r["sop_uid"]: os.path.realpath(r["file"]) for r in recs if r["file"]}
            check(len(inst) == n, "pydicom FileSet loads %d instances (expected %d)" % (len(inst), n))
            check(inst == mine, "pydicom and siaara agree on every SOP Instance UID and file")
            check(counts == counts_of(recs), "pydicom record tree matches: %s" % sorted(counts.items()))
            import pydicom as pd
            bad = [u for u, f in inst.items() if str(pd.dcmread(f, stop_before_pixels=True).SOPInstanceUID) != u]
            check(not bad, "every referenced file holds the SOP Instance UID its record names")
        else:
            print("skip  pydicom FileSet (pydicom not installed)")
        if have_dcmtk:
            extra = os.path.join(work, "extra")
            run([tool, "make", extra, "1", "1000"])
            os.makedirs(os.path.join(media, "EXTRA"), exist_ok=True)
            shutil.copy(os.path.join(extra, "IMAGES", "IM001000"), os.path.join(media, "EXTRA", "IM001000"))
            a = run(["dcmmkdir", "+A", "+D", "DICOMDIR", "EXTRA/IM001000"], cwd=media)
            check(a.returncode == 0 and "E:" not in a.stderr, "dcmmkdir appends to the SIAARA DICOMDIR")
            recs2, summ2 = siaara_read(tool, dd)
            check(summ2["files"] == n + 1 and summ2["unreachable"] == 0,
                  "siaara reads the DCMTK-appended DICOMDIR (%d files)" % summ2["files"])

        # ---- 2. others write, SIAARA reads
        if have_dcmtk:
            fsroot = os.path.join(work, "fs-dcmtk")
            os.makedirs(os.path.join(fsroot, "IMAGES"))
            for f in files:
                shutil.copy(f, os.path.join(fsroot, "IMAGES", os.path.basename(f)))
            m = run(["dcmmkdir", "--recurse", "+F", "DCMTKSET", "IMAGES"], cwd=fsroot)
            check(m.returncode == 0, "dcmmkdir builds a DICOMDIR")
            recs, summ = siaara_read(tool, os.path.join(fsroot, "DICOMDIR"))
            check(summ["files"] == n and summ["unreachable"] == 0 and not summ["fallback"],
                  "siaara reads the dcmmkdir DICOMDIR (%d records, %d files)" % (summ["records"], summ["files"]))
            if have_pydicom:
                inst, counts = pydicom_view(os.path.join(fsroot, "DICOMDIR"))
                mine = {r["sop_uid"]: os.path.realpath(r["file"]) for r in recs if r["file"]}
                check(inst == mine, "siaara and pydicom agree on the dcmmkdir DICOMDIR")
                check(counts == counts_of(recs), "record types agree: %s" % sorted(counts.items()))
        if have_pydicom:
            from pydicom import dcmread
            from pydicom.fileset import FileSet
            out = os.path.join(work, "fs-pydicom")
            fset = FileSet()
            for f in files:
                fset.add(dcmread(f))
            fset.write(out)
            recs, summ = siaara_read(tool, os.path.join(out, "DICOMDIR"))
            inst, counts = pydicom_view(os.path.join(out, "DICOMDIR"))
            mine = {r["sop_uid"]: os.path.realpath(r["file"]) for r in recs if r["file"]}
            check(summ["files"] == n and not summ["fallback"], "siaara reads the pydicom DICOMDIR (%d files)" % summ["files"])
            check(inst == mine, "siaara and pydicom agree on the pydicom DICOMDIR")
            check(counts == counts_of(recs), "record types agree: %s" % sorted(counts.items()))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
