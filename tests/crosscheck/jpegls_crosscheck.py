#!/usr/bin/env python3
"""Cross-checks the SIAARA JPEG-LS codec against independent decoders (test-only, never shipped).

Oracles (used only to compare results): CharLS via pyjpegls (module jpeg_ls) and Thomas Richter's
libjpeg via pylibjpeg-libjpeg (module libjpeg). Whichever of them is installed is used.

  1. pydicom's JPEG-LS test files (transfer syntaxes 1.2.840.10008.1.2.4.80 / .81): every frame is
     decoded by SIAARA and compared pixel for pixel with each oracle and with pydicom.
  2. Synthetic images encoded by the CharLS oracle are decoded by SIAARA (bit-exact comparison).
  3. Synthetic images encoded by SIAARA (lossless and near-lossless, ILV 0/1/2, restart intervals,
     custom LSE presets) are decoded by every oracle and compared bit-exactly with SIAARA's own
     decode, and with the source within NEAR.

  4. Optionally, a pylibjpeg-data checkout (https://github.com/pydicom/pylibjpeg-data): its DICOM
     JPEG-LS files and the ISO/IEC 14495-1 conformance codestreams (jpg/14495/*/*.JLS).

Known oracle disagreements on LSE presets (each checked case must still match at least one
oracle bit-exactly, and the disagreeing oracle is reported):
  - libjpeg does not apply the T.87 rule that a preset field of 0 selects the default;
  - CharLS rejects some streams with MAXVAL < 2^P - 1 or RESET != 64.
CharLS also does not support 2-component scans in ILV 1/2; those cases are checked the same way.

Usage:  python3 -I jpegls_crosscheck.py <path to jpegls_tool> [<pylibjpeg-data>/ljdata]
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

TOOL = sys.argv[1]
EXTRA = sys.argv[2] if len(sys.argv) > 2 else None
TMP = tempfile.mkdtemp(prefix="siaara_jls_")

try:
    import jpeg_ls  # CharLS
except Exception:  # pragma: no cover
    jpeg_ls = None
try:
    import libjpeg  # Thomas Richter's libjpeg
except Exception:  # pragma: no cover
    libjpeg = None

results = {"ok": 0, "bad": 0}


def report(ok, what):
    results["ok" if ok else "bad"] += 1
    print(("ok    " if ok else "FAIL  ") + what)


def native_dtype(bits):
    return np.dtype("<u2") if bits > 8 else np.dtype("u1")


def ours_decode(stream, signed=False):
    src = os.path.join(TMP, "dec.jls")
    dst = os.path.join(TMP, "dec.raw")
    with open(src, "wb") as f:
        f.write(stream)
    args = [TOOL, "decode", src, dst] + (["--signed"] if signed else [])
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip()
    w, h, c, p, _near, _ilv, _ct = map(int, r.stdout.split())
    arr = np.frombuffer(open(dst, "rb").read(), native_dtype(p))
    return arr.reshape((h, w, c) if c > 1 else (h, w)), None


def ours_encode(img, bits, near, ilv, restart=0, preset=None):
    src = os.path.join(TMP, "enc.raw")
    dst = os.path.join(TMP, "enc.jls")
    img.astype(native_dtype(bits)).tofile(src)
    h, w = img.shape[:2]
    c = 1 if img.ndim == 2 else img.shape[2]
    args = [TOOL, "encode", src, dst, str(h), str(w), str(c), str(bits), str(near), str(ilv), str(restart)]
    if preset:
        args += [str(v) for v in preset]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    return open(dst, "rb").read()


def oracle_decoders():
    out = []
    if jpeg_ls is not None:
        out.append(("CharLS", charls_decode))
    if libjpeg is not None:
        out.append(("libjpeg", libjpeg_decode))
    return out


def charls_decode(stream, shape, bits):
    data, meta = jpeg_ls.decode_buffer(bytes(stream))
    arr = np.frombuffer(bytes(data), native_dtype(bits))
    if len(shape) == 3 and meta["interleave_mode"] == 0:  # CharLS returns ILV 0 planar
        return arr.reshape(shape[2], shape[0], shape[1]).transpose(1, 2, 0)
    return arr.reshape(shape)


def libjpeg_decode(stream, shape, bits):
    return np.asarray(libjpeg.decode(bytes(stream))).reshape(shape).astype(native_dtype(bits))


def charls_encode(img, bits, near, ilv):
    h, w = img.shape[:2]
    c = 1 if img.ndim == 2 else img.shape[2]
    src = img.astype(native_dtype(bits))
    if c > 1 and ilv == 0:
        src = np.ascontiguousarray(src.transpose(2, 0, 1))
    return bytes(jpeg_ls.encode_buffer(src.tobytes(), h, w, c, bits, near, ilv if c > 1 else None))


# ------------------------------------------------------------------------------ synthetic images
def synthetic(kind, h, w, c, bits, seed):
    rng = np.random.default_rng(seed)
    maxval = (1 << bits) - 1
    y, x = np.mgrid[0:h, 0:w]
    planes = []
    for k in range(c):
        if kind == "gradient":
            p = (x * (k + 1) * maxval // max(1, w - 1) + y * maxval // max(1, 2 * h)) % (maxval + 1)
        elif kind == "noise":
            p = rng.integers(0, maxval + 1, (h, w))
        elif kind == "flat":  # long runs with sparse spikes: exercises run mode and interruptions
            p = np.full((h, w), maxval // 3 + k)
            spikes = rng.random((h, w)) < 0.02
            p[spikes] = rng.integers(0, maxval + 1, spikes.sum())
        elif kind == "edges":
            p = np.where(((x // 7) + (y // 5) + k) % 2 == 0, maxval // 5, maxval - maxval // 7)
            p = p + (rng.integers(-2, 3, (h, w)) if maxval > 8 else 0)
        elif kind == "ct":  # smooth body with noise, like a CT slice
            r = np.hypot(x - w / 2, y - h / 2)
            p = np.where(r < min(h, w) * 0.4, maxval * 0.45 + 30 * np.sin(x / 9.0), maxval * 0.02)
            p = p + rng.normal(0, max(1.0, maxval / 2000.0), (h, w))
        p = np.clip(np.asarray(p, dtype=np.int64), 0, maxval)
        planes.append(p)
    img = np.stack(planes, -1) if c > 1 else planes[0]
    return img.astype(native_dtype(bits))


def main():
    print(f"oracles: CharLS={'yes' if jpeg_ls else 'no'} libjpeg={'yes' if libjpeg else 'no'}")
    if not oracle_decoders():
        print("no oracle installed")
        return 2

    # 1. pydicom test files ----------------------------------------------------------------------
    import warnings

    warnings.simplefilter("ignore")
    import pydicom
    from pydicom.data import get_testdata_files
    from pydicom.encaps import generate_frames

    paths = sorted(set(get_testdata_files("*")))
    if EXTRA:
        import glob

        paths += sorted(glob.glob(os.path.join(EXTRA, "ds", "**", "*.dcm"), recursive=True))
    files = []
    seen = set()
    for path in paths:
        try:
            ds = pydicom.dcmread(path, stop_before_pixels=False)
            ts = str(ds.file_meta.TransferSyntaxUID)
        except Exception:
            continue
        if ts in ("1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.81") and "PixelData" in ds:
            key = (os.path.basename(path), len(ds.PixelData))
            if key not in seen:
                seen.add(key)
                files.append((path, ds, ts))
    print(f"\n{len(files)} DICOM JPEG-LS files")
    for path, ds, ts in files:
        name = os.path.basename(path)
        nframes = int(ds.get("NumberOfFrames", 1) or 1)
        bits = int(ds.BitsStored)
        signed = int(ds.PixelRepresentation) == 1
        spp = int(ds.SamplesPerPixel)
        shape = (int(ds.Rows), int(ds.Columns)) + ((spp,) if spp > 1 else ())
        try:
            ref_all = ds.pixel_array
        except Exception as e:
            ref_all = None
            print(f"      pydicom could not decode {name}: {e}")
        mask = (1 << bits) - 1
        for i, frame in enumerate(generate_frames(ds.PixelData, number_of_frames=nframes)):
            ours, err = ours_decode(frame, signed)
            what = f"{name} frame {i} ({ts[-3:]}, {shape}, {bits}-bit{' signed' if signed else ''})"
            if ours is None:
                report(False, f"{what}: SIAARA error {err}")
                continue
            for oname, dec in oracle_decoders():
                try:
                    ref = dec(frame, shape, 16 if bits > 8 else 8)
                except Exception as e:
                    print(f"      {oname} could not decode {what}: {e}")
                    continue
                report(np.array_equal(ours.astype(np.int64) & mask, ref.astype(np.int64) & mask),
                       f"{what} vs {oname}")
            if ref_all is not None:
                ref = ref_all[i] if nframes > 1 else ref_all
                report(np.array_equal(ours.astype(np.int64) & mask, ref.astype(np.int64) & mask),
                       f"{what} vs pydicom pixel_array")

    if EXTRA:
        import glob

        conf = sorted(glob.glob(os.path.join(EXTRA, "jpg", "14495", "*", "*.JLS")))
        print(f"\n{len(conf)} ISO/IEC 14495-1 conformance codestreams")
        for path in conf:
            name = "/".join(path.split(os.sep)[-2:])
            stream = open(path, "rb").read()
            ours, err = ours_decode(stream)
            if ours is None:
                if "subsampled" in err:
                    print(f"skip  {name}: subsampled components are not supported (not used by DICOM)")
                else:
                    report(False, f"{name}: SIAARA error {err}")
                continue
            bits = 16 if ours.dtype.itemsize == 2 else 8
            for oname, dec in oracle_decoders():
                try:
                    ref = dec(stream, ours.shape, bits)
                except Exception as e:
                    print(f"      {oname} could not decode {name}: {e}")
                    continue
                report(np.array_equal(ours, ref), f"{name} {ours.shape} vs {oname}")

    # 2. oracle-encoded synthetic streams decoded by SIAARA ---------------------------------------
    print("\nCharLS-encoded streams decoded by SIAARA")
    if jpeg_ls is None:
        print("      skipped (pyjpegls missing)")
    else:
        seed = 100
        for bits in (2, 5, 8, 10, 12, 16):
            for c, ilvs in ((1, (0,)), (3, (0, 1, 2))):
                for ilv in ilvs:
                    for near in (0, 1, 3) if bits > 2 else (0, 1):
                        for kind in ("gradient", "noise", "flat", "edges", "ct"):
                            seed += 1
                            img = synthetic(kind, 37, 53, c, bits, seed)
                            try:
                                stream = charls_encode(img, bits, near, ilv)
                            except Exception as e:
                                print(f"      CharLS could not encode {kind} {bits}-bit: {e}")
                                continue
                            ref = charls_decode(stream, img.shape, bits)
                            ours, err = ours_decode(stream)
                            what = f"CharLS enc {kind:8s} {bits:2d}-bit c={c} ilv={ilv} near={near}"
                            report(ours is not None and np.array_equal(ours, ref), what + ("" if ours is not None else f": {err}"))

    # 3. SIAARA-encoded streams decoded by the oracles --------------------------------------------
    print("\nSIAARA-encoded streams decoded by the oracles")
    seed = 500
    cases = []
    for bits in (2, 6, 8, 12, 16):
        for c, ilvs in ((1, (0,)), (3, (0, 1, 2))):
            for ilv in ilvs:
                for near in (0, 2, 7) if bits > 4 else (0, 1):
                    for kind in ("gradient", "noise", "flat", "edges", "ct"):
                        cases.append((kind, bits, c, ilv, near, 0, None))
    # 2 and 4 components
    for c in (2, 4):
        for ilv in (0, 1, 2):
            for near in (0, 3):
                for kind in ("flat", "ct"):
                    cases.append((kind, 8, c, ilv, near, 0, None))
                    cases.append((kind, 12, c, ilv, near, 3, None))
    # restart intervals and custom presets
    for c, ilv in ((1, 0), (3, 0), (3, 1), (3, 2)):
        for rst in (1, 5):
            cases.append(("flat", 8, c, ilv, 0, rst, None))
            cases.append(("ct", 12, c, ilv, 2, rst, None))
    cases.append(("ct", 12, 1, 0, 0, 0, (4095, 10, 40, 200, 32)))
    cases.append(("ct", 16, 1, 0, 3, 0, (0, 30, 90, 400, 0)))
    cases.append(("flat", 8, 3, 2, 1, 7, (255, 5, 9, 30, 100)))
    cases.append(("gradient", 8, 1, 0, 0, 0, (200, 3, 7, 21, 64)))
    cases.append(("flat", 8, 1, 0, 0, 0, (0, 0, 0, 0, 100)))
    cases.append(("flat", 8, 3, 2, 1, 0, (255, 5, 9, 30, 64)))
    for kind, bits, c, ilv, near, rst, preset in cases:
        seed += 1
        img = synthetic(kind, 41, 47, c, bits, seed)
        if preset and preset[0]:
            img = np.minimum(img, preset[0]).astype(img.dtype)
        stream = ours_encode(img, bits, near, ilv, rst, preset)
        ours, err = ours_decode(stream)
        what = f"SIAARA enc {kind:8s} {bits:2d}-bit c={c} ilv={ilv} near={near} rst={rst} preset={preset}"
        if ours is None:
            report(False, f"{what}: SIAARA decode error {err}")
            continue
        err_max = int(np.abs(ours.astype(np.int64) - img.astype(np.int64)).max())
        report(err_max <= near, f"{what}: max error {err_max} <= NEAR")
        agree, differ = [], []
        for oname, dec in oracle_decoders():
            try:
                ref = dec(stream, img.shape, bits)
                (agree if np.array_equal(ours, ref) else differ).append(oname)
            except Exception as e:
                differ.append(f"{oname} ({str(e)[:60]})")
        if preset is None and not (c == 2 and ilv > 0):
            for oname in differ:
                report(False, f"{what} decoded by {oname}")
            for oname in agree:
                report(True, f"{what} decoded by {oname}")
        else:
            note = f"; disagreeing oracle: {', '.join(differ)}" if differ else ""
            report(bool(agree), f"{what} matches {', '.join(agree) or 'no oracle'}{note}")

    print(f"\n{results['ok']} checks passed, {results['bad']} failed")
    return 1 if results["bad"] else 0


if __name__ == "__main__":
    sys.exit(main())
