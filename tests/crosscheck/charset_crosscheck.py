#!/usr/bin/env python3
"""Cross-checks SIAARA character set decoding against pydicom (test-only tool, never shipped).

pydicom is used only as an oracle. For every file from pydicom.data.get_charset_files() (the
PS3.5 annex example files and other charset test files), each SH/LO/UC/ST/LT/UT/PN element,
including those inside sequence items, is decoded by charset_tool (SIAARA) and by pydicom, and
the UTF-8 text is compared. Usage:
    python3 charset_crosscheck.py <path to charset_tool>

Normalisation applied to both sides before comparing (presentation differences, not decoding):
  - values joined with '\\'; trailing spaces/NULs removed per value (pydicom also strips
    leading spaces of SH/LO/PN values, as SIAARA does);
  - PN: empty trailing component groups ('=' at the end) removed, as pydicom's PersonName
    drops them.
"""
import subprocess
import sys
import warnings

import pydicom
from pydicom.data import get_charset_files

VRS = {"SH", "LO", "UC", "ST", "LT", "UT", "PN"}


def norm(vr, text):
    if vr == "PN":
        text = "\\".join(v.rstrip("=") for v in text.split("\\"))
    return text


def pydicom_values(ds, prefix=""):
    out = {}
    for elem in ds:
        path = prefix + f"({elem.tag.group:04X},{elem.tag.element:04X})"
        if elem.VR == "SQ":
            for i, item in enumerate(elem.value):
                out.update(pydicom_values(item, f"{path}[{i}]."))
            continue
        if elem.VR not in VRS:
            continue
        v = elem.value
        if v is None:
            vals = []
        elif isinstance(v, (list, pydicom.multival.MultiValue)):
            vals = [str(x) for x in v]
        else:
            vals = [str(v)]
        if isinstance(v, bytes):
            vals = None  # pydicom left it undecoded
        out[path] = (elem.VR, None if vals is None else "\\".join(vals))
    return out


def main():
    tool = sys.argv[1]
    files = sorted(p for p in get_charset_files("*.dcm"))
    total_bad = total_rt = 0
    for path in files:
        name = path.replace("\\", "/").split("/")[-1]
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            ds = pydicom.dcmread(path)
            ds.decode()  # applies (0008,0005), inherited by sequence items
            ref = pydicom_values(ds)
            pyd_warn = len(w)
        r = subprocess.run([tool, path], capture_output=True)
        if r.returncode != 0:
            print(f"FAIL {name}: charset_tool exit {r.returncode}: {r.stderr.decode(errors='replace')[:200]}")
            total_bad += 1
            continue
        ours = {}
        for line in r.stdout.decode("ascii").splitlines():
            p, vr, hexval, rt = line.split("\t")
            ours[p] = (vr, "" if hexval == "-" else bytes.fromhex(hexval).decode("utf-8"), rt)
        same = diff = skipped = no_rt = 0
        msgs = []
        for p, (vr, text) in sorted(ref.items()):
            if p in ours and ours[p][0] == "UN":
                skipped += 1
                msgs.append(f"    skipped {p}: private element read as UN by SIAARA (implicit VR, no private dictionary)")
                continue
            if p in ours and ours[p][2] != "rt":
                no_rt += 1
                msgs.append(f"    {p}: re-encoding the decoded value did not reproduce the original bytes")
            if p not in ours:
                diff += 1
                msgs.append(f"    missing in SIAARA: {p}")
                continue
            if text is None:
                diff += 1
                msgs.append(f"    pydicom did not decode {p}")
                continue
            a, b = norm(vr, ours[p][1]), norm(vr, text)
            if a == b:
                same += 1
            else:
                diff += 1
                msgs.append(f"    {p} {vr}: siaara={a!r} pydicom={b!r}")
        extra = sorted(p for p in set(ours) - set(ref) if ours[p][0] != "UN")
        for p in extra:
            diff += 1
            msgs.append(f"    only in SIAARA: {p}")
        cs = ds.get("SpecificCharacterSet", "")
        status = "ok  " if diff == 0 else "DIFF"
        print(f"{status} {name:28s} {str(cs)!s:40s} {same:3d} match, {diff} differ, {skipped} skipped, "
              f"{same + diff - no_rt} of {same + diff} re-encode to the original bytes"
              + (f" (pydicom warnings: {pyd_warn})" if pyd_warn else ""))
        for m in msgs:
            print(m)
        total_bad += diff
        total_rt += no_rt
    print(f"\n{len(files)} files, {total_bad} differing elements, {total_rt} without byte-exact re-encoding")
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
