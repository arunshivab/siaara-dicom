#!/usr/bin/env python3
"""Cross-checks the SIAARA pixel pipeline against pydicom (test-only, never shipped).

pydicom (with numpy, pylibjpeg, pylibjpeg-libjpeg, pylibjpeg-openjpeg) is used only as an oracle:
for every pydicom test file with pixel data, it compares
  - our decoded frames (stored values) with pydicom.pixels.pixel_array(..., raw=True)
  - colour frames converted to RGB with pixel_array(..., raw=False)        (informational)
  - PALETTE COLOR output with pydicom.pixels.apply_color_lut
  - modality output with pydicom.pixels.apply_modality_lut
  - VOI output with pydicom.pixels.apply_windowing / apply_voi (same output range as pydicom)

Usage:
    python3 pixel_crosscheck.py <path to pixel_tool> [file ...]
"""
import os
import subprocess
import sys
import tempfile
import warnings

import numpy as np
import pydicom
from pydicom.data import get_testdata_files
from pydicom.pixels import apply_color_lut, apply_modality_lut, apply_voi, apply_windowing, pixel_array

warnings.simplefilter("ignore")
TOOL = sys.argv[1]
TMP = tempfile.mkdtemp(prefix="pixel_cc_")


def run(*args):
    return subprocess.run([TOOL, *args], capture_output=True, text=True, errors="replace")


def info(path):
    r = run("info", path)
    if r.returncode != 0:
        return None, r
    d = {}
    for line in r.stdout.splitlines():
        k, _, v = line.partition("=")
        d.setdefault(k, v)
    return d, r


def ours(cmd, path, *extra):
    out = os.path.join(TMP, cmd + ".bin")
    r = run(cmd, path, out, *extra)
    if r.returncode != 0:
        return None, r.stderr.strip()
    with open(out, "rb") as f:
        return f.read(), ""


def stored(values, bits_stored, signed):
    """Stored values: low bits_stored bits, sign-extended when signed (as int64)."""
    v = values.astype(np.int64) & ((1 << bits_stored) - 1)
    if signed:
        v = np.where(v >= (1 << (bits_stored - 1)), v - (1 << bits_stored), v)
    return v


def diff(a, b):
    a = np.asarray(a, dtype=np.float64).ravel()
    b = np.asarray(b, dtype=np.float64).ravel()
    if a.shape != b.shape:
        return f"shape {a.shape} vs {b.shape}"
    d = np.abs(a - b)
    n = int(np.count_nonzero(d > 1e-9 * np.maximum(1.0, np.abs(b))))
    return "match" if n == 0 else f"{n}/{a.size} differ, max |diff| {d.max():.6g}"


def our_samples(path, inf):
    rows, cols, spp = int(inf["rows"]), int(inf["columns"]), int(inf["spp"])
    ba, bs, frames = int(inf["bits_allocated"]), int(inf["bits_stored"]), int(inf["frames"])
    signed = inf["signed"] == "1"
    kind = int(inf["kind"])
    if spp == 1:
        data, err = ours("frames", path)
        if data is None:
            return None, err
        if ba == 1:
            fb = int(inf["frame_bytes"])
            px = []
            for f in range(frames):
                bits = np.unpackbits(np.frombuffer(data[f * fb:(f + 1) * fb], np.uint8), bitorder="little")
                px.append(bits[: rows * cols])
            return np.array(px).reshape(frames, rows, cols).astype(np.int64), ""
        if kind == 1:
            return np.frombuffer(data, "<f4").reshape(frames, rows, cols), ""
        if kind == 2:
            return np.frombuffer(data, "<f8").reshape(frames, rows, cols), ""
        a = np.frombuffer(data, f"<u{ba // 8}").reshape(frames, rows, cols)
        return stored(a, bs, signed), ""
    data, err = ours("full", path)
    if data is None:
        return None, err
    a = np.frombuffer(data, f"<u{ba // 8}").reshape(frames, rows, cols, spp)
    return stored(a, bs, False), ""


def pyd_raw(ds, path):
    """pydicom's raw decode; from the parsed data set first (handles Deflated), else from the path."""
    try:
        return pixel_array(ds, raw=True)
    except Exception:  # noqa: BLE001
        return pixel_array(path, raw=True)


def pyd_samples(ds, path, inf):
    arr = pyd_raw(ds, path)
    frames = int(inf["frames"])
    arr = arr.reshape((frames,) + tuple(arr.shape[-3:] if int(inf["spp"]) > 1 else arr.shape[-2:]))
    if int(inf["kind"]) != 0:
        return arr
    if int(inf["bits_allocated"]) == 1:
        return arr.astype(np.int64)
    return stored(arr, int(inf["bits_stored"]), inf["signed"] == "1" and int(inf["spp"]) == 1)


def voi_range(ds):
    """The output range pydicom's apply_windowing uses (so both sides compute the same function)."""
    if ds.get("ModalityLUTSequence"):
        return 0.0, float(2 ** ds.ModalityLUTSequence[0].LUTDescriptor[2] - 1)
    bs = int(ds.BitsStored)
    if ds.PixelRepresentation == 0:
        lo, hi = 0.0, float(2 ** bs - 1)
    else:
        lo, hi = float(-(2 ** (bs - 1))), float(2 ** (bs - 1) - 1)
    if ds.get("RescaleSlope") is not None and ds.get("RescaleIntercept") is not None:
        s, i = float(ds.RescaleSlope), float(ds.RescaleIntercept)
        lo, hi = lo * s + i, hi * s + i
    return lo, hi


def check(path):
    name = os.path.basename(path)
    try:
        ds = pydicom.dcmread(path, force=True)
    except Exception as e:  # noqa: BLE001
        return name, "?", "skip", f"pydicom cannot read: {e}"
    if not any(k in ds for k in ("PixelData", "FloatPixelData", "DoubleFloatPixelData")):
        return None
    ts = str(ds.file_meta.get("TransferSyntaxUID", "?")) if hasattr(ds, "file_meta") else "?"
    inf, r = info(path)
    try:
        pyd_ok = True
        pyd_raw(ds, path)
    except Exception as e:  # noqa: BLE001
        pyd_ok = False
        pyd_err = str(e).splitlines()[0][:80]
    if inf is None:
        msg = r.stderr.strip()
        if r.returncode == 3:
            return name, ts, "unsupported", msg
        if not pyd_ok:
            return name, ts, "both-reject", f"ours: {msg} | pydicom: {pyd_err}"
        return name, ts, "FAIL", f"we reject, pydicom decodes: {msg}"
    if not pyd_ok:
        a, err = our_samples(path, inf)
        ourmsg = "we decode" if a is not None else f"we also fail: {err}"
        return name, ts, "pydicom-rejects", f"{ourmsg}; pydicom: {pyd_err}"
    parts = []
    status = "ok"
    a, err = our_samples(path, inf)
    if a is None:
        return name, ts, ("unsupported" if "error 6" in err else "FAIL"), err
    b = pyd_samples(ds, path, inf)
    d = diff(a, b)
    parts.append(f"frames {d}")
    if d != "match":
        status = "differs"
        if a.ndim == 4 and a.shape == b.shape:  # per-sample maximum difference (Y/Cb/Cr or R/G/B)
            per = [float(np.abs(a[..., k].astype(np.float64) - b[..., k]).max()) for k in range(a.shape[-1])]
            parts.append("per-sample max " + "/".join(f"{x:g}" for x in per))
    spp = int(inf["spp"])
    photo = inf["photometric"]
    if spp == 3:
        data, err = ours("rgb", path)
        if data is not None:
            rgb = np.frombuffer(data, f"<u{int(inf['bits_allocated']) // 8}")
            try:
                prgb = pixel_array(path, raw=False)
                parts.append(f"rgb {diff(rgb, prgb)}")
            except Exception as e:  # noqa: BLE001
                parts.append(f"rgb (pydicom: {e})")
        else:
            parts.append(f"rgb error: {err}")
    if photo == "PALETTE COLOR":
        data, err = ours("palette", path)
        if data is None:
            parts.append(f"palette error: {err}")
            status = "FAIL"
        else:
            pal = np.frombuffer(data, "<u2")
            pd = diff(pal, apply_color_lut(pyd_raw(ds, path), ds))
            parts.append(f"palette {pd}")
            if pd != "match":
                status = "differs"
    if spp == 1 and photo.startswith("MONOCHROME") and inf["kind"] == "0":
        raw = pyd_raw(ds, path)
        data, err = ours("modality", path)
        if data is not None:
            mod = np.frombuffer(data, "<f8")
            pm = apply_modality_lut(raw, ds)
            md = diff(mod, pm)
            parts.append(f"modality {md}")
            if md != "match":
                status = "differs"
            has_win = "WindowCenter" in ds and "WindowWidth" in ds
            if has_win or "VOILUTSequence" in ds:
                lo, hi = voi_range(ds)
                data, err = ours("voi", path, repr(lo), repr(hi))
                if data is None:
                    parts.append(f"voi error: {err}")
                else:
                    ds2 = pydicom.dcmread(path, force=True)
                    pv = apply_windowing(pm, ds2) if has_win else apply_voi(pm, ds2)
                    vd = diff(np.frombuffer(data, "<f8"), pv)
                    parts.append(f"voi {vd}")
                    if vd != "match":
                        status = "differs"
        else:
            parts.append(f"modality error: {err}")
    return name, ts, status, "; ".join(parts)


def main():
    files = sys.argv[2:] or sorted(get_testdata_files("*"))
    rows = []
    for p in files:
        r = check(p)
        if r:
            rows.append(r)
            print(f"{r[2]:16} {r[0]:45} {r[1]:24} {r[3]}")
    counts = {}
    for r in rows:
        counts[r[2]] = counts.get(r[2], 0) + 1
    print("\n" + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items())))
    sys.exit(1 if counts.get("FAIL") else 0)


if __name__ == "__main__":
    main()
