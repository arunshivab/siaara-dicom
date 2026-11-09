#!/usr/bin/env python3
"""Regenerates src/deid/deid_table.inc with tools/gen_deid.py and checks it equals the
committed file byte for byte.

Usage: deid_table_check.py <source root> <part15.xml> <part16.xml>
"""
import os
import subprocess
import sys
import tempfile


def main(argv):
    if len(argv) != 4:
        raise SystemExit(__doc__)
    root, p15, p16 = argv[1:]
    committed = os.path.join(root, "src", "deid", "deid_table.inc")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "deid_table.inc")
        subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "gen_deid.py"), p15, p16, out],
                       check=True)
        with open(out, "rb") as a, open(committed, "rb") as b:
            if a.read() != b.read():
                print("src/deid/deid_table.inc differs from the generator output")
                return 1
    print("src/deid/deid_table.inc matches the generator output")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
