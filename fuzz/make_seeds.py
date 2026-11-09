#!/usr/bin/env python3
"""Builds seed corpora for the fuzz targets in fuzz/targets/.

    python3 fuzz/make_seeds.py --tool build/fuzz/siaara_make_seeds --out fuzz/corpus
    python3 fuzz/make_seeds.py --tool build/fuzz/siaara_make_seeds --out /tmp/corpus --pydicom

1. Runs siaara_make_seeds (the toolkit's own writers and encoders) into --out/<target>/. These
   small seeds are the committed corpus in fuzz/corpus/.
2. With --pydicom, adds pydicom's test files (if pydicom is installed): whole files for the Part 10
   based targets, encapsulated frames extracted for the codec targets, the JSON test files and the
   DICOMDIR test set. These are copied at seed-generation time and are not committed.

Standard library only, apart from the optional pydicom import (used to locate and to unpack the
test files).
"""
import argparse
import os
import shutil
import subprocess
import sys

PART10_TARGETS = ("part10", "deid", "clinical")
CODEC_BY_TS = {
    "1.2.840.10008.1.2.5": "codec_rle",
    "1.2.840.10008.1.2.4.50": "codec_jpeg",
    "1.2.840.10008.1.2.4.51": "codec_jpeg",
    "1.2.840.10008.1.2.4.57": "codec_jpeg",
    "1.2.840.10008.1.2.4.70": "codec_jpeg",
    "1.2.840.10008.1.2.4.80": "codec_jpegls",
    "1.2.840.10008.1.2.4.81": "codec_jpegls",
    "1.2.840.10008.1.2.4.90": "codec_j2k",
    "1.2.840.10008.1.2.4.91": "codec_j2k",
    "1.2.840.10008.1.2.4.201": "codec_j2k",
    "1.2.840.10008.1.2.4.202": "codec_j2k",
    "1.2.840.10008.1.2.4.203": "codec_j2k",
}
MAX_FILE = 512 * 1024  # skip larger files: slow seeds help nobody


def put(out, target, name, data):
    d = os.path.join(out, target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def rle_prefix(ds):
    """The 4-byte frame description the codec_rle harness reads, or None."""
    rows, cols = int(ds.Rows), int(ds.Columns)
    if not (1 <= rows <= 64 and 1 <= cols <= 64):
        return None
    bits = {8: 0, 16: 1, 32: 2}.get(int(ds.BitsAllocated))
    if bits is None:
        return None
    spp = int(ds.SamplesPerPixel)
    flags = (1 if spp == 3 else 0) | (bits << 1) | (8 if int(getattr(ds, "PixelRepresentation", 0)) else 0)
    return bytes([rows - 1, cols - 1, flags, 0])


def add_pydicom(out):
    try:
        import pydicom
        from pydicom.data import get_testdata_file  # noqa: F401
        import pydicom.encaps as encaps
    except ImportError:
        print("pydicom not installed: skipping its test files", file=sys.stderr)
        return 0
    base = os.path.join(os.path.dirname(pydicom.__file__), "data", "test_files")
    n = 0
    for root, _dirs, files in os.walk(base):
        for name in sorted(files):
            path = os.path.join(root, name)
            if os.path.getsize(path) > MAX_FILE:
                continue
            with open(path, "rb") as f:
                data = f.read()
            stem = "pydicom_" + os.path.relpath(path, base).replace(os.sep, "_")
            if name.endswith(".json"):
                put(out, "dicom_json", stem, data)
                put(out, "json", stem, data)
                n += 2
                continue
            if name == "DICOMDIR":
                put(out, "dicomdir", stem, data)
                n += 1
                continue
            if not (name.endswith(".dcm") or name.startswith("DICOMDIR")):
                continue
            for t in PART10_TARGETS:
                put(out, t, stem, data)
                n += 1
            put(out, "frames_render", stem, data + b"\x00")
            n += 1
            try:
                ds = pydicom.dcmread(path, force=True)
                tsuid = str(ds.file_meta.TransferSyntaxUID)
            except Exception:  # pylint: disable=broad-except
                continue
            target = CODEC_BY_TS.get(tsuid)
            if not target or "PixelData" not in ds:
                continue
            try:
                frames = list(encaps.generate_frames(ds.PixelData, number_of_frames=int(getattr(ds, "NumberOfFrames", 1) or 1)))
            except Exception:  # pylint: disable=broad-except
                continue
            for i, frame in enumerate(frames[:2]):
                if target == "codec_rle":
                    prefix = rle_prefix(ds)
                    if prefix is None:
                        break
                    frame = prefix + frame
                put(out, target, "%s_f%d" % (stem, i), frame)
                n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True, help="path to the siaara_make_seeds executable")
    ap.add_argument("--out", required=True, help="output folder (one subfolder per target)")
    ap.add_argument("--pydicom", action="store_true", help="also copy pydicom's test files")
    ap.add_argument("--clean", action="store_true", help="empty the output folder first")
    args = ap.parse_args()
    if args.clean and os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)
    subprocess.run([args.tool, args.out], check=True)
    if args.pydicom:
        print("%d files added from pydicom" % add_pydicom(args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
