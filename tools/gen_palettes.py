#!/usr/bin/env python3
"""Generates src/image/palettes_data.inc from the DICOM standard's DocBook XML (PS3.6 Annex B).

Usage:
    python3 tools/gen_palettes.py <part06.xml> <output .inc>

Source (download from https://dicom.nema.org/medical/dicom/current/source/docbook/):
    part06/part06.xml : tables B.1.x.2-1 (palette descriptors) and B.1.x.2-2 (palette data) for
                        Hot Iron (B.1.1), PET (B.1.2), Hot Metal Blue (B.1.3) and PET 20 Step (B.1.4).

Only the Python standard library is used. The output is deterministic so CI can check that the
committed file matches the generator.
"""
import re
import sys
import xml.etree.ElementTree as ET

DB = "{http://docbook.org/ns/docbook}"
XML_ID = "{http://www.w3.org/XML/1998/namespace}id"

# (C++ identifier, section number, Content Label the section requires, SOP Instance UID of PS3.6 table B-1)
PALETTES = [
    ("kHotIron", "B.1.1", "HOT_IRON", "1.2.840.10008.1.5.1"),
    ("kPet", "B.1.2", "PET", "1.2.840.10008.1.5.2"),
    ("kHotMetalBlue", "B.1.3", "HOT_METAL_BLUE", "1.2.840.10008.1.5.3"),
    ("kPet20Step", "B.1.4", "PET_20_STEP", "1.2.840.10008.1.5.4"),
]


def text(node):
    s = "".join(node.itertext())
    s = s.replace("​", "").replace(" ", " ")
    return re.sub(r"\s+", " ", s).strip()


def find_table(root, table_id):
    for tbl in root.iter(DB + "table"):
        if tbl.get(XML_ID) == table_id:
            return tbl
    raise SystemExit(f"table {table_id} not found")


def table_rows(tbl):
    body = tbl.find(DB + "tbody")
    for tr in body.iter(DB + "tr"):
        yield [text(td) for td in tr.findall(DB + "td")]


def header(tbl):
    head = tbl.find(DB + "thead")
    return [text(th) for th in head.iter(DB + "th")]


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    root = ET.parse(sys.argv[1]).getroot()
    out = []
    out.append("// GENERATED FILE - do not edit. Produced by tools/gen_palettes.py from DICOM PS3.6 Annex B.")
    out.append("// Tables B.1.1.2-1/-2 (Hot Iron), B.1.2.2-1/-2 (PET), B.1.3.2-1/-2 (Hot Metal Blue),")
    out.append("// B.1.4.2-1/-2 (PET 20 Step).")
    out.append("")
    for ident, sect, label, uid in PALETTES:
        desc_tbl = find_table(root, f"table_{sect}.2-1")
        data_tbl = find_table(root, f"table_{sect}.2-2")
        drows = list(table_rows(desc_tbl))
        if len(drows) != 1 or len(drows[0]) != 3:
            raise SystemExit(f"{sect}: unexpected descriptor table shape")
        entries, first, bits = (int(v) for v in drows[0])
        if header(data_tbl) != ["Red", "Green", "Blue"]:
            raise SystemExit(f"{sect}: unexpected data table header {header(data_tbl)}")
        rows = []
        for r in table_rows(data_tbl):
            if len(r) != 3:
                raise SystemExit(f"{sect}: data row with {len(r)} cells")
            vals = [int(v) for v in r]
            for v in vals:
                if v < 0 or v >= (1 << bits):
                    raise SystemExit(f"{sect}: value {v} does not fit {bits} bits")
            rows.append(vals)
        if len(rows) != entries:
            raise SystemExit(f"{sect}: {len(rows)} rows but the descriptor says {entries}")
        out.append(f"// {sect}: Content Label \"{label}\", descriptor {entries}\\{first}\\{bits}, UID {uid}")
        out.append(f"constexpr std::uint16_t {ident}Descriptor[3] = {{{entries}, {first}, {bits}}};")
        for ci, cname in enumerate(("Red", "Green", "Blue")):
            out.append(f"constexpr std::uint8_t {ident}{cname}[{entries}] = {{")
            vals = [row[ci] for row in rows]
            for i in range(0, len(vals), 16):
                out.append("    " + ", ".join(str(v) for v in vals[i:i + 16]) + ",")
            out.append("};")
        out.append("")
    with open(sys.argv[2], "w", newline="\n") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
