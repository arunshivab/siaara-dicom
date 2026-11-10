#!/usr/bin/env python3
"""Generates src/dict/dictionary_data.inc from the DICOM standard's DocBook XML.

Usage:
    python3 tools/gen_dictionary.py <part06.xml> <part07.xml> <output .inc>

Sources (download from https://dicom.nema.org/medical/dicom/current/source/docbook/):
    part06/part06.xml : tables 6-1, 7-1, 8-1, 9-1 (data elements) and A-1 (UIDs)
    part07/part07.xml : table E.1-1 (command elements) and E.2-1 (retired command elements)

Only the Python standard library is used. The output is deterministic so CI can check that the
committed file matches the generator.
"""
import re
import sys
import xml.etree.ElementTree as ET

DB = "{http://docbook.org/ns/docbook}"
XML_ID = "{http://www.w3.org/XML/1998/namespace}id"

KNOWN_VRS = {
    "AE", "AS", "AT", "CS", "DA", "DS", "DT", "FL", "FD", "IS", "LO", "LT", "OB", "OD", "OF",
    "OL", "OV", "OW", "PN", "SH", "SL", "SQ", "SS", "ST", "SV", "TM", "UC", "UI", "UL", "UN",
    "UR", "US", "UT", "UV",
}


def text(node):
    s = "".join(node.itertext())
    s = s.replace("​", "").replace(" ", " ")
    return re.sub(r"\s+", " ", s).strip()


def table_rows(root, table_id):
    for tbl in root.iter(DB + "table"):
        if tbl.get(XML_ID) == table_id:
            body = tbl.find(DB + "tbody")
            for tr in body.iter(DB + "tr"):
                yield [text(td) for td in tr.findall(DB + "td")]
            return
    raise SystemExit(f"table {table_id} not found")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse_tag(tag_text):
    m = re.fullmatch(r"\(([0-9A-Fa-fx]{4}),([0-9A-Fa-fx]{4})\)", tag_text.replace(" ", ""))
    if not m:
        return None
    raw = (m.group(1) + m.group(2)).upper()
    value = int(raw.replace("X", "0"), 16)
    mask = int("".join("0" if ch == "X" else "F" for ch in raw), 16)
    return value, mask


def parse_vr(vr_text):
    # Examples: "US", "US or SS", "OB or OW", "US or SS or OW", "" (item delimiters), "See Note"
    vrs = [v for v in re.findall(r"\b[A-Z]{2}\b", vr_text) if v in KNOWN_VRS]
    vrs = (vrs + ["None", "None", "None"])[:3]
    return vrs[0], vrs[1], vrs[2]


def collect_elements(part06, part07):
    entries = {}
    sources = [(part06, t) for t in ("table_6-1", "table_7-1", "table_8-1", "table_9-1")]
    sources += [(part07, t) for t in ("table_E.1-1", "table_E.2-1")]
    for root, tid in sources:
        for cells in table_rows(root, tid):
            if len(cells) < 5:
                continue
            tag_text, name, keyword, vr_text, vm = cells[:5]
            # PS3.7 Table E.2-1 lists only retired command elements (it has no "RET" column).
            retired = tid == "table_E.2-1" or (len(cells) > 5 and "RET" in cells[5].upper())
            parsed = parse_tag(tag_text)
            if parsed is None or not keyword:
                continue
            value, mask = parsed
            vr, vr_alt, vr_alt2 = parse_vr(vr_text)
            entries[(value, mask)] = (value, mask, vr, vr_alt, keyword, name, vm, retired, vr_alt2)
    return sorted(entries.values(), key=lambda e: (e[0], e[1]))


def collect_uids(part06):
    uids = {}
    for cells in table_rows(part06, "table_A-1"):
        if len(cells) < 4:
            continue
        uid, name, keyword, uid_type = cells[0], cells[1], cells[2], cells[3]
        if not re.fullmatch(r"[0-9.]+", uid):
            continue
        retired = "(Retired)" in name
        uids[uid] = (uid, name, keyword, uid_type, retired)
    return [uids[k] for k in sorted(uids)]


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    part06 = ET.parse(sys.argv[1]).getroot()
    part07 = ET.parse(sys.argv[2]).getroot()
    elements = collect_elements(part06, part07)
    uids = collect_uids(part06)

    plain = [e for e in elements if e[1] == 0xFFFFFFFF]
    masked = [e for e in elements if e[1] != 0xFFFFFFFF]

    out = []
    out.append("// GENERATED FILE - do not edit. Produced by tools/gen_dictionary.py from DICOM PS3.6/PS3.7.")
    out.append(f"// {len(plain)} plain entries, {len(masked)} repeating-group entries, {len(uids)} UIDs.")
    out.append("")

    def entry_line(e):
        value, mask, vr, vr_alt, keyword, name, vm, retired, vr_alt2 = e
        third = "" if vr_alt2 == "None" else f", VR::{vr_alt2}"  # "US or SS or OW"
        return (f"    Entry{{0x{value:08X}u, 0x{mask:08X}u, VR::{vr}, VR::{vr_alt}, {c_str(keyword)}, "
                f"{c_str(name)}, {c_str(vm)}, {'true' if retired else 'false'}{third}}},")

    out.append("constexpr Entry kEntries[] = {")
    out += [entry_line(e) for e in plain]
    out.append("};")
    out.append("")
    out.append("constexpr Entry kMaskedEntries[] = {")
    out += [entry_line(e) for e in masked]
    out.append("};")
    out.append("")
    out.append("constexpr UidEntry kUids[] = {")
    for uid, name, keyword, uid_type, retired in uids:
        out.append(f"    UidEntry{{{c_str(uid)}, {c_str(name)}, {c_str(keyword)}, {c_str(uid_type)}, "
                   f"{'true' if retired else 'false'}}},")
    out.append("};")
    out.append("")

    with open(sys.argv[3], "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print(f"wrote {sys.argv[3]}: {len(plain)} + {len(masked)} elements, {len(uids)} UIDs")


if __name__ == "__main__":
    main()
