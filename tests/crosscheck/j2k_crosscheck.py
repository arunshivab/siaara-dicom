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


def _limit_memory():
    import resource
    resource.setrlimit(resource.RLIMIT_AS, (3 << 30, 3 << 30))


def run_oracle(args, timeout=300):
    """Oracle processes get a 3 GiB address-space limit: some corrupt files make them allocate
    huge amounts of memory. (SIAARA's own tool may be an ASan build, which cannot run under an
    address-space limit; its memory use is bounded by the library itself.)"""
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout, preexec_fn=_limit_memory)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(args, 1, "", "timeout")


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
    p = run_oracle([OPJ_DEC, "-i", src, "-o", os.path.join(d, "o.pgx")])
    files = sorted(glob.glob(os.path.join(d, "o*.pgx")), key=lambda s: int(s.rsplit("_", 1)[-1][:-4]) if "_" in os.path.basename(s) else 0)
    out = [read_pgx(f)[0] for f in files] if p.returncode == 0 and files else None
    shutil.rmtree(d, ignore_errors=True)
    return out


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



# ------------------------------------------------------------------ 2b. non-regression corpus

def section_nonregression():
    print("== 2b. OpenJPEG non-regression corpus (many deliberately corrupt files)")
    base = os.path.join(OPJ_DATA, "input", "nonregression")
    files = sorted(f for f in glob.glob(os.path.join(base, "*"))
                   if os.path.splitext(f)[1].lower() in (".j2k", ".jp2", ".j2c", ".jpc", ".jpx", ".jph", ".jhc"))
    if not files:
        skip("no non-regression corpus")
        return
    for f in files:
        name = os.path.basename(f)
        raw = os.path.join(TMP, "nr.raw")
        try:
            p = run([TOOL, "components", f, raw], timeout=120)
        except subprocess.TimeoutExpired:
            report(False, f"{name}: SIAARA timed out")
            continue
        sanitizer = "Sanitizer" in p.stderr or "runtime error" in p.stderr
        if p.returncode < 0 or sanitizer:
            report(False, f"{name}: SIAARA crashed or tripped a sanitizer: {p.stderr.strip()[-200:]}")
            continue
        ref = opj_components(f)
        if p.returncode != 0:
            msg = p.stderr.strip().splitlines()[-1] if p.stderr.strip() else "?"
            if ref is None:
                report(True, f"{name}: rejected by both ({msg})")
            else:
                # OpenJPEG is more lenient with some corrupt files; this is not a crash.
                report(True, f"{name}: rejected by SIAARA ({msg}); OpenJPEG decodes it [lenient-oracle]")
            continue
        ours, comps = ours_components(f)
        info = probe(f)
        if ref is None:
            report(True, f"{name}: decoded by SIAARA only (OpenJPEG rejects it)")
            continue
        d = compare(ours, ref, comps)
        limit = 0 if info and info["reversible"] else (2 if info and info["precision"] > 12 else 1)
        if d is not None and d > limit and info and info["subsampled"] and info["components"] == 3 and ref[1].shape == ref[0].shape:
            d2 = compare(sycc_to_rgb(ours, info), ref)
            if d2 is not None and d2 <= 3:
                d, limit = d2, 3
        tag = "" if d is not None and d <= limit else " [differs]"
        # Differences on corrupt input are reported but only crashes count as failures here.
        report(True, f"{name}: both decode; peak error vs OpenJPEG {d}{tag}")


# ------------------------------------------------------------------ 3. opj_compress streams

def synth_image(rng, w, h, nc, bits, signed):
    yy, xx = np.mgrid[0:h, 0:w]
    planes = []
    top = (1 << bits) - 1
    for c in range(nc):
        base = (np.sin(xx / (7.0 + 3 * c)) + np.cos(yy / (5.0 + c)) + 2) / 4 * top
        noise = rng.normal(0, top / 40.0 + 0.5, (h, w))
        edge = ((xx // 9 + yy // 13) % 2) * top / 6
        v = np.clip(base + noise + edge, 0, top).astype(np.int64)
        if signed:
            v -= 1 << (bits - 1)
        planes.append(v)
    return planes


def write_raw(path, planes, bits, signed):
    """opj_compress -F raw input: component after component, big-endian when bits > 8."""
    dt = (">i2" if signed else ">u2") if bits > 8 else ("i1" if signed else "u1")
    with open(path, "wb") as f:
        for p in planes:
            f.write(p.astype(dt).tobytes())


def section_opj_streams():
    print("== 3. Codestreams written by opj_compress")
    if not OPJ_ENC or not OPJ_DEC:
        skip("opj_compress / opj_decompress not installed")
        return
    rng = np.random.default_rng(1234)
    cases = [
        # (w, h, nc, bits, signed, extra opj_compress arguments)
        (64, 64, 1, 8, False, []),
        (97, 61, 1, 8, False, ["-I"]),
        (128, 96, 3, 8, False, []),
        (128, 96, 3, 8, False, ["-I", "-r", "20,10,5"]),
        (77, 53, 1, 12, False, ["-n", "4"]),
        (77, 53, 1, 16, False, []),
        (64, 48, 1, 16, True, []),
        (64, 48, 1, 12, True, ["-I", "-r", "8"]),
        (1, 1, 1, 8, False, ["-n", "1"]),
        (1, 37, 1, 8, False, ["-n", "1"]),
        (37, 1, 1, 8, False, ["-n", "1"]),
        (7, 13, 1, 8, False, ["-n", "3"]),
        (513, 511, 1, 12, False, ["-n", "7"]),
        (200, 150, 1, 8, False, ["-t", "64,48"]),
        (200, 150, 3, 8, False, ["-t", "100,64", "-TP", "R"]),
        (200, 150, 3, 8, False, ["-t", "64,64", "-TP", "C", "-p", "CPRL"]),
        (200, 150, 1, 8, False, ["-d", "13,7", "-T", "5,3", "-t", "50,40"]),
        (160, 120, 3, 8, False, ["-p", "RLCP", "-r", "40,20,10,1"]),
        (160, 120, 3, 8, False, ["-p", "RPCL", "-c", "[64,64],[32,32],[16,16]"]),
        (160, 120, 3, 8, False, ["-p", "PCRL", "-c", "[32,32],[32,32],[16,16]", "-r", "30,10,3"]),
        (160, 120, 3, 8, False, ["-p", "CPRL", "-c", "[128,64],[64,32]", "-r", "30,10"]),
        (160, 120, 1, 8, False, ["-b", "16,16"]),
        (160, 120, 1, 8, False, ["-b", "64,16"]),
        (160, 120, 1, 8, False, ["-b", "4,1024"]),
        (160, 120, 1, 8, False, ["-SOP", "-EPH", "-r", "20,5"]),
        (160, 120, 1, 8, False, ["-M", "1", "-r", "20,5,1"]),
        (160, 120, 1, 8, False, ["-M", "2"]),
        (160, 120, 1, 8, False, ["-M", "4", "-r", "15,4"]),
        (160, 120, 1, 8, False, ["-M", "8"]),
        (160, 120, 1, 8, False, ["-M", "16"]),
        (160, 120, 1, 8, False, ["-M", "32"]),
        (160, 120, 1, 12, False, ["-M", "63", "-r", "20,5"]),
        (160, 120, 3, 8, False, ["-M", "5", "-I", "-r", "30,10"]),
        (160, 120, 3, 8, False, ["-POC", "T1=0,0,1,3,2,LRCP/T1=0,0,1,6,3,RLCP", "-r", "30,10,1", "-n", "6"]),
        (160, 120, 3, 8, False, ["-POC", "T0=0,0,3,6,3,CPRL/T0=0,0,3,6,3,RPCL", "-r", "40,10,3", "-t", "80,60"]),
        (160, 120, 1, 8, False, ["-ROI", "c=0,U=6"]),
        (160, 120, 3, 8, False, ["-ROI", "c=1,U=10", "-I", "-r", "10"]),
        (160, 120, 3, 8, False, ["-mct", "0"]),
        (160, 120, 3, 8, False, ["-s", "2,2", "-mct", "0"]),
        (162, 121, 2, 8, False, ["-s", "2,1", "-mct", "0"]),
        (161, 122, 2, 12, False, ["-s", "1,2", "-mct", "0", "-I"]),
        (160, 120, 1, 8, False, ["-PLT", "-TLM"]),
        (160, 120, 1, 8, False, ["-GuardBits", "1"]),
        (160, 120, 1, 8, False, ["-GuardBits", "5", "-I"]),
        (300, 200, 1, 16, False, ["-I", "-q", "50,60"]),
        (120, 90, 1, 8, False, ["-n", "1"]),
    ]
    for i, (w, h, nc, bits, signed, extra) in enumerate(cases):
        subsampled = "-s" in extra
        if subsampled:
            sx, sy = (int(v) for v in extra[extra.index("-s") + 1].split(","))
            sub = ":".join(["1x1"] + [f"{sx}x{sy}"] * (nc - 1))
            extra = [a for j, a in enumerate(extra) if a != "-s" and (j == 0 or extra[j - 1] != "-s")]
            planes = synth_image(rng, w, h, 1, bits, signed)
            for _ in range(nc - 1):
                planes += synth_image(rng, -(-w // sx), -(-h // sy), 1, bits, signed)
        else:
            sub = ":".join(["1x1"] * nc)
            planes = synth_image(rng, w, h, nc, bits, signed)
        raw = os.path.join(TMP, f"s{i}.raw")
        write_raw(raw, planes, bits, signed)
        stream = os.path.join(TMP, f"s{i}.j2k")
        fmt = f"{w},{h},{nc},{bits},{'s' if signed else 'u'}@{sub}"
        p = run([OPJ_ENC, "-i", raw, "-o", stream, "-F", fmt] + extra)
        desc = f"{w}x{h}x{nc} {bits}-bit{' signed' if signed else ''} {' '.join(extra)}{' subsampled ' + sub if subsampled else ''}"
        if p.returncode != 0 or not os.path.exists(stream):
            skip(f"opj_compress refused: {desc}: {p.stderr.strip()[-80:]}")
            continue
        ours, comps = ours_components(stream)
        if ours is None:
            report(False, f"{desc}: SIAARA failed: {comps}")
            continue
        ref = opj_components(stream)
        d = compare(ours, ref, comps)
        lossy = "-I" in extra or "-r" in extra or "-q" in extra
        # Lossy: +-1, except above 12 bits where OpenJPEG's 9-7 reconstruction differs from
        # SIAARA's by a gain of about 3e-5 (measured), i.e. up to 2 at 16 bits.
        limit = (2 if bits > 12 else 1) if lossy else 0
        conv = ""
        if subsampled and nc == 3 and ref is not None and (d is None or d > limit):
            info = probe(stream)
            d = compare(sycc_to_rgb(ours, info), ref)
            limit = 3
            conv = " (after OpenJPEG's sYCC to RGB conversion)"
        exact_src = None
        if not lossy and "-ROI" not in extra:
            exact_src = all(np.array_equal(a, b) for a, b in zip(ours, planes))
        ok = d is not None and d <= limit
        if exact_src is False:
            ok = False
        report(ok, f"{desc}: peak error vs OpenJPEG {d}{conv}" + ("" if exact_src is None else f", lossless round trip {'exact' if exact_src else 'NOT exact'}"))


# ------------------------------------------------------------------ 4. DICOM files

J2K_TS = {"1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.4.201",
          "1.2.840.10008.1.2.4.202", "1.2.840.10008.1.2.4.203"}


def dicom_files():
    files = []
    try:
        from pydicom.data import get_testdata_files
        files += get_testdata_files("*")
    except Exception:
        pass
    files += glob.glob(os.path.join(PLJ_DATA, "ljdata", "ds", "*", "*.dcm"))
    seen, out = set(), []
    for f in files:
        b = os.path.basename(f)
        if b in seen or not f.lower().endswith(".dcm"):
            continue
        seen.add(b)
        out.append(f)
    return sorted(out)


def section_dicom():
    print("== 4. DICOM files with JPEG 2000 / HTJ2K transfer syntaxes")
    try:
        import warnings
        warnings.simplefilter("ignore")
        import pydicom
        from pydicom.encaps import generate_frames
    except Exception:
        skip("pydicom not installed")
        return
    for f in dicom_files():
        try:
            ds = pydicom.dcmread(f)
            ts = str(ds.file_meta.TransferSyntaxUID)
        except Exception:
            continue
        if ts not in J2K_TS:
            continue
        name = os.path.basename(f)
        nframes = int(getattr(ds, "NumberOfFrames", 1) or 1)
        rows, cols = int(ds.Rows), int(ds.Columns)
        spp = int(ds.SamplesPerPixel)
        ba, bs = int(ds.BitsAllocated), int(ds.BitsStored)
        sgn = int(ds.PixelRepresentation)
        frames = list(generate_frames(ds.PixelData, number_of_frames=nframes))
        worst, bad, rejected, oracle_fail = 0, 0, None, 0
        for i, fr in enumerate(frames):
            src = os.path.join(TMP, "frame.j2k")
            with open(src, "wb") as fh:
                fh.write(fr)
            dst = os.path.join(TMP, "frame.raw")
            p = run([TOOL, "frame", src, dst, str(rows), str(cols), str(spp), str(ba), str(bs), str(sgn)])
            info = probe(src)
            if p.returncode != 0:
                rejected = p.stderr.strip().splitlines()[-1] if p.stderr.strip() else "?"
                # A rejection is correct only when the codestream disagrees with the data set.
                disagree = info is None or (info["width"], info["height"], info["components"], info["precision"], info["signed"]) != (cols, rows, spp, bs, sgn)
                if not disagree:
                    bad += 1
                continue
            dt = np.dtype(("<i" if sgn else "<u") + str(ba // 8))
            ours = np.fromfile(dst, dtype=dt).astype(np.int64)
            ref = None
            if openjpeg is not None:
                try:
                    ref = np.asarray(openjpeg.decode(fr)).astype(np.int64).ravel()
                except Exception:
                    ref = None
            if ref is None or ref.size != ours.size:
                oracle_fail += 1
                continue
            d = int(np.abs(ours - ref).max()) if ours.size else 0
            worst = max(worst, d)
            limit = 0 if info and info["reversible"] else 1
            if d > limit:
                bad += 1
        desc = f"{name} ({ts.rsplit('.', 1)[-1]}, {rows}x{cols}x{spp}, {bs}/{ba}-bit{' signed' if sgn else ''}, {len(frames)} frame(s))"
        if rejected is not None and bad == 0:
            report(True, f"{desc}: rejected as required (codestream header disagrees with the data set, or the codestream is invalid): {rejected}")
        elif oracle_fail == len(frames):
            skip(f"{desc}: oracle could not decode it")
        else:
            report(bad == 0, f"{desc}: peak error vs pylibjpeg-openjpeg {worst}" + (f", {bad} frame(s) bad" if bad else ""))


# ------------------------------------------------------------------ 5. SIAARA-encoded streams

def section_ours():
    print("== 5. Codestreams written by SIAARA, decoded by the oracles")
    if not OPJ_DEC and openjpeg is None:
        skip("no oracle decoder")
        return
    rng = np.random.default_rng(99)
    cases = [
        # (w, h, nc, bits, signed, mode, quality, layers, levels, tile, progression)
        (64, 64, 1, 8, False, "lossless", 90, 1, 5, 0, 0),
        (1, 1, 1, 8, False, "lossless", 90, 1, 5, 0, 0),
        (1, 29, 1, 12, False, "lossless", 90, 1, 5, 0, 0),
        (7, 13, 3, 8, False, "lossless", 90, 2, 5, 0, 1),
        (513, 511, 1, 16, False, "lossless", 90, 1, 6, 0, 0),
        (100, 80, 1, 16, True, "lossless", 90, 3, 4, 0, 2),
        (120, 90, 3, 8, False, "lossless", 90, 2, 5, 48, 3),
        (120, 90, 3, 12, False, "lossless", 90, 1, 5, 64, 4),
        (128, 96, 3, 8, False, "lossy", 90, 1, 5, 0, 0),
        (128, 96, 3, 8, False, "lossy", 50, 3, 5, 0, 1),
        (97, 61, 1, 12, False, "lossy", 80, 2, 4, 32, 2),
        (300, 200, 1, 16, False, "lossy", 85, 1, 5, 0, 0),
        (64, 64, 1, 8, False, "ht", 90, 1, 5, 0, 0),
        (1, 1, 1, 8, False, "ht", 90, 1, 5, 0, 0),
        (7, 13, 3, 8, False, "ht", 90, 1, 5, 0, 0),
        (513, 511, 1, 16, False, "ht", 90, 1, 5, 0, 0),
        (100, 80, 1, 16, True, "ht", 90, 1, 3, 0, 2),
        (200, 150, 3, 12, False, "ht", 90, 1, 5, 64, 4),
    ]
    for (w, h, nc, bits, sgn, mode, q, layers, levels, tile, prog) in cases:
        planes = synth_image(rng, w, h, nc, bits, sgn)
        ba = 8 if bits <= 8 else 16
        dt = np.dtype(("<i" if sgn else "<u") + str(ba // 8))
        raw = os.path.join(TMP, "enc_in.raw")
        np.stack(planes, axis=-1).astype(dt).tofile(raw)
        stream = os.path.join(TMP, "enc_out.j2k")
        p = run([TOOL, "encode", raw, stream, str(h), str(w), str(nc), str(ba), str(bits), "1" if sgn else "0",
                 mode, str(q), str(layers), str(levels), str(tile), str(prog)])
        desc = f"{w}x{h}x{nc} {bits}-bit{' signed' if sgn else ''} {mode} q={q} layers={layers} levels={levels} tile={tile} prog={prog}"
        if p.returncode != 0:
            report(False, f"{desc}: SIAARA encode failed: {p.stderr.strip()}")
            continue
        ours, comps = ours_components(stream)
        if ours is None:
            report(False, f"{desc}: SIAARA decode failed: {comps}")
            continue
        size = os.path.getsize(stream)
        results_txt = []
        ok = True
        for oracle_name, ref in (("OpenJPEG", opj_components(stream)), ("pylibjpeg-openjpeg", pylibjpeg_components(stream))):
            if ref is None:
                results_txt.append(f"{oracle_name}: no decode")
                ok = False
                continue
            if mode != "lossy":
                d = compare(planes, ref)
                ok = ok and d == 0
                results_txt.append(f"{oracle_name} vs source {d}")
            else:
                d = compare(ours, ref, comps)
                limit = 2 if bits > 12 else 1
                ok = ok and d is not None and d <= limit
                results_txt.append(f"{oracle_name} vs SIAARA {d}")
        if mode == "lossy":
            mse = np.mean([((a - b) ** 2).mean() for a, b in zip(ours, planes)])
            top = (1 << bits) - 1
            results_txt.append(f"PSNR {10 * np.log10(top * top / mse):.1f} dB" if mse > 0 else "PSNR inf")
        else:
            ok = ok and all(np.array_equal(a, b) for a, b in zip(ours, planes))
        report(ok, f"{desc}: {size} bytes; " + ", ".join(results_txt))


# ------------------------------------------------------------------ main

if __name__ == "__main__":
    section_conformance()
    section_files()
    section_nonregression()
    section_opj_streams()
    section_dicom()
    section_ours()
    print(f"\n{results['ok']} ok, {results['bad']} failed, {results['skip']} skipped")
    shutil.rmtree(TMP, ignore_errors=True)
    sys.exit(1 if results["bad"] else 0)
