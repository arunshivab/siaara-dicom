#!/usr/bin/env python3
"""Regenerates src/repair/iod_tables.inc with tools/gen_iod.py from the standard's XML and checks
that the committed file is identical (the generator is deterministic).

    python3 -I tests/crosscheck/repair_iod_table_check.py <repo root> <part03.xml> <part04.xml> <part16.xml>
"""
import os
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    root, p3, p4, p16 = sys.argv[1:]
    committed = os.path.join(root, "src", "repair", "iod_tables.inc")
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "iod_tables.inc")
        subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "gen_iod.py"), p3, p4, p16, out], check=True,
                       stdout=subprocess.DEVNULL)
        with open(out, "rb") as a, open(committed, "rb") as b:
            if a.read() != b.read():
                print("src/repair/iod_tables.inc differs from the generator output; rerun tools/gen_iod.py")
                return 1
    print("src/repair/iod_tables.inc matches tools/gen_iod.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
