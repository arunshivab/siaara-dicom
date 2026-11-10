#!/usr/bin/env python3
"""Downloads DICOM standard parts pinned by SHA-256 in tools/ci/dicom_standard.json.

    python3 tools/ci/fetch_standard.py --out /tmp/dicom part03 part04 part06 part07 part15 part16

For each part, tries https://dicom.nema.org/medical/dicom/<edition>/source/docbook/<p>/<p>.xml,
then .../current/...; a download whose SHA-256 differs from the pin is rejected. Exits non-zero
when any part cannot be obtained with the pinned hash (e.g. 'current' moved to a newer edition
before NEMA published the archived copy). Standard library only.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = "https://dicom.nema.org/medical/dicom/%s/source/docbook/%s/%s.xml"


def fetch(url):
    with urllib.request.urlopen(url, timeout=600) as r:  # nosec - fixed https host
        return r.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("parts", nargs="+")
    args = ap.parse_args()
    with open(os.path.join(HERE, "dicom_standard.json"), encoding="utf-8") as f:
        pins = json.load(f)
    os.makedirs(args.out, exist_ok=True)
    bad = 0
    for part in args.parts:
        want = pins["parts"][part]
        got = None
        for edition in (pins["edition"], "current"):
            url = BASE % (edition, part, part)
            try:
                data = fetch(url)
            except Exception as e:  # pylint: disable=broad-except
                print("%s: %s (%s)" % (part, url, e))
                continue
            h = hashlib.sha256(data).hexdigest()
            if h != want:
                print("%s: %s has SHA-256 %s, pinned %s (not edition %s)" % (part, url, h, want, pins["edition"]))
                continue
            got = data
            print("%s: %s (SHA-256 ok)" % (part, url))
            break
        if got is None:
            bad += 1
            continue
        with open(os.path.join(args.out, part + ".xml"), "wb") as f:
            f.write(got)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
