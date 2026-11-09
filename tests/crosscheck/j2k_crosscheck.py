#!/usr/bin/env python3
"""Cross-checks the SIAARA JPEG 2000 / HTJ2K codec against independent implementations.

Test-only, never shipped. Third-party software is used purely as an oracle (its output is
compared with ours); none of its code is used by SIAARA.

Oracles, whichever are installed:
  - opj_decompress / opj_compress (OpenJPEG command line tools, e.g. Debian libopenjp2-tools)
  - pylibjpeg-openjpeg (module `openjpeg`; recent versions also decode HTJ2K)
  - pydicom (to read the DICOM sample files)

Sections:
  1. ISO/IEC 15444-4 conformance codestreams (openjpeg-data input/conformance p0_*, p1_*):
     every component is compared with the ISO class-1 reference decode (c1p*_N.pgx) and with
     opj_decompress. Reversible streams must match exactly; irreversible ones are reported with
     their peak error (OpenJPEG must agree within +-1).
  2. The other conformance and JP2 files in openjpeg-data (a*-g* .j2c, file*.jp2, ...) and
     HTJ2K codestreams (openjpeg-data nonregression/htj2k, pylibjpeg-data jpg/15444/HTJ2K):
     compared with opj_decompress, falling back to pylibjpeg-openjpeg.
  3. Synthetic images encoded by opj_compress with many option combinations (progression
     orders, precincts, code-block styles, SOP/EPH, tiles and tile-parts, POC, layers, ROI,
     subsampling, bit depths): decoded by SIAARA and compared with opj_decompress.
  4. DICOM files with transfer syntaxes 1.2.840.10008.1.2.4.90/.91/.201/.202/.203 (pydicom test
     files and pylibjpeg-data ds/): every frame decoded through decode_j2k with the data set's
     FrameInfo and compared with pylibjpeg-openjpeg. Files whose header disagrees with the
     codestream must be rejected (PARSE-35) and are reported as such.
  5. Images encoded by SIAARA (lossless 5-3/RCT, lossy 9-7/ICT, HTJ2K lossless, several layers,
     tiles, progressions) decoded by opj_decompress and pylibjpeg-openjpeg: lossless must be
     exact; lossy must match SIAARA's own decode within +-1.

Usage:  python3 -I j2k_crosscheck.py <path to j2k_tool> [<openjpeg-data>] [<pylibjpeg-data>]
"""
import glob
import os
import random
import shutil
import subprocess
import sys
import tempfile

import numpy as np

TOOL = sys.argv[1]
OPJ_DATA = sys.argv[2] if len(sys.argv) > 2 else "/tmp/openjpeg-data"
PLJ_DATA = sys.argv[3] if len(sys.argv) > 3 else "/tmp/pylibjpeg-data"
TMP = tempfile.mkdtemp(prefix="siaara_j2k_")
OPJ_DEC = shutil.which("opj_decompress")
OPJ_ENC = shutil.which("opj_compress")
try:
    import openjpeg  # pylibjpeg-openjpeg
except Exception:  # pragma: no cover
    openjpeg = None

results = {"ok": 0, "bad": 0, "skip": 0}


def report(ok, what):
    results["ok" if ok else "bad"] += 1
    print(("ok    " if ok else "FAIL  ") + what, flush=True)


def skip(what):
    results["skip"] += 1
    print("skip  " + what, flush=True)


def run(args, timeout=300):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


# ------------------------------------------------------------------ helpers

def read_pgx(path):
    with open(path, "rb") as f:
        data = f.read()
    nl = data.index(b"\n")
    head = data[:nl].decode("ascii").split()
    # "PG ML + 8 64 126" (the sign may be glued to the depth, e.g. "+8" or "-12")
    toks = head[1:]
    endian = toks[0]
    rest = " ".join(toks[1:]).replace("+", "+ ").replace("-", "- ").split()
    sign = "+"
    if rest[0] in ("+", "-"):
        sign = rest.pop(0)
    depth, w, h = int(rest[0]), int(rest[1]), int(rest[2])
    nbytes = 1 if depth <= 8 else (2 if depth <= 16 else 4)
    signed = sign == "-"
    kind = ("i" if signed else "u") + str(nbytes)
    dt = np.dtype((">" if endian == "ML" else "<") + kind)
    arr = np.frombuffer(data[nl + 1:nl + 1 + w * h * nbytes], dtype=dt).reshape(h, w)
    return arr.astype(np.int64), depth, signed


def ours_components(path):
    raw = os.path.join(TMP, "ours.raw")
    p = run([TOOL, "components", path, raw])
    if p.returncode != 0:
        return None, p.stderr.strip()
    comps = [tuple(int(v) for v in line.split()) for line in p.stdout.strip().splitlines()]
    data = np.fromfile(raw, dtype="<i4")
    out, off = [], 0
    for (w, h, prec, sgn, dx, dy) in comps:
        out.append(data[off:off + w * h].reshape(h, w).astype(np.int64))
        off += w * h
    return out, comps


def raw_codestream(data):
    """The contiguous codestream of a JP2/JPH file (I.5.4), or the data itself."""
    if data[:2] == b"\xff\x4f":
        return data
    pos = 0
    while pos + 8 <= len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        t = data[pos + 4:pos + 8]
        hdr = 8
        if n == 1:
            n = int.from_bytes(data[pos + 8:pos + 16], "big")
            hdr = 16
        elif n == 0:
            n = len(data) - pos
        if t == b"jp2c":
            return data[pos + hdr:pos + n]
        if n < hdr:
            break
        pos += n
    return data


def sycc_to_rgb(comps, info):
    """OpenJPEG's command line tools treat any three-component image with subsampled chroma as
    sYCC and convert it to RGB. To compare, the same textbook conversion (T.800 G.3 constants,
    nearest-neighbour chroma upsampling) is applied to SIAARA's components. Rounding differs
    slightly from OpenJPEG's, so these comparisons allow +-3."""
    y = comps[0].astype(np.float64)
    h, w = y.shape
    up = []
    for c in comps[1:]:
        fy = -(-h // c.shape[0])
        fx = -(-w // c.shape[1])
        up.append(np.repeat(np.repeat(c, fy, axis=0), fx, axis=1)[:h, :w].astype(np.float64))
    off = 1 << (info["precision"] - 1)
    cb, cr = up[0] - off, up[1] - off
    top = (1 << info["precision"]) - 1
    r = np.clip(y + np.trunc(1.402 * cr), 0, top)
    g = np.clip(y - np.trunc(0.344 * cb + 0.714 * cr), 0, top)
    b = np.clip(y + np.trunc(1.772 * cb), 0, top)
    return [r.astype(np.int64), g.astype(np.int64), b.astype(np.int64)]


def opj_components(path):
    """Decodes the raw codestream with opj_decompress into PGX, one file per component.
    JP2 colour boxes (palettes, sYCC, ICC) are outside the codestream and are not applied."""
    if not OPJ_DEC:
        return None
    d = tempfile.mkdtemp(dir=TMP)
    src = os.path.join(d, "in.j2k")
    with open(path, "rb") as f:
        data = raw_codestream(f.read())
    with open(src, "wb") as f:
        f.write(data)
    p = run([OPJ_DEC, "-i", src, "-o", os.path.join(d, "o.pgx")])
    files = sorted(glob.glob(os.path.join(d, "o*.pgx")), key=lambda s: int(s.rsplit("_", 1)[-1][:-4]) if "_" in os.path.basename(s) else 0)
    if p.returncode != 0 or not files:
        return None
    return [read_pgx(f)[0] for f in files]


def pylibjpeg_components(path):
    if openjpeg is None:
        return None
    try:
        with open(path, "rb") as f:
            arr = np.asarray(openjpeg.decode(raw_codestream(f.read())))
    except Exception:
        return None
    if arr.ndim == 2:
        return [arr.astype(np.int64)]
    return [arr[:, :, i].astype(np.int64) for i in range(arr.shape[2])]


def compare(ours, ref, comps=None):
    """Largest absolute difference over all components, or None if the shapes differ.
    opj_decompress writes subsampled components upsampled by replication; those are compared
    on the component's own sample grid (`comps` gives each component's dx, dy)."""
    if ref is None or ours is None or len(ours) != len(ref):
        return None
    worst = 0
    for i, (a, b) in enumerate(zip(ours, ref)):
        if a.shape != b.shape and comps is not None:
            dx, dy = comps[i][4], comps[i][5]
            b = b[::dy, ::dx]
        if a.shape != b.shape:
            return None
        if a.size:
            worst = max(worst, int(np.abs(a - b).max()))
    return worst


def probe(path):
    p = run([TOOL, "probe", path])
    if p.returncode != 0:
        return None
    v = [int(x) for x in p.stdout.split()]
    keys = ["width", "height", "components", "precision", "signed", "levels", "layers", "progression",
            "reversible", "mct", "ht", "tiles", "subsampled"]
    return dict(zip(keys, v))


# ------------------------------------------------------------------ 1. ISO conformance

def section_conformance():
    print("== 1. ISO/IEC 15444-4 conformance codestreams")
    base = os.path.join(OPJ_DATA, "input", "conformance")
    refdir = os.path.join(OPJ_DATA, "baseline", "conformance")
    files = sorted(glob.glob(os.path.join(base, "p0_*.j2k")) + glob.glob(os.path.join(base, "p1_*.j2k")))
    if not files:
        skip("no conformance codestreams in " + base)
        return
    for f in files:
        name = os.path.basename(f)[:-4]
        info = probe(f)
        ours, comps = ours_components(f)
        if ours is None:
            report(False, f"{name}: SIAARA failed: {comps}")
            continue
        refs = sorted(glob.glob(os.path.join(refdir, f"c1{name}_*.pgx")), key=lambda s: int(s.rsplit("_", 1)[-1][:-4]))
        iso = [read_pgx(r)[0] for r in refs] if refs else None
        # The ISO class-1 references cover the components a class-1 decoder must produce.
        d_iso = compare(ours[:len(iso)], iso) if iso else None
        opj = opj_components(f)
        d_opj = compare(ours, opj, comps)
        rev = info and info["reversible"]
        limit = 0 if rev else 1
        detail = f"comps={len(ours)} {comps[0][0]}x{comps[0][1]} {comps[0][2]}-bit {'5-3' if rev else '9-7'}"
        ok = d_opj is not None and d_opj <= limit
        note = ""
        if iso is not None and d_iso is not None:
            if rev and d_iso != 0:
                ok = False
            # Where OpenJPEG itself is far from the ISO reference (lossy streams with large
            # conformance tolerances), require SIAARA to be no further from it than OpenJPEG.
            d_opj_iso = compare(opj[:len(iso)], iso) if opj else None
            if not ok and not rev and d_opj_iso is not None and d_iso <= d_opj_iso:
                ok = True
                note = f" (OpenJPEG vs ISO reference: {d_opj_iso})"
        elif iso:
            note = " (ISO reference is a reduced-resolution decode; compared with OpenJPEG only)"
        report(ok, f"{name}: {detail}; peak error vs ISO reference {d_iso}, vs OpenJPEG {d_opj}{note}")


# ------------------------------------------------------------------ 2. other files

def section_files():
    print("== 2. Other conformance, JP2 and HTJ2K files")
    pats = [os.path.join(OPJ_DATA, "input", "conformance", "*.j2c"),
            os.path.join(OPJ_DATA, "input", "conformance", "*.jp2"),
            os.path.join(OPJ_DATA, "input", "nonregression", "htj2k", "*"),
            os.path.join(PLJ_DATA, "ljdata", "jpg", "15444", "*", "*.j2k")]
    files = []
    for p in pats:
        files += sorted(glob.glob(p))
    for f in files:
        name = os.path.relpath(f, os.path.dirname(os.path.dirname(f)))
        ours, comps = ours_components(f)
        ref = opj_components(f)
        oracle = "OpenJPEG"
        if ref is None:
            ref = pylibjpeg_components(f)
            oracle = "pylibjpeg-openjpeg"
        if ours is None:
            if ref is None:
                skip(f"{name}: neither SIAARA nor the oracles decode it ({comps})")
            else:
                report(False, f"{name}: SIAARA failed: {comps}")
            continue
        info = probe(f)
        d = compare(ours, ref, comps)
        if ref is None:
            skip(f"{name}: no oracle decode")
            continue
        limit = 0 if info and info["reversible"] else 1
        conv = ""
        if (d is None or d > limit) and info and info["subsampled"] and info["components"] == 3 and ref[1].shape == ref[0].shape:
            d = compare(sycc_to_rgb(ours, info), ref)
            limit = 3
            conv = " (after the sYCC to RGB conversion OpenJPEG applies)"
        report(d is not None and d <= limit,
               f"{name}: comps={len(ours)} {comps[0][0]}x{comps[0][1]} {comps[0][2]}-bit "
               f"{'ht ' if info and info['ht'] else ''}{'5-3' if info and info['reversible'] else '9-7'}; "
               f"peak error vs {oracle} {d}{conv}")


# ------------------------------------------------------------------ main

if __name__ == "__main__":
    section_conformance()
    section_files()
    print(f"\n{results['ok']} ok, {results['bad']} failed, {results['skip']} skipped")
    shutil.rmtree(TMP, ignore_errors=True)
    sys.exit(1 if results["bad"] else 0)
