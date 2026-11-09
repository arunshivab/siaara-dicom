#!/usr/bin/env python3
"""Cross-checks SIAARA's RLE Lossless codec against pydicom's RLE handler (test-only, never shipped).

pydicom encodes, SIAARA decodes; SIAARA encodes, pydicom decodes. Covers 8/16/32 bits allocated,
1 and 3 samples per pixel, planar configuration 0 and 1, odd and even sizes, and several data
patterns. Usage:
    python3 -I rle_crosscheck.py <path to siaara_codec_tool>
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from pydicom.dataset import Dataset, FileMetaDataset
from pydicom.encaps import encapsulate
from pydicom.pixels.encoders import RLELosslessEncoder
from pydicom.uid import RLELossless, generate_uid

tool = sys.argv[1]
rng = np.random.default_rng(20261109)


def make_array(pattern, rows, cols, spp, bits):
    dtype = {8: "<u1", 16: "<u2", 32: "<u4"}[bits]
    shape = (rows, cols) if spp == 1 else (rows, cols, spp)
    top = (1 << bits) - 1
    if pattern == "random":
        return rng.integers(0, top, size=shape, dtype=np.uint64, endpoint=True).astype(dtype)
    if pattern == "constant":
        return np.full(shape, top // 3, dtype=dtype)
    if pattern == "runs":  # long runs broken by occasional noise, typical of CT backgrounds
        a = np.repeat(rng.integers(0, 4, size=shape[0] * shape[1] // 7 + 8), 7)[: rows * cols]
        a = a.reshape(rows, cols) * (top // 5)
        if spp == 3:
            a = np.stack([a, a // 2, (a + 1) % (top + 1)], axis=-1)
        noise = rng.random(shape) < 0.02
        out = a.astype(np.uint64)
        out[noise] = rng.integers(0, top, size=int(noise.sum()), dtype=np.uint64, endpoint=True)
        return out.astype(dtype)
    if pattern == "ramp":
        a = (np.arange(rows * cols * spp, dtype=np.uint64) * 2654435761) % (top + 1)
        return a.reshape(shape).astype(dtype)
    raise ValueError(pattern)


def native_bytes(arr, spp, planar):
    if spp == 3 and planar:
        return np.ascontiguousarray(arr.transpose(2, 0, 1)).tobytes()
    return arr.tobytes()


def pydicom_decode(frame, rows, cols, spp, bits, planar):
    ds = Dataset()
    ds.file_meta = FileMetaDataset()
    ds.file_meta.TransferSyntaxUID = RLELossless
    ds.SOPClassUID = "1.2.840.10008.5.1.4.1.1.7"
    ds.SOPInstanceUID = generate_uid()
    ds.Rows, ds.Columns = rows, cols
    ds.SamplesPerPixel = spp
    ds.BitsAllocated = ds.BitsStored = bits
    ds.HighBit = bits - 1
    ds.PixelRepresentation = 0
    ds.PhotometricInterpretation = "RGB" if spp == 3 else "MONOCHROME2"
    if spp == 3:
        ds.PlanarConfiguration = planar
    ds.PixelData = encapsulate([frame])
    ds["PixelData"].is_undefined_length = True
    ds["PixelData"].VR = "OB"
    return ds.pixel_array


def run(args):
    r = subprocess.run([tool] + [str(a) for a in args], capture_output=True, text=True)
    return r.returncode, r.stderr.strip()


ok = bad = 0
sizes = [(1, 1), (1, 7), (5, 3), (16, 16), (17, 33), (64, 129), (256, 200)]
with tempfile.TemporaryDirectory() as tmp:
    raw_path = os.path.join(tmp, "raw")
    rle_path = os.path.join(tmp, "rle")
    out_path = os.path.join(tmp, "out")
    for bits in (8, 16, 32):
        for spp in (1, 3):
            for planar in ((0, 1) if spp == 3 else (0,)):
                for rows, cols in sizes:
                    for pattern in ("random", "constant", "runs", "ramp"):
                        label = f"bits={bits} spp={spp} planar={planar} {rows}x{cols} {pattern}"
                        arr = make_array(pattern, rows, cols, spp, bits)
                        native = native_bytes(arr, spp, planar)
                        geom = [rows, cols, spp, bits, planar]
                        problems = []

                        # pydicom encodes -> SIAARA decodes
                        theirs = RLELosslessEncoder.encode(
                            # pydicom's encoding profile table omits 32-bit RLE (Annex G allows it), so the
                            # profile check is skipped for that case only.
                            arr, encoding_plugin="pydicom", validate=bits != 32, rows=rows, columns=cols, samples_per_pixel=spp,
                            bits_allocated=bits, bits_stored=bits, pixel_representation=0,
                            photometric_interpretation="RGB" if spp == 3 else "MONOCHROME2",
                            planar_configuration=0, number_of_frames=1)
                        with open(rle_path, "wb") as f:
                            f.write(theirs)
                        rc, err = run(["rle-decode", *geom, rle_path, out_path])
                        if rc != 0:
                            problems.append(f"siaara decode failed: {err}")
                        else:
                            with open(out_path, "rb") as f:
                                if f.read() != native:
                                    problems.append("siaara decode of pydicom stream differs")

                        # SIAARA encodes -> pydicom decodes
                        with open(raw_path, "wb") as f:
                            f.write(native)
                        rc, err = run(["rle-encode", *geom, raw_path, rle_path])
                        if rc != 0:
                            problems.append(f"siaara encode failed: {err}")
                        else:
                            with open(rle_path, "rb") as f:
                                ours = f.read()
                            if len(ours) % 2:
                                problems.append("odd-length frame")
                            try:
                                back = pydicom_decode(ours, rows, cols, spp, bits, planar)
                                if not np.array_equal(back.reshape(arr.shape), arr):
                                    problems.append("pydicom decode of siaara stream differs")
                            except Exception as e:  # noqa: BLE001 - report any oracle failure
                                problems.append(f"pydicom decode failed: {e}")
                        if problems:
                            bad += 1
                            print(f"FAIL {label}: {'; '.join(problems)}")
                        else:
                            ok += 1
print(f"\n{ok} cases match, {bad} differ")
sys.exit(1 if bad else 0)
