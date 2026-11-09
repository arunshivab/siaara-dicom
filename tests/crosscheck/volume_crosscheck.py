#!/usr/bin/env python3
"""Cross-checks the SIAARA volume layer against independent implementations (test oracles only).

  scipy.ndimage.map_coordinates: trilinear (order=1) and cubic B-spline without prefilter
      (order=3, prefilter=False) resampling and MPR planes, mode='nearest' (edge replication);
  numpy: a Catmull-Rom cubic convolution oracle written here;
  SimpleITK (optional): ImageSeriesReader on DICOM series written with pydicom (oblique
      orientation, shuffled file order, per-slice rescale) - origin, spacing, direction, voxels.

Usage: volume_crosscheck.py <path to volume_tool>
Requires numpy and scipy (pip install --break-system-packages numpy scipy); SimpleITK and pydicom
are optional (their checks are skipped when missing).
"""
import os
import random
import subprocess
import sys
import tempfile

import numpy as np
from scipy import ndimage

TOOL = None
FAILS = 0


def check(cond, what):
    global FAILS
    print(("[ ok ] " if cond else "[FAIL] ") + what)
    if not cond:
        FAILS += 1


def rot(a, b):
    x = np.array([np.cos(a), np.sin(a), 0.0])
    y = np.array([-np.sin(a), np.cos(a), 0.0])
    z = np.array([0.0, 0.0, 1.0])
    rx = np.array([[1, 0, 0], [0, np.cos(b), -np.sin(b)], [0, np.sin(b), np.cos(b)]])
    return [rx @ x, rx @ y, rx @ z]


def write_grid(path, dims, spacing, origin, axes):
    with open(path, "w") as f:
        f.write("dims %d %d %d\n" % tuple(dims))
        f.write("spacing %.17g %.17g %.17g\n" % tuple(spacing))
        f.write("origin %.17g %.17g %.17g\n" % tuple(origin))
        f.write("axes " + " ".join("%.17g" % v for a in axes for v in a) + "\n")


def read_kv(path):
    out = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if p:
                out[p[0]] = [float(v) for v in p[1:]]
    return out


def index_of(points, origin, spacing, axes):
    """Patient points (N,3) -> continuous (i, j, k) indices (N,3), solved independently."""
    m = np.stack([axes[0] * spacing[0], axes[1] * spacing[1], axes[2] * spacing[2]], axis=1)
    return np.linalg.solve(m, (points - origin).T).T


def inside(idx, dims):
    ok = np.ones(len(idx), dtype=bool)
    for a in range(3):
        ok &= (idx[:, a] >= -0.5) & (idx[:, a] <= dims[a] - 0.5)
    return ok


def catmull_rom(vol, idx):
    """Separable Catmull-Rom cubic convolution with clamped neighbours. vol is (nz, ny, nx)."""
    nz, ny, nx = vol.shape
    f = np.floor(idx)
    t = idx - f
    f = f.astype(np.int64)

    def w(t):
        t2, t3 = t * t, t * t * t
        return [0.5 * (-t3 + 2 * t2 - t), 0.5 * (3 * t3 - 5 * t2 + 2), 0.5 * (-3 * t3 + 4 * t2 + t), 0.5 * (t3 - t2)]

    wx, wy, wz = w(t[:, 0]), w(t[:, 1]), w(t[:, 2])
    out = np.zeros(len(idx))
    for c in range(4):
        zi = np.clip(f[:, 2] - 1 + c, 0, nz - 1)
        for b in range(4):
            yi = np.clip(f[:, 1] - 1 + b, 0, ny - 1)
            for a in range(4):
                xi = np.clip(f[:, 0] - 1 + a, 0, nx - 1)
                out += wz[c] * wy[b] * wx[a] * vol[zi, yi, xi]
    return out


def oracle(vol, idx, interp):
    coords = idx[:, ::-1].T  # (z, y, x)
    if interp == "linear":
        return ndimage.map_coordinates(vol.astype(np.float64), coords, order=1, mode="nearest")
    if interp == "bspline":
        return ndimage.map_coordinates(vol.astype(np.float64), coords, order=3, mode="nearest", prefilter=False)
    if interp == "nearest":
        r = np.floor(idx + 0.5).astype(np.int64)
        for a, n in enumerate((vol.shape[2], vol.shape[1], vol.shape[0])):
            r[:, a] = np.clip(r[:, a], 0, n - 1)
        return vol[r[:, 2], r[:, 1], r[:, 0]].astype(np.float64)
    return catmull_rom(vol.astype(np.float64), idx)


def run(*args):
    r = subprocess.run([TOOL] + [str(a) for a in args], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
    return r.returncode == 0


def resample_and_mpr(tmp):
    rng = np.random.default_rng(5)
    dims = (37, 31, 23)
    spacing = (0.9, 1.1, 1.7)
    origin = np.array([-20.0, 15.5, -60.25])
    axes = rot(0.35, -0.2)
    # Smooth-ish data plus noise so every kernel matters.
    z, y, x = np.mgrid[0:dims[2], 0:dims[1], 0:dims[0]]
    vol = (100 * np.sin(x / 4.0) * np.cos(y / 5.0) + 3 * z + rng.normal(0, 20, (dims[2], dims[1], dims[0]))).astype(np.float32)
    src_raw, src_txt = os.path.join(tmp, "src.raw"), os.path.join(tmp, "src.txt")
    vol.tofile(src_raw)
    write_grid(src_txt, dims, spacing, origin, axes)

    tdims = (41, 29, 27)
    tsp = (0.77, 1.03, 1.31)
    taxes = rot(-0.4, 0.3)
    torigin = origin + np.array([-3.0, 2.0, 1.5])
    tgt_txt = os.path.join(tmp, "tgt.txt")
    write_grid(tgt_txt, tdims, tsp, torigin, taxes)
    k, j, i = np.mgrid[0:tdims[2], 0:tdims[1], 0:tdims[0]]
    pts = torigin + np.stack([i.ravel(), j.ravel(), k.ravel()], axis=1) @ np.stack(
        [taxes[0] * tsp[0], taxes[1] * tsp[1], taxes[2] * tsp[2]])
    idx = index_of(pts, origin, spacing, axes)
    ins = inside(idx, dims)
    for interp in ("nearest", "linear", "cubic", "bspline"):
        out_raw = os.path.join(tmp, "res_%s.raw" % interp)
        ok = run("resample", src_raw, src_txt, tgt_txt, interp, out_raw)
        if not ok:
            check(False, "resample %s ran" % interp)
            continue
        got = np.fromfile(out_raw, dtype=np.float32).astype(np.float64)
        exp = oracle(vol, idx, interp)
        nan_ok = np.array_equal(np.isnan(got), ~ins)
        err = np.max(np.abs(got[ins] - exp[ins])) if ins.any() else 0.0
        tol = 0.0 if interp == "nearest" else 1e-5 * float(np.max(np.abs(vol)))
        # Nearest: ignore points within 1e-6 of a rounding tie.
        if interp == "nearest":
            frac = np.abs((idx - np.floor(idx)) - 0.5)
            safe = ins & np.all(frac > 1e-6, axis=1)
            err = np.max(np.abs(got[safe] - exp[safe]))
        check(nan_ok and err <= tol,
              "resample %-7s vs %s: %d inside voxels, max |diff| %.3g (tol %.3g), outside mask %s" %
              (interp, "scipy map_coordinates" if interp in ("linear", "bspline") else "numpy oracle", ins.sum(), err, tol,
               "matches" if nan_ok else "DIFFERS"))

    # Oblique MPR planes (with and without a slab average... thin only: planes are the oracle).
    for n, (normal, up, ps) in enumerate([((0.3, -0.2, 0.93), (0.1, -1.0, 0.2), 0.45), ((0.9, 0.4, -0.1), (0, 0, 1), 0.61)]):
        w, h = 70, 55
        point = origin + axes[0] * 15 + axes[1] * 13 + axes[2] * 9.3
        vtxt = os.path.join(tmp, "view%d.txt" % n)
        with open(vtxt, "w") as f:
            f.write("size %d %d\npoint %.17g %.17g %.17g\n" % (w, h, *point))
            f.write("normal %.17g %.17g %.17g\nup %.17g %.17g %.17g\nspacing %.17g\n" % (*normal, *up, ps))
        nn = np.array(normal) / np.linalg.norm(normal)
        uu = np.array(up) - nn * np.dot(up, nn)
        uu /= np.linalg.norm(uu)
        right = np.cross(nn, uu)
        down = -uu
        vv, ux = np.mgrid[0:h, 0:w]
        pts = point + np.outer((ux.ravel() - (w - 1) / 2.0) * ps, right) + np.outer((vv.ravel() - (h - 1) / 2.0) * ps, down)
        idx = index_of(pts, origin, spacing, axes)
        ins = inside(idx, dims)
        for interp in ("linear", "cubic", "bspline"):
            out_raw = os.path.join(tmp, "mpr%d_%s.raw" % (n, interp))
            if not run("mpr", src_raw, src_txt, vtxt, interp, out_raw):
                check(False, "mpr ran")
                continue
            got = np.fromfile(out_raw, dtype=np.float32).astype(np.float64)
            exp = oracle(vol, idx, interp)
            err = np.max(np.abs(got[ins] - exp[ins]))
            nan_ok = np.array_equal(np.isnan(got), ~ins)
            tol = 1e-5 * float(np.max(np.abs(vol)))
            check(nan_ok and err <= tol, "oblique MPR %d %-7s: %d inside pixels, max |diff| %.3g, outside mask %s" %
                  (n, interp, ins.sum(), err, "matches" if nan_ok else "DIFFERS"))


def write_series(folder, per_slice_rescale, seed):
    import pydicom
    from pydicom.dataset import Dataset, FileMetaDataset
    from pydicom.uid import ExplicitVRLittleEndian, generate_uid

    rng = np.random.default_rng(seed)
    rows, cols, nslices = 40, 48, 18
    a, b = 0.3, 0.15
    axes = rot(a, b)
    rowdir, coldir = axes[0], axes[1]
    normal = np.cross(rowdir, coldir)
    origin = np.array([-100.25, 80.5, 300.0])
    series_uid, study_uid, for_uid = generate_uid(), generate_uid(), generate_uid()
    order = list(range(nslices))
    random.Random(seed).shuffle(order)
    truth = np.zeros((nslices, rows, cols))
    for n, k in enumerate(order):
        stored = rng.integers(-1000, 3000, size=(rows, cols)).astype(np.int16)
        slope = 1.0
        intercept = -1024.0 + (k * 3 if per_slice_rescale else 0)
        truth[k] = stored * slope + intercept
        ds = Dataset()
        ds.file_meta = FileMetaDataset()
        ds.file_meta.TransferSyntaxUID = ExplicitVRLittleEndian
        ds.file_meta.MediaStorageSOPClassUID = "1.2.840.10008.5.1.4.1.1.2"
        sop = generate_uid()
        ds.file_meta.MediaStorageSOPInstanceUID = sop
        ds.SOPClassUID = "1.2.840.10008.5.1.4.1.1.2"
        ds.SOPInstanceUID = sop
        ds.StudyInstanceUID, ds.SeriesInstanceUID, ds.FrameOfReferenceUID = study_uid, series_uid, for_uid
        ds.Modality = "CT"
        ds.PatientName = "Synthetic^Volume"
        ds.PatientID = "VOL"
        ds.InstanceNumber = n + 1  # deliberately unrelated to the position
        ds.ImagePositionPatient = [round(float(v), 6) for v in origin + normal * (2.5 * k)]
        ds.ImageOrientationPatient = [round(float(v), 8) for v in list(rowdir) + list(coldir)]
        ds.PixelSpacing = [0.8, 0.7]
        ds.SliceThickness = 2.5
        ds.Rows, ds.Columns = rows, cols
        ds.SamplesPerPixel = 1
        ds.PhotometricInterpretation = "MONOCHROME2"
        ds.BitsAllocated, ds.BitsStored, ds.HighBit, ds.PixelRepresentation = 16, 16, 15, 1
        ds.RescaleSlope, ds.RescaleIntercept = slope, intercept
        ds.PixelData = stored.tobytes()
        ds.save_as(os.path.join(folder, "img_%03d.dcm" % n), enforce_file_format=True)
    return truth, origin, normal, axes


def simpleitk_series(tmp):
    try:
        import SimpleITK as sitk
        import pydicom  # noqa: F401
    except ImportError as e:
        print("[skip] SimpleITK / pydicom not available: %s" % e)
        return
    for per_slice in (False, True):
        folder = os.path.join(tmp, "series_%d" % per_slice)
        os.makedirs(folder)
        truth, origin, normal, axes = write_series(folder, per_slice, 3 + per_slice)
        files = sorted(os.path.join(folder, f) for f in os.listdir(folder))
        prefix = os.path.join(tmp, "built_%d" % per_slice)
        if not run("build", prefix, *files):
            check(False, "build ran")
            continue
        g = read_kv(prefix + ".txt")
        dims = [int(v) for v in g["dims"]]
        ours = np.fromfile(prefix + ".raw", dtype=np.float32).reshape(dims[2], dims[1], dims[0])
        reader = sitk.ImageSeriesReader()
        names = reader.GetGDCMSeriesFileNames(folder)
        reader.SetFileNames(names)
        img = sitk.Cast(reader.Execute(), sitk.sitkFloat32)
        ref = sitk.GetArrayFromImage(img)
        label = "per-slice rescale" if per_slice else "common rescale"
        check(list(img.GetSize()) == dims, "SimpleITK %s: size %s vs ours %s" % (label, img.GetSize(), dims))
        check(np.allclose(img.GetOrigin(), g["origin"], atol=1e-4), "SimpleITK %s: origin %s vs ours %s" %
              (label, np.round(img.GetOrigin(), 4), np.round(g["origin"], 4)))
        check(np.allclose(img.GetSpacing(), g["spacing"], atol=1e-4), "SimpleITK %s: spacing %s vs ours %s" %
              (label, np.round(img.GetSpacing(), 5), np.round(g["spacing"], 5)))
        d = np.array(img.GetDirection()).reshape(3, 3)
        ax = np.array(g["axes"]).reshape(3, 3).T  # columns = axes
        check(np.allclose(d, ax, atol=1e-6), "SimpleITK %s: direction matrix matches (max diff %.2g)" % (label, np.max(np.abs(d - ax))))
        if ref.shape == ours.shape:
            diff = float(np.max(np.abs(ref - ours)))
            check(diff == 0.0, "SimpleITK %s: voxel values, max |diff| %g" % (label, diff))
        check(float(np.max(np.abs(ours - truth))) == 0.0, "pydicom ground truth %s: voxel values equal" % label)


def main():
    global TOOL
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    TOOL = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        resample_and_mpr(tmp)
        simpleitk_series(tmp)
    print("\n%d failure(s)" % FAILS)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
