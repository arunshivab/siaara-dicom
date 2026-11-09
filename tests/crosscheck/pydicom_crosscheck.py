#!/usr/bin/env python3
"""Cross-checks the SIAARA reader against pydicom (test-only tool, never shipped).

For each pydicom built-in test file, dumps it with siaara-dump and checks that every top-level
element pydicom sees is also present in SIAARA's output. Usage:
    python3 pydicom_crosscheck.py <path to siaara-dump>
"""
import subprocess
import sys

import pydicom
from pydicom.data import get_testdata_files

dump = sys.argv[1]
files = sorted(get_testdata_files("*.dcm"))
ok = bad = skipped = 0
for path in files:
    try:
        ds = pydicom.dcmread(path, force=True)
    except Exception:
        skipped += 1
        continue
    r = subprocess.run([dump, path], capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        bad += 1
        print(f"FAIL parse  {path}: {r.stderr.strip()[:200]}")
        continue
    out = r.stdout
    missing = []
    for elem in ds:
        tag = f"({elem.tag.group:04X},{elem.tag.element:04X})"
        if tag not in out:
            missing.append(tag)
    if missing:
        bad += 1
        print(f"FAIL elems  {path}: missing {missing[:8]}")
    else:
        ok += 1
print(f"\n{ok} files match, {bad} differ, {skipped} skipped (pydicom could not read)")
sys.exit(1 if bad else 0)
