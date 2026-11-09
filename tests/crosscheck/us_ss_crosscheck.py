#!/usr/bin/env python3
""""US or SS" cross-check against pydicom (test-only tool, never shipped; testing-chat F8).

For every file under the given folders (and pydicom's own test files) that pydicom reads, this
compares the values of the elements whose VR is "US or SS" by Pixel Representation (Smallest /
Largest Image Pixel Value, Pixel Padding Value, LUT and palette descriptors, ...), at every
nesting level, between SIAARA (siaara_dataset_get_int64 through ctypes) and pydicom. pydicom
reads an implicit VR file with SS when Pixel Representation is 1. Known, intended difference:
pydicom reads all three LUT Descriptor values as SS, SIAARA keeps the number of entries and the
bits per entry unsigned (PS3.3 C.11.1.1.1); entry counts above 32767 are therefore compared with
pydicom's value + 65536. Usage:
    python3 us_ss_crosscheck.py <libsiaara.so> [folder ...] [-v]
"""
import ctypes
import glob
import os
import sys
import warnings

import pydicom
from pydicom.data import get_testdata_files

warnings.simplefilter("ignore")
if len(sys.argv) < 2:
    print(__doc__)
    sys.exit(2)
lib = ctypes.CDLL(os.path.abspath(sys.argv[1]))
verbose = "-v" in sys.argv[2:]
folders = [a for a in sys.argv[2:] if a != "-v"]
lib.siaara_file_open.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
lib.siaara_file_dataset.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
lib.siaara_dataset_get_int64.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_int64)]
lib.siaara_dataset_item.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
lib.siaara_file_free.argtypes = [ctypes.c_void_p]

US_SS = {0x00189810, 0x00221452, 0x00280071, 0x00280104, 0x00280105, 0x00280106, 0x00280107, 0x00280108, 0x00280109,
         0x00280110, 0x00280111, 0x00280120, 0x00280121, 0x00409211, 0x00409216, 0x00603004, 0x00603006}
DESCRIPTORS = {0x00283002, 0x00281100, 0x00281101, 0x00281102, 0x00281103, 0x00281111, 0x00281112, 0x00281113}

paths = list(get_testdata_files("*"))
for f in folders:
    paths += [p for p in glob.glob(os.path.join(f, "**", "*"), recursive=True) if os.path.isfile(p) and "/.git" not in p]

files = compared = mismatched = 0


def get(ds, tag, i):
    v = ctypes.c_int64()
    return v.value if lib.siaara_dataset_get_int64(ds, tag, i, ctypes.byref(v)) == 0 else None


def walk(pyds, ours, where, out):
    for elem in pyds:
        t = int(elem.tag)
        if elem.VR == "SQ":
            for k, it in enumerate(elem.value):
                child = ctypes.c_void_p()
                if lib.siaara_dataset_item(ours, t, k, ctypes.byref(child)) == 0:
                    walk(it, child, f"{where}({t:08X})[{k}]", out)
            continue
        if t not in US_SS and t not in DESCRIPTORS:
            continue
        vals = elem.value
        if vals is None or vals == "" or isinstance(vals, (bytes, bytearray)):
            continue
        vals = list(vals) if isinstance(vals, (list, tuple, pydicom.multival.MultiValue)) else [vals]
        for i, v in enumerate(vals):
            exp = int(v)
            if t in DESCRIPTORS and i != 1 and exp < 0:
                exp += 65536  # number of entries / bits per entry: unsigned (PS3.3 C.11.1.1.1)
            got = get(ours, t, i)
            out.append((f"{where}({t:08X})[{i}]", exp, got))


for p in sorted(set(paths)):
    try:
        d = pydicom.dcmread(p, force=True)
        _ = list(d.iterall())
    except Exception:
        continue
    f = ctypes.c_void_p()
    bp = p.encode()
    if lib.siaara_file_open(bp, len(bp), None, ctypes.byref(f)) != 0:
        continue
    ds = ctypes.c_void_p()
    lib.siaara_file_dataset(f, ctypes.byref(ds))
    out = []
    try:
        walk(d, ds, "", out)
    except Exception:
        out = []
    lib.siaara_file_free(f)
    if not out:
        continue
    files += 1
    pr = d.get("PixelRepresentation")
    ts = str(getattr(getattr(d, "file_meta", None), "TransferSyntaxUID", "") or "")
    for where, exp, got in out:
        compared += 1
        if exp != got:
            mismatched += 1
            print(f"DIFF {os.path.basename(p)} PR={pr} TS={ts} {where}: pydicom {exp}, siaara {got}")
        elif verbose:
            print(f"ok   {os.path.basename(p)} PR={pr} TS={ts} {where}: {exp}")
print(f"{files} files with US/SS elements, {compared} values compared, {mismatched} differ")
sys.exit(1 if mismatched else 0)
