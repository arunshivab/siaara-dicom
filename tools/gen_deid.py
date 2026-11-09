#!/usr/bin/env python3
"""Generates src/deid/deid_table.inc from the DICOM standard's DocBook XML.

Usage:
    python3 tools/gen_deid.py <part15.xml> <part16.xml> <output .inc>

Sources (https://dicom.nema.org/medical/dicom/current/source/docbook/):
    part15/part15.xml : Table E.1-1  Application Level Confidentiality Profile Attributes
                        Table E.3.10-1 Safe Private Attributes
    part16/part16.xml : CID 7050 De-identification Method

Only the Python standard library is used. The output is deterministic (rows are sorted, no
timestamps), so a test can check that the committed file matches the generator.
"""
import re
import sys
import xml.etree.ElementTree as ET

DB = "{http://docbook.org/ns/docbook}"
XML_ID = "{http://www.w3.org/XML/1998/namespace}id"

# Table E.1-1 columns from "Basic Prof." onwards, and the C++ names used for them.
EXPECTED_HEADER = [
    "Attribute Name", "Tag", "Retd.", "In Std. Comp. IOD", "Basic Prof.",
    "Rtn. Safe Priv. Opt.", "Rtn. UIDs Opt.", "Rtn. Dev. Id. Opt.", "Rtn. Inst. Id. Opt.",
    "Rtn. Pat. Chars. Opt.", "Rtn. Long. Full Dates Opt.", "Rtn. Long. Modif. Dates Opt.",
    "Clean Desc. Opt.", "Clean Struct. Cont. Opt.", "Clean Graph. Opt.",
]
OPTION_NAMES = [
    "RetainSafePrivate", "RetainUids", "RetainDeviceIdentity", "RetainInstitutionIdentity",
    "RetainPatientCharacteristics", "RetainLongitudinalFullDates",
    "RetainLongitudinalModifiedDates", "CleanDescriptors", "CleanStructuredContent",
    "CleanGraphics",
]
ACTIONS = {
    "": "None", "D": "D", "Z": "Z", "X": "X", "K": "K", "C": "C", "U": "U",
    "Z/D": "ZD", "X/Z": "XZ", "X/D": "XD", "X/Z/D": "XZD", "X/Z/U*": "XZU",
}
KNOWN_VRS = {
    "AE", "AS", "AT", "CS", "DA", "DS", "DT", "FL", "FD", "IS", "LO", "LT", "OB", "OD", "OF",
    "OL", "OV", "OW", "PN", "SH", "SL", "SQ", "SS", "ST", "SV", "TM", "UC", "UI", "UL", "UN",
    "UR", "US", "UT", "UV",
}


def text(node):
    s = "".join(node.itertext())
    s = s.replace("​", "").replace(" ", " ")
    return re.sub(r"\s+", " ", s).strip()


def find_table(root, table_id):
    for tbl in root.iter(DB + "table"):
        if tbl.get(XML_ID) == table_id:
            return tbl
    raise SystemExit(f"table {table_id} not found")


def header(tbl):
    return [text(th) for th in tbl.find(DB + "thead").iter(DB + "th")]


def rows(tbl):
    for tr in tbl.find(DB + "tbody").iter(DB + "tr"):
        tds = tr.findall(DB + "td")
        for td in tds:
            if td.get("rowspan", "1") != "1" or td.get("colspan", "1") != "1":
                raise SystemExit(f"unexpected row/col span in {tbl.get(XML_ID)}")
        yield [text(td) for td in tds]


def edition(root):
    sub = root.find(DB + "subtitle")
    m = re.match(r"DICOM (PS3\.\d+ \d{4}[a-z]?)", text(sub) if sub is not None else "")
    if not m:
        raise SystemExit("edition not found")
    return m.group(1)


def c_str(s):
    if any(ord(ch) > 0x7E or ord(ch) < 0x20 for ch in s):
        raise SystemExit(f"non-ASCII text in table: {s!r}")
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse_tag(tag_text):
    """'(0008,0050)' -> (value, mask); 'x' digits become 0 in the value and the mask."""
    m = re.fullmatch(r"\(([0-9A-Fa-fx]{4}),([0-9A-Fa-fx]{4})\)", tag_text.replace(" ", ""))
    if not m:
        return None
    raw = (m.group(1) + m.group(2)).upper()
    value = int(raw.replace("X", "0"), 16)
    mask = int("".join("0" if ch == "X" else "F" for ch in raw), 16)
    return value, mask


def gen_profile(root15, out):
    tbl = find_table(root15, "table_E.1-1")
    hdr = header(tbl)
    norm = [re.sub(r" \(from.*$", "", h) for h in hdr]
    if norm != EXPECTED_HEADER:
        raise SystemExit(f"Table E.1-1 header changed: {hdr}")
    entries = []
    private_row = None
    for r in rows(tbl):
        if len(r) != len(EXPECTED_HEADER):
            raise SystemExit(f"Table E.1-1 row has {len(r)} cells: {r[:2]}")
        name, tag_text, retired, in_iod = r[0], r[1], r[2], r[3]
        acts = r[4:]
        for a in acts:
            if a not in ACTIONS:
                raise SystemExit(f"unknown action {a!r} for {name}")
        if acts[0] == "":
            raise SystemExit(f"no Basic Profile action for {name}")
        if retired not in ("Y", "N") or in_iod not in ("Y", "N"):
            raise SystemExit(f"bad Retd./In IOD flags for {name}")
        if tag_text.startswith("(gggg,eeee)"):
            private_row = (name, acts)
            continue
        t = parse_tag(tag_text)
        if t is None:
            raise SystemExit(f"cannot parse tag {tag_text!r} for {name}")
        entries.append((t[0], t[1], name, retired == "Y", in_iod == "Y", acts))
    if private_row is None:
        raise SystemExit("Private Attributes row not found")
    entries.sort(key=lambda e: (e[0], e[1]))
    seen = set()
    for e in entries:
        if (e[0], e[1]) in seen:
            raise SystemExit(f"duplicate tag {e[0]:08X}")
        seen.add((e[0], e[1]))

    def acts_init(acts):
        return "A::" + ACTIONS[acts[0]] + ", {" + ", ".join("A::" + ACTIONS[a] for a in acts[1:]) + "}"

    out.append(f"// Table E.1-1: {len(entries)} attribute rows (plus the Private Attributes row).")
    out.append("// Fields: tag (x positions zeroed), mask, retired, in standard composite IOD, Basic Profile")
    out.append("// action, then the option columns in this order:")
    out.append("//   " + ", ".join(OPTION_NAMES))
    out.append("constexpr ProfileRow kProfileRows[] = {")
    for tag, mask, name, retired, in_iod, acts in entries:
        out.append(f"    {{0x{tag:08X}u, 0x{mask:08X}u, {'true' if retired else 'false'}, "
                   f"{'true' if in_iod else 'false'}, {acts_init(acts)}, {c_str(name)}}},")
    out.append("};")
    out.append(f"constexpr std::size_t kProfileRowCount = {len(entries)};")
    out.append("// Row \"Private Attributes (gggg,eeee) where gggg is odd\".")
    out.append(f"constexpr ProfileRow kPrivateAttributesRow = {{0x00000000u, 0x00000000u, false, false, "
               f"{acts_init(private_row[1])}, {c_str(private_row[0])}}};")
    out.append("")
    return len(entries)


def gen_safe_private(root15, out):
    tbl = find_table(root15, "table_E.3.10-1")
    hdr = header(tbl)
    if hdr != ["Data Element", "Private Creator", "VR", "VM", "Meaning"]:
        raise SystemExit(f"Table E.3.10-1 header changed: {hdr}")
    entries = set()
    for r in rows(tbl):
        if len(r) != 5:
            raise SystemExit(f"Table E.3.10-1 row has {len(r)} cells")
        m = re.fullmatch(r"\(([0-9A-Fa-f]{4}),[xX]{2}([0-9A-Fa-f]{2})\)", r[0].replace(" ", ""))
        if not m:
            raise SystemExit(f"cannot parse private tag {r[0]!r}")
        group = int(m.group(1), 16)
        if group % 2 == 0:
            raise SystemExit(f"safe private group {group:04X} is even")
        elem = int(m.group(2), 16)
        creator = r[1]
        vrs = [v for v in re.split(r"[/ ]+", r[2]) if v]
        for v in vrs:
            if v not in KNOWN_VRS:
                raise SystemExit(f"unknown VR {r[2]!r} in Table E.3.10-1")
        is_sq = "SQ" in vrs
        entries.add((group, creator, elem, is_sq))
    entries = sorted(entries)
    out.append(f"// Table E.3.10-1: {len(entries)} safe private attributes, sorted by group, creator, element.")
    out.append("// Fields: group, private creator, element offset (the xx-relative low byte), VR is SQ.")
    out.append("constexpr SafePrivateRow kSafePrivateRows[] = {")
    for group, creator, elem, is_sq in entries:
        out.append(f"    {{0x{group:04X}u, {c_str(creator)}, 0x{elem:02X}u, {'true' if is_sq else 'false'}}},")
    out.append("};")
    out.append(f"constexpr std::size_t kSafePrivateRowCount = {len(entries)};")
    out.append("")
    return len(entries)


def gen_cid7050(root16, out):
    tbl = find_table(root16, "table_CID_7050")
    hdr = header(tbl)
    if hdr != ["Coding Scheme Designator", "Code Value", "Code Meaning"]:
        raise SystemExit(f"CID 7050 header changed: {hdr}")
    codes = []
    for r in rows(tbl):
        if len(r) != 3:
            raise SystemExit("CID 7050 row has the wrong cell count")
        codes.append((r[0], r[1], r[2]))
    if len({c[1] for c in codes}) != len(codes):
        raise SystemExit("duplicate CID 7050 code")
    out.append(f"// PS3.16 CID 7050 De-identification Method: {len(codes)} codes, in table order.")
    out.append("constexpr MethodCode kCid7050[] = {")
    for scheme, value, meaning in codes:
        if len(meaning) > 64 or len(value) > 16:
            raise SystemExit(f"code {value} too long for SH/LO")
        out.append(f"    {{{c_str(scheme)}, {c_str(value)}, {c_str(meaning)}}},")
    out.append("};")
    out.append(f"constexpr std::size_t kCid7050Count = {len(codes)};")
    out.append("")
    return len(codes)


def main(argv):
    if len(argv) != 4:
        raise SystemExit(__doc__)
    root15 = ET.parse(argv[1]).getroot()
    root16 = ET.parse(argv[2]).getroot()
    out = [
        "// Generated by tools/gen_deid.py - do not edit.",
        f"// Sources: DICOM {edition(root15)} Tables E.1-1 and E.3.10-1; DICOM {edition(root16)} CID 7050.",
        "// Included by src/deid/deid_tables.cpp inside namespace siaara::deid::detail.",
        "",
    ]
    n1 = gen_profile(root15, out)
    n2 = gen_safe_private(root15, out)
    n3 = gen_cid7050(root16, out)
    data = "\n".join(out) + "\n"
    with open(argv[3], "w", encoding="ascii", newline="\n") as f:
        f.write(data)
    print(f"profile rows: {n1}, safe private rows: {n2}, CID 7050 codes: {n3}")


if __name__ == "__main__":
    main(sys.argv)
