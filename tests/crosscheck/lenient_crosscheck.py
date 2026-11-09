#!/usr/bin/env python3
"""Lenient frame decoding cross-check (test-only, never shipped).

For every JPEG / JPEG-LS / JPEG 2000 file of the pydicom test set and of pylibjpeg-data, this
compares SIAARA's decoded frames (pixel_tool frames, lenient by default) with
pydicom.pixels.pixel_array(..., raw=True) backed by pylibjpeg (an oracle only; GDCM via
python-gdcm when pydicom cannot decode a file), and reports
whether the codestream header disagrees with the Image Pixel module and which frame warnings we
attach. With --before <older pixel_tool> it also reports what that build did with the file.

Values are compared as integers: ours are read with the Bits Stored / Pixel Representation that
pixel_tool info reports (the effective Bits Stored after lenient adjustments); pydicom's are its
raw output, unmasked.

Usage:
    python3 lenient_crosscheck.py <pixel_tool> [--before <old pixel_tool>] [--all] [--data DIR] [file ...]
      --all   list every encapsulated file (default: only files whose codestream header disagrees
              with the data set, or that either build refuses)
      --data  pylibjpeg-data checkout (default /tmp/pylibjpeg-data)
      --extra DIR  also scan DIR (repeatable), e.g. a gdcmData or pydicom-data checkout
    Files given explicitly are checked whatever their transfer syntax (gdcmData, pydicom-data ...).
"""
import glob
import os
import struct
import subprocess
import sys
import tempfile
import warnings

import numpy as np
import pydicom
from pydicom.data import get_testdata_files
from pydicom.encaps import generate_frames
from pydicom.pixels import pixel_array

warnings.simplefilter("ignore")
TMP = tempfile.mkdtemp(prefix="lenient_cc_")

J2K = {"1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.4.201",
       "1.2.840.10008.1.2.4.202", "1.2.840.10008.1.2.4.203"}
JLS = {"1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.81"}
JPEG = {"1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.57",
        "1.2.840.10008.1.2.4.70"}


def run(tool, *args):
    return subprocess.run([tool, *args], capture_output=True, text=True, errors="replace")


def info(tool, path):
    r = run(tool, "info", path)
    if r.returncode != 0:
        return None, r.stderr.strip()
    d, warns = {}, []
    for line in r.stdout.splitlines():
        k, _, v = line.partition("=")
        if k == "warning":
            warns.append(v)
        else:
            d.setdefault(k, v)
    d["warnings"] = warns
    return d, ""


def frames(tool, path, inf):
    out = os.path.join(TMP, "frames.bin")
    r = run(tool, "frames", path, out)
    if r.returncode != 0:
        return None, r.stderr.strip()
    with open(out, "rb") as f:
        data = f.read()
    rows, cols, spp = int(inf["rows"]), int(inf["columns"]), int(inf["spp"])
    ba, bs, nf = int(inf["bits_allocated"]), int(inf["bits_stored"]), int(inf["frames"])
    a = np.frombuffer(data, f"<u{ba // 8}").astype(np.int64)
    if spp > 1 and inf["planar"] == "1":
        a = a.reshape(nf, spp, rows, cols).transpose(0, 2, 3, 1)
    a = a.reshape((nf, rows, cols, spp) if spp > 1 else (nf, rows, cols))
    a &= (1 << bs) - 1
    if inf["signed"] == "1" and spp == 1:
        a = np.where(a >= (1 << (bs - 1)), a - (1 << bs), a)
    return a, ""


# ---- codestream headers (our own minimal parsers, for the report only)
def j2k_header(b):
    i = b.find(b"\xff\x4f\xff\x51")
    if i < 0:
        return None
    s = i + 4
    xs, ys, xo, yo = struct.unpack(">IIII", b[s + 4:s + 20])
    nc = struct.unpack(">H", b[s + 36:s + 38])[0]
    ssiz = b[s + 38]
    return {"w": xs - xo, "h": ys - yo, "c": nc, "p": (ssiz & 0x7F) + 1, "signed": bool(ssiz & 0x80)}


def jpeg_header(b):
    i = 2
    while i + 4 <= len(b):
        if b[i] != 0xFF:
            return None
        m = b[i + 1]
        if m in (0xD8, 0x01, 0xFF) or 0xD0 <= m <= 0xD7:
            i += 1 if m == 0xFF else 2
            continue
        n = struct.unpack(">H", b[i + 2:i + 4])[0]
        if m in (0xC0, 0xC1, 0xC2, 0xC3, 0xF7):
            p = b[i + 4]
            h, w = struct.unpack(">HH", b[i + 5:i + 9])
            return {"w": w, "h": h, "c": b[i + 9], "p": p, "signed": None, "sof": m}
        i += 2 + n
    return None


def mismatch(ds, ts):
    try:
        nf = int(ds.get("NumberOfFrames", 1) or 1)
        f0 = next(generate_frames(ds.PixelData, number_of_frames=nf))
    except Exception:  # noqa: BLE001
        return "?"
    h = j2k_header(f0) if ts in J2K else jpeg_header(f0)
    if not h:
        return "no header"
    if any(k not in ds for k in ("Rows", "Columns", "SamplesPerPixel", "BitsAllocated", "BitsStored",
                                 "PixelRepresentation")):
        return "Image Pixel attributes missing"
    out = []
    if h["w"] != ds.Columns or (h["h"] and h["h"] != ds.Rows):
        out.append(f"size {h['w']}x{h['h']} vs {ds.Columns}x{ds.Rows}")
    if h["c"] != ds.SamplesPerPixel:
        out.append(f"components {h['c']} vs {ds.SamplesPerPixel}")
    if h["p"] != ds.BitsStored:
        out.append(f"P {h['p']} vs BS {ds.BitsStored}")
    if h["p"] > ds.BitsAllocated:
        out.append(f"P {h['p']} > BA {ds.BitsAllocated}")
    if h["signed"] is not None and int(h["signed"]) != ds.PixelRepresentation:
        out.append(f"signed {int(h['signed'])} vs PR {ds.PixelRepresentation}")
    return ", ".join(out)


def oracle_pydicom(path, inf):
    arr = pixel_array(path, raw=True)
    nf, spp = int(inf["frames"]), int(inf["spp"])
    shape = (nf,) + (arr.shape[-3:] if spp > 1 else arr.shape[-2:])
    return arr.reshape(shape).astype(np.int64)


def oracle_gdcm(path, inf):
    import gdcm  # optional second oracle (python-gdcm), test-only
    r = gdcm.ImageReader()
    r.SetFileName(path)
    if not r.Read():
        raise RuntimeError("GDCM cannot read")
    img = r.GetImage()
    pf = img.GetPixelFormat()
    buf = img.GetBuffer()
    if buf is None:
        raise RuntimeError("GDCM cannot decode")
    b = buf.encode("utf-8", "surrogateescape")
    nf, spp = int(inf["frames"]), int(inf["spp"])
    rows, cols = int(inf["rows"]), int(inf["columns"])
    n = nf * rows * cols * spp
    if n == 0 or len(b) % n:
        raise RuntimeError(f"GDCM buffer of {len(b)} bytes")
    w = len(b) // n
    a = np.frombuffer(b, {1: "u1", 2: "<u2", 4: "<u4"}[w]).astype(np.int64)
    if pf.GetPixelRepresentation() == 1:
        a = np.where(a >= 1 << (8 * w - 1), a - (1 << (8 * w)), a)
    return a.reshape((nf, rows, cols, spp) if spp > 1 else (nf, rows, cols))


def oracle(path, inf):
    """pydicom first, GDCM when pydicom cannot decode the file; returns (array, oracle name)."""
    try:
        return oracle_pydicom(path, inf), "pydicom"
    except Exception as e:  # noqa: BLE001
        first = e
    try:
        return oracle_gdcm(path, inf), "GDCM"
    except ImportError:
        raise first


def compare(a, b):
    if a.shape != b.shape:
        return f"shape {a.shape} vs {b.shape}"
    d = np.abs(a - b)
    n = int(np.count_nonzero(d))
    return "match" if n == 0 else f"{n}/{a.size} differ, max |diff| {int(d.max())}"


def outcome(tool, path):
    inf, err = info(tool, path)
    if inf is None:
        return "refused", err, None, None
    a, err = frames(tool, path, inf)
    if a is None:
        return "refused", err, inf, None
    return "opens", "", inf, a


def main():
    args = sys.argv[1:]
    tool = args.pop(0)
    before = None
    show_all = False
    data = "/tmp/pylibjpeg-data"
    extra_dirs = []
    files = []
    while args:
        a = args.pop(0)
        if a == "--before":
            before = args.pop(0)
        elif a == "--all":
            show_all = True
        elif a == "--data":
            data = args.pop(0)
        elif a == "--extra":
            extra_dirs.append(args.pop(0))
        else:
            files.append(a)
    explicit = bool(files)
    if not files:
        seen = {}
        extra = []
        for d in extra_dirs:  # gdcmData, pydicom-data checkouts: every file, any extension
            extra += sorted(q for q in glob.glob(os.path.join(d, "**/*"), recursive=True)
                            if os.path.isfile(q) and "/.git/" not in q)
        for p in sorted(get_testdata_files("*")) + sorted(glob.glob(os.path.join(data, "ljdata/ds/**/*.dcm"),
                                                                     recursive=True)) + extra:
            seen.setdefault(os.path.basename(p), p)  # one copy of files present in both sets
        files = list(seen.values())
    counts = {}
    for p in files:
        try:
            ds = pydicom.dcmread(p, force=True)
            ts = str(ds.file_meta.TransferSyntaxUID) if "TransferSyntaxUID" in ds.file_meta else "?"
        except Exception:  # noqa: BLE001
            if not explicit:
                continue
            ds, ts = None, "?"
        if ds is None:
            st, err, inf, a = outcome(tool, p)
            print(f"{os.path.basename(p)}\n    pydicom cannot parse it; ours: {st} {err[:80]}")
            continue
        if not explicit and ("PixelData" not in ds or ts not in J2K | JLS | JPEG):
            continue
        mm = mismatch(ds, ts) if ts in J2K | JLS | JPEG and "PixelData" in ds else ""
        b_state = ""
        if before:
            st, err, _, _ = outcome(before, p)
            b_state = "opens" if st == "opens" else "refused: " + err[:70]
        st, err, inf, a = outcome(tool, p)
        try:
            ref, oname = oracle(p, inf) if inf else (None, "")
            perr = ""
        except Exception as e:  # noqa: BLE001
            ref, oname, perr = None, "", str(e).splitlines()[0][:70]
        if st != "opens":
            verdict = "refused (" + err[:90] + ")"
            key = "refused" + ("; pydicom also fails" if ref is None and inf else "")
        elif ref is None:
            verdict = "opens; no oracle decodes it: " + perr
            key = "opens, no oracle"
        else:
            c = compare(a, ref)
            verdict = "opens, " + ("matches " + oname if c == "match" else "differs from " + oname + ": " + c)
            key = "opens, matches" if c == "match" else "opens, differs"
        interesting = bool(mm) or st != "opens" or (before and not b_state.startswith("opens"))
        if not (interesting or show_all):
            continue
        counts[key] = counts.get(key, 0) + 1
        warn = "; ".join(inf["warnings"]) if inf else ""
        bs = f" BS {inf['declared_bits_stored'] if 'declared_bits_stored' in inf else ds.get("BitsStored")}->{inf['bits_stored']}" \
            if inf and "BitsStored" in ds and int(inf["bits_stored"]) != int(ds.BitsStored) else ""
        print(f"{os.path.basename(p)}\n    header: {mm or 'consistent'}")
        if before:
            print(f"    before: {b_state}")
        print(f"    after:  {verdict}{bs}")
        if warn:
            print(f"    warnings: {warn}")
    print("\n" + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items())))


if __name__ == "__main__":
    main()
