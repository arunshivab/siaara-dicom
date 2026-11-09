#!/usr/bin/env python3
"""Cross-checks the SIAARA JPEG codec against independent decoders (test-only, never shipped).

Oracles (only used to produce reference pixels):
  - pylibjpeg-libjpeg (Thomas Richter's libjpeg), colour_transform=0: every process incl. 12-bit and
    lossless. Its chroma upsampling interpolates.
  - jpeglib (pip package wrapping IJG libjpeg 6b and libjpeg-turbo 2.1), no colour conversion,
    8-bit DCT streams: with do_fancy_upsampling on (triangular upsampling, SIAARA's default) and
    off (sample replication, SIAARA's JpegDecodeOptions::replicate_chroma).
  - imagecodecs.jpeg8_decode (libjpeg-turbo 3.x): second opinion for grey-scale lossless and
    12-bit streams, and the verdict for lossless point transform (Pt > 0), where pylibjpeg-libjpeg
    seeds the first prediction with 2^(P-1) instead of the 2^(P-Pt-1) of T.81 H.1.2.1.
T.81 does not define how subsampled components are upsampled, so for subsampled lossy streams the
verdict compares SIAARA's default (fancy) decode with libjpeg's fancy upsampling, and SIAARA's
replicating decode with libjpeg's replication; the difference to pylibjpeg is still printed.
Pillow (libjpeg-turbo) is used to write test streams.

Parts:
  A. JPEG test files shipped with pydicom (transfer syntaxes .50 .51 .57 .70): every frame is
     decoded with SIAARA and with pylibjpeg-libjpeg (no colour transform) and compared.
  B. Streams written by Pillow/libjpeg-turbo that pydicom lacks: progressive (spectral selection +
     successive approximation), restart intervals, subsampling 4:2:0 / 4:2:2, odd sizes.
  C. Streams written by the SIAARA encoder, decoded by the oracle (encoder interoperability).

Lossless must match exactly; lossy may differ by at most 1 per sample (the max error is reported).
Usage:
    python3 -I jpeg_crosscheck.py <path to jpeg_tool>
"""
import io
import os
import subprocess
import sys
import tempfile
import warnings

import numpy as np

warnings.filterwarnings("ignore")

import imagecodecs  # libjpeg-turbo 3.x
import jpeglib  # IJG libjpeg / libjpeg-turbo builds, used without fancy upsampling
import libjpeg  # pylibjpeg-libjpeg
import pydicom
from PIL import Image
from pydicom.data import get_testdata_files
from pydicom.encaps import generate_frames

TOOL = sys.argv[1]
TS = {
    "1.2.840.10008.1.2.4.50": "baseline",
    "1.2.840.10008.1.2.4.51": "extended",
    "1.2.840.10008.1.2.4.57": "lossless",
    "1.2.840.10008.1.2.4.70": "lossless-sv1",
}
TMP = tempfile.mkdtemp(prefix="siaara_jpeg_")
results = []  # (name, ok, detail)


def siaara_decode(stream, rows, cols, spp, alloc, stored, signed=0, replicate=False):
    src = os.path.join(TMP, "in.jpg")
    dst = os.path.join(TMP, "out.raw")
    with open(src, "wb") as f:
        f.write(stream)
    r = subprocess.run([TOOL, "decode", src, dst, str(rows), str(cols), str(spp), str(alloc),
                        str(stored), str(signed), "0"] + (["replicate"] if replicate else []),
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip()
    dt = np.uint8 if alloc == 8 else np.dtype("<u2")
    a = np.fromfile(dst, dtype=dt)
    return a.reshape((rows, cols, spp)) if spp > 1 else a.reshape((rows, cols)), ""


def siaara_encode(arr, alloc, stored, lossless, predictor=1, quality=90, ybr=0, lh=1, lv=1, ri=0, pt=0,
                  signed=0):
    rows, cols = arr.shape[:2]
    spp = arr.shape[2] if arr.ndim == 3 else 1
    src = os.path.join(TMP, "enc.raw")
    dst = os.path.join(TMP, "enc.jpg")
    arr.astype(np.uint8 if alloc == 8 else "<u2").tofile(src)
    r = subprocess.run([TOOL, "encode", src, dst, str(rows), str(cols), str(spp), str(alloc), str(stored),
                        str(signed), "0", str(int(lossless)), str(predictor), str(quality), str(ybr), str(lh),
                        str(lv), str(ri), str(pt)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    with open(dst, "rb") as f:
        return f.read()


def oracle(stream):
    return np.asarray(libjpeg.decode(bytes(stream), colour_transform=0))


def turbo3(stream):
    return np.asarray(imagecodecs.jpeg8_decode(bytes(stream)))


def libjpeg_oracle(stream, version, fancy):
    """Decode with jpeglib (given libjpeg build), no colour conversion; fancy or replicated chroma."""
    path = os.path.join(TMP, "oracle.jpg")
    with open(path, "wb") as f:
        f.write(stream)
    with jpeglib.version(version):
        cs = jpeglib.read_dct(path).jpeg_color_space
        flags = ["+DO_FANCY_UPSAMPLING" if fancy else "-DO_FANCY_UPSAMPLING", "-DO_BLOCK_SMOOTHING"]
        im = jpeglib.read_spatial(path, out_color_space=cs, flags=flags)
        a = np.asarray(im.spatial)
    return a[..., 0] if a.ndim == 3 and a.shape[2] == 1 else a


def subsampled(stream):
    """True when the SOF of an 8-bit DCT stream has components with different sampling factors."""
    for m in (b"\xff\xc0", b"\xff\xc1", b"\xff\xc2"):
        i = stream.find(m)
        if i >= 0:
            nf = stream[i + 9]
            hv = {stream[i + 11 + 3 * k] for k in range(nf)}
            return len(hv) > 1
    return False


def compare_lossy8(name, stream, ours, note="", ours_replicated=None):
    """8-bit DCT stream: verdict from libjpeg (fancy and replicating) when subsampled, else pylibjpeg."""
    ref_interp = oracle(stream)
    if not subsampled(stream):
        compare(name, ours, ref_interp, False, note)
        return
    details = []
    ok = ours is not None
    for ver in ("6b", "turbo210"):
        for fancy, mine in ((True, ours), (False, ours_replicated)):
            if mine is None:
                continue
            try:
                ref = libjpeg_oracle(stream, ver, fancy)
            except Exception as e:
                details.append(f"{ver}: oracle failed {e}")
                continue
            err = int(np.max(np.abs(mine.astype(np.int64) - ref.reshape(mine.shape).astype(np.int64))))
            ok &= err <= 1
            details.append(f"libjpeg-{ver} ({'fancy' if fancy else 'replicating'}) max error {err}")
    if ours is not None:
        r = ref_interp.reshape(ours.shape).astype(np.int64)
        d = np.abs(ours.astype(np.int64) - r)
        details.append(f"pylibjpeg (interpolating) max error {int(d.max())}, luma {int(d[..., 0].max())}")
    else:
        details.append(f"SIAARA failed: {note}")
    results.append((name, ok, "; ".join(details)))


def compare(name, ours, ref, lossless_mode, note=""):
    if ours is None:
        results.append((name, False, f"SIAARA failed: {note}"))
        return
    ref = ref.reshape(ours.shape)
    err = int(np.max(np.abs(ours.astype(np.int64) - ref.astype(np.int64)))) if ours.size else 0
    if lossless_mode:
        ok = err == 0
        detail = "exact" if ok else f"MISMATCH max error {err}"
    else:
        ok = err <= 1
        n_off = int(np.count_nonzero(ours.astype(np.int64) != ref.astype(np.int64)))
        detail = f"max error {err} ({n_off} of {ours.size} samples differ)"
    results.append((name, ok, detail + (f"  [{note}]" if note else "")))


# ------------------------------------------------------------------ A. pydicom test files
def fix_sequential_sos(stream):
    """Copy of a sequential DCT stream with every SOS set to Ss=0, Se=63, Ah=Al=0."""
    b = bytearray(stream)
    pos = 2
    while pos + 4 <= len(b) and b[pos] == 0xFF:
        m, length = b[pos + 1], (b[pos + 2] << 8) | b[pos + 3]
        if m == 0xDA:
            ns = b[pos + 4]
            q = pos + 5 + 2 * ns
            b[q], b[q + 1], b[q + 2] = 0, 63, 0
            break
        pos += 2 + length
    return bytes(b)


def part_a():
    for path in sorted(get_testdata_files("*"), key=os.path.basename):
        try:
            ds = pydicom.dcmread(path, force=True)
            ts = str(ds.file_meta.TransferSyntaxUID)
        except Exception:
            continue
        if ts not in TS or "PixelData" not in ds:
            continue
        name = os.path.basename(path)
        rows, cols = int(ds.Rows), int(ds.Columns)
        spp = int(ds.get("SamplesPerPixel", 1))
        alloc, stored = int(ds.BitsAllocated), int(ds.BitsStored)
        signed = int(ds.get("PixelRepresentation", 0))
        nframes = int(ds.get("NumberOfFrames", 1) or 1)
        lossless_mode = TS[ts].startswith("lossless")
        for i, frame in enumerate(generate_frames(ds.PixelData, number_of_frames=nframes)):
            label = f"A {name}" + (f" frame {i}" if nframes > 1 else "") + f" ({TS[ts]}, {ds.get('PhotometricInterpretation', '')})"
            oracle_stream = frame
            try:
                ref = oracle(frame)
            except Exception as e:
                # Some files carry Ss/Se/Ah/Al values that are invalid for sequential scans (SIAARA
                # ignores them, the oracle refuses). Retry the oracle on a corrected copy.
                oracle_stream = fix_sequential_sos(frame) if not lossless_mode else None
                try:
                    ref = oracle(oracle_stream)
                    label += " [oracle given copy with SOS Ss/Se fixed]"
                except Exception:  # oracle cannot decode: record, not counted as our failure
                    results.append((label, None, f"oracle failed: {e}"))
                    continue
            if signed and lossless_mode and ref.dtype != np.int16:
                # oracle returns the stored bit pattern; sign-extend from BitsStored like SIAARA does
                s = min(stored, 16)
                v = ref.astype(np.int64) & ((1 << s) - 1)
                v = np.where(v >= (1 << (s - 1)), v - (1 << s), v)
                ref = (v & 0xFFFF).astype(np.uint16)
            ours, err = siaara_decode(frame, rows, cols, spp, alloc, stored, signed)
            if not lossless_mode and alloc == 8:
                rep_, _ = siaara_decode(frame, rows, cols, spp, alloc, stored, signed, replicate=True)
                compare_lossy8(label, oracle_stream, ours, err, rep_)
            else:
                compare(label, ours, ref, lossless_mode, err)
            if spp == 1 and ours is not None and not signed:
                compare(label + " vs libjpeg-turbo 3", ours, turbo3(oracle_stream), lossless_mode)


# ------------------------------------------------------------------ B. Pillow-written streams
def test_image(h, w, mode, seed):
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w]
    base = 128 + 90 * np.sin(x / 9.0) * np.cos(y / 13.0)
    if mode == "L":
        a = base + rng.normal(0, 6, (h, w))
    else:
        a = np.stack([base, 255 - base * 0.8, 64 + (x * 3 + y) % 128], axis=-1) + rng.normal(0, 6, (h, w, 3))
    return np.clip(a, 0, 255).astype(np.uint8)


def part_b():
    cases = [
        ("gray progressive", "L", 61, 77, dict(progressive=True, quality=80)),
        ("gray progressive optimised", "L", 64, 64, dict(progressive=True, optimize=True, quality=60)),
        ("gray restart every MCU row", "L", 50, 70, dict(quality=85, restart_marker_rows=1)),
        ("gray restart every 3 blocks", "L", 50, 70, dict(quality=85, restart_marker_blocks=3)),
        ("gray progressive + restarts", "L", 50, 70, dict(progressive=True, quality=85, restart_marker_blocks=2)),
        ("colour 4:4:4 progressive", "RGB", 45, 59, dict(progressive=True, quality=90, subsampling=0)),
        ("colour 4:4:4 baseline", "RGB", 45, 59, dict(quality=90, subsampling=0)),
        ("colour 4:2:2 baseline", "RGB", 45, 59, dict(quality=90, subsampling=1)),
        ("colour 4:2:0 baseline", "RGB", 45, 59, dict(quality=90, subsampling=2)),
        ("colour 4:2:0 progressive", "RGB", 45, 59, dict(progressive=True, quality=75, subsampling=2)),
        ("colour 4:2:0 progressive + restarts", "RGB", 33, 47, dict(progressive=True, quality=75, subsampling=2,
                                                                   restart_marker_blocks=1)),
    ]
    for name, mode, h, w, kw in cases:
        img = test_image(h, w, mode, len(name))
        bio = io.BytesIO()
        Image.fromarray(img, mode).save(bio, "JPEG", **kw)
        stream = bio.getvalue()
        spp = 1 if mode == "L" else 3
        ours, err = siaara_decode(stream, h, w, spp, 8, 8)
        rep_, _ = siaara_decode(stream, h, w, spp, 8, 8, replicate=True)
        compare_lossy8(f"B {name}", stream, ours, err, rep_)


# ------------------------------------------------------------------ C. SIAARA encoder -> oracle
def part_c():
    rng = np.random.default_rng(1)
    for bits, alloc in ((8, 8), (12, 16), (16, 16), (5, 8)):
        y, x = np.mgrid[0:37, 0:53]
        img = ((1 << bits) - 1) * (0.5 + 0.4 * np.sin(x / 7.0) * np.cos(y / 5.0)) + rng.normal(0, 2 ** (bits - 6), x.shape)
        img = np.clip(img, 0, (1 << bits) - 1).astype(np.uint16)
        for psv in range(1, 8):
            for ri in (0, 53):
                stream = siaara_encode(img, alloc, bits, True, predictor=psv, ri=ri)
                try:
                    ref = oracle(stream)
                except Exception as e:
                    results.append((f"C lossless {bits}-bit psv {psv} ri {ri}", False, f"oracle failed: {e}"))
                    continue
                ok = np.array_equal(ref.reshape(img.shape).astype(np.int64), img.astype(np.int64))
                ok2 = np.array_equal(turbo3(stream).reshape(img.shape).astype(np.int64), img.astype(np.int64))
                results.append((f"C lossless {bits}-bit psv {psv} ri {ri} -> both oracles", ok and ok2,
                                "exact" if ok and ok2 else f"MISMATCH (pylibjpeg {ok}, turbo3 {ok2})"))
    rgb = test_image(31, 45, "RGB", 3).astype(np.uint16)
    for psv in (1, 4, 7):
        for ri in (0, 45, 90):
            stream = siaara_encode(rgb, 8, 8, True, predictor=psv, ri=ri)
            ref = oracle(stream)
            ok = np.array_equal(ref.reshape(rgb.shape), rgb)
            results.append((f"C lossless RGB interleaved psv {psv} ri {ri} -> oracle", ok, "exact" if ok else "MISMATCH"))
    # Point transform and signed data.
    img12 = (np.arange(37 * 53).reshape(37, 53) * 37 % 4096).astype(np.uint16)
    for pt in (1, 4, 11):
        stream = siaara_encode(img12, 16, 12, True, predictor=6, pt=pt)
        want = (img12.astype(np.int64) >> pt) << pt
        ref = turbo3(stream).reshape(img12.shape).astype(np.int64)
        ours, err = siaara_decode(stream, 37, 53, 1, 16, 12)
        ok = np.array_equal(ref, want) and ours is not None and np.array_equal(ours.astype(np.int64), want)
        alt = oracle(stream).reshape(img12.shape).astype(np.int64)
        info = "pylibjpeg agrees" if np.array_equal(alt, want) else "pylibjpeg differs (2^(P-1) seed)"
        results.append((f"C lossless 12-bit Pt {pt} -> libjpeg-turbo 3 and SIAARA", ok,
                        ("exact" if ok else "MISMATCH") + f"; {info}"))
    sgn = (np.arange(29 * 31).reshape(29, 31) * 101 % 4096 - 2048).astype(np.int16)
    stream = siaara_encode(sgn.view(np.uint16), 16, 12, True, predictor=4, signed=1)
    ref = oracle(stream).reshape(sgn.shape).astype(np.int64) & 0xFFF
    ok = np.array_equal(ref, sgn.astype(np.int64) & 0xFFF)
    results.append(("C lossless 12-bit signed -> oracle (bit pattern)", ok, "exact" if ok else "MISMATCH"))
    ours, err = siaara_decode(stream, 29, 31, 1, 16, 12, signed=1)
    ok = ours is not None and np.array_equal(ours.view(np.int16), sgn)
    results.append(("C lossless 12-bit signed SIAARA round trip sign-extended", ok, "exact" if ok else f"MISMATCH {err}"))
    # Lossy: oracle decode of our stream vs our own decode.
    for bits, alloc in ((8, 8), (12, 16)):
        y, x = np.mgrid[0:40, 0:56]
        img = (((1 << bits) - 1) * (0.5 + 0.4 * np.sin(x / 7.0) * np.cos(y / 5.0))).astype(np.uint16)
        for q in (50, 90):
            for ri in (0, 3):
                stream = siaara_encode(img, alloc, bits, False, quality=q, ri=ri)
                ours, err = siaara_decode(stream, 40, 56, 1, alloc, bits)
                compare(f"C lossy {bits}-bit q{q} ri {ri} -> oracle", ours, oracle(stream), False, err)
                compare(f"C lossy {bits}-bit q{q} ri {ri} -> libjpeg-turbo 3", ours, turbo3(stream), False, err)
    col = test_image(41, 50, "RGB", 9).astype(np.uint16)
    for lh, lv in ((1, 1), (2, 2), (2, 1)):
        stream = siaara_encode(col, 8, 8, False, quality=90, ybr=1, lh=lh, lv=lv)
        ours, err = siaara_decode(stream, 41, 50, 3, 8, 8)
        rep_, _ = siaara_decode(stream, 41, 50, 3, 8, 8, replicate=True)
        compare_lossy8(f"C lossy YCbCr {lh}x{lv} -> oracle", stream, ours, err, rep_)


part_a()
part_b()
part_c()
bad = 0
for name, ok, detail in results:
    tag = "SKIP" if ok is None else ("ok  " if ok else "FAIL")
    bad += ok is False
    print(f"{tag} {name}: {detail}")
print(f"\n{sum(1 for r in results if r[1])} ok, {bad} failed, {sum(1 for r in results if r[1] is None)} skipped")
sys.exit(1 if bad else 0)
