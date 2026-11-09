#!/usr/bin/env python3
"""Generates src/charset/charset_tables.inc (character set conversion tables).

Usage:
    python3 tools/gen_charsets.py --download <cache dir> <output .inc>
    python3 tools/gen_charsets.py --input <dir with the files below> <output .inc>

With --download the files are fetched into <cache dir> (existing files are reused). Only the
Python standard library is used and the output is deterministic, so CI can check that the
committed file matches the generator.

Mapping data comes only from these authoritative public files:

  Unicode Consortium, https://www.unicode.org/Public/MAPPINGS/
    ISO8859/8859-{1,2,3,4,5,6,7,8,9,11,15}.TXT    single-byte sets (DICOM ISO_IR 100 ... 203, 166)
    OBSOLETE/EASTASIA/JIS/JIS0201.TXT              JIS X 0201 Romaji and Katakana (ISO_IR 13/14)
    OBSOLETE/EASTASIA/JIS/JIS0208.TXT              JIS X 0208 (ISO 2022 IR 87), JIS column
    OBSOLETE/EASTASIA/JIS/JIS0212.TXT              JIS X 0212 (ISO 2022 IR 159)
    OBSOLETE/EASTASIA/KSC/KSC5601.TXT              KS X 1001 (ISO 2022 IR 149): the entries of
                                                   this Unified Hangul table whose two bytes are
                                                   both in 0xA1..0xFE are exactly KS X 1001 (EUC)
  WHATWG Encoding Standard, https://encoding.spec.whatwg.org/
    index-gb18030.txt, index-gb18030-ranges.txt    GB18030 and GBK; also GB 2312 (ISO 2022 IR 58)

GB 2312: the Unicode file OBSOLETE/EASTASIA/GB/GB2312.TXT is no longer published (HTTP 404), so
the GB 2312 table is the GB 2312 region of the WHATWG GB18030 index (EUC-CN bytes = GB18030 two
byte codes). Only the cells that GB 2312-80 defines are kept; that cell layout (which rows and
cells of the 94x94 plane are assigned) is GB2312_LAYOUT below and yields the standard's 7445
characters (682 symbols + 6763 hanzi), which the generator checks.

Two documented corrections, both from the Unicode file set's own notes (OBSOLETE/EASTASIA/
ReadMe.txt lists them as mapping-file issues where the full-width FFxx character is the intended
mapping): JIS X 0208 0x2140 -> U+FF3C (file: U+005C) and JIS X 0212 0x2237 -> U+FF5E (file:
U+007E). A double-byte character must never decode to an ASCII character, because U+005C is the
DICOM value delimiter; the generator asserts that no double-byte cell maps below U+0080.

The asymmetric GB18030 encoder rows (18 PUA code points) are copied from the gb18030 encoder
algorithm of the WHATWG Encoding Standard, section 10.2.2.
"""
import hashlib
import os
import sys
import urllib.request

UNICODE = "https://www.unicode.org/Public/MAPPINGS/"
WHATWG = "https://encoding.spec.whatwg.org/"

SINGLE_BYTE = [  # (C identifier, file)
    ("k8859_1", "8859-1.TXT"), ("k8859_2", "8859-2.TXT"), ("k8859_3", "8859-3.TXT"),
    ("k8859_4", "8859-4.TXT"), ("k8859_5", "8859-5.TXT"), ("k8859_6", "8859-6.TXT"),
    ("k8859_7", "8859-7.TXT"), ("k8859_8", "8859-8.TXT"), ("k8859_9", "8859-9.TXT"),
    ("k8859_11", "8859-11.TXT"), ("k8859_15", "8859-15.TXT"),
]

FILES = {f: UNICODE + "ISO8859/" + f for _, f in SINGLE_BYTE}
FILES.update({
    "JIS0201.TXT": UNICODE + "OBSOLETE/EASTASIA/JIS/JIS0201.TXT",
    "JIS0208.TXT": UNICODE + "OBSOLETE/EASTASIA/JIS/JIS0208.TXT",
    "JIS0212.TXT": UNICODE + "OBSOLETE/EASTASIA/JIS/JIS0212.TXT",
    "KSC5601.TXT": UNICODE + "OBSOLETE/EASTASIA/KSC/KSC5601.TXT",
    "index-gb18030.txt": WHATWG + "index-gb18030.txt",
    "index-gb18030-ranges.txt": WHATWG + "index-gb18030-ranges.txt",
})

# GB 2312-80 assigned cells: row -> list of inclusive (first cell, last cell) ranges (1-based).
GB2312_LAYOUT = {
    1: [(1, 94)], 2: [(17, 66), (69, 78), (81, 92)], 3: [(1, 94)], 4: [(1, 83)], 5: [(1, 86)],
    6: [(1, 24), (33, 56)], 7: [(1, 33), (49, 81)], 8: [(1, 26), (37, 73)], 9: [(4, 79)],
}
for _r in range(16, 88):
    GB2312_LAYOUT[_r] = [(1, 89)] if _r == 55 else [(1, 94)]

CORRECTIONS = {"JIS0208": {0x2140: 0xFF3C}, "JIS0212": {0x2237: 0xFF5E}}

# WHATWG Encoding Standard 10.2.2, gb18030 encoder: code point -> two bytes.
GB18030_ENCODE_EXTRA = [
    (0xE78D, 0xA6D9), (0xE78E, 0xA6DA), (0xE78F, 0xA6DB), (0xE790, 0xA6DC), (0xE791, 0xA6DD),
    (0xE792, 0xA6DE), (0xE793, 0xA6DF), (0xE794, 0xA6EC), (0xE795, 0xA6ED), (0xE796, 0xA6F3),
    (0xE81E, 0xFE59), (0xE826, 0xFE61), (0xE82B, 0xFE66), (0xE82C, 0xFE67), (0xE832, 0xFE6D),
    (0xE843, 0xFE7E), (0xE854, 0xFE90), (0xE864, 0xFEA0),
]


def fail(msg):
    sys.exit("gen_charsets: " + msg)


def fetch(cache, name):
    path = os.path.join(cache, name)
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        with urllib.request.urlopen(FILES[name], timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    return path


def read_pairs(path, key_col, val_col):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = line.split()
            if not cols[0].startswith("0x"):  # KSC5601.TXT ends with a DOS EOF byte (0x1A)
                continue
            out.append((int(cols[key_col], 16), int(cols[val_col], 16)))
    return out


def read_whatwg(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            cols = line.split()
            out.append((int(cols[0]), int(cols[1], 16)))
    return out


def array(name, ctype, values, per_line=16, fmt="0x{:04X}"):
    lines = [f"constexpr {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def reverse_index(table):
    """Cell indices of mapped cells, sorted by (code point, cell) for binary search."""
    return sorted((i for i, v in enumerate(table) if v), key=lambda i: (table[i], i))


def double_byte(pairs, label):
    t = [0] * (94 * 94)
    fix = CORRECTIONS.get(label, {})
    for code, uni in pairs:
        r, c = (code >> 8) - 0x21, (code & 0xFF) - 0x21
        if not (0 <= r < 94 and 0 <= c < 94):
            fail(f"{label}: code {code:#x} outside 94x94")
        uni = fix.get(code, uni)
        if uni < 0x80 or uni > 0xFFFF:
            fail(f"{label}: code {code:#x} maps to {uni:#x}")
        if t[r * 94 + c]:
            fail(f"{label}: duplicate {code:#x}")
        t[r * 94 + c] = uni
    return t


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("--download", "--input"):
        fail("usage: gen_charsets.py --download <cache dir> | --input <dir>  <output .inc>")
    src_dir, out_path = sys.argv[2], sys.argv[3]
    paths = {}
    for name in sorted(FILES):
        paths[name] = fetch(src_dir, name) if sys.argv[1] == "--download" else os.path.join(src_dir, name)
        if not os.path.exists(paths[name]):
            fail("missing input " + paths[name])

    out = ["// Generated by tools/gen_charsets.py - do not edit.",
           "// Mapping data (sha256 of each input file):"]
    for name in sorted(FILES):
        with open(paths[name], "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        out.append(f"//   {FILES[name]}")
        out.append(f"//     {digest}")
    out.append("// Included by src/charset/charset.cpp only (internal linkage).")
    out.append("")
    total = 0

    # ---- single-byte sets: G1 96-set, bytes 0xA0..0xFF (0 = unassigned)
    for ident, fname in SINGLE_BYTE:
        m = dict(read_pairs(paths[fname], 0, 1))
        for b in range(0xA0):
            if m.get(b) != b:
                fail(f"{fname}: byte {b:#x} is not identity")
        hi = [m.get(b, 0) for b in range(0xA0, 0x100)]
        out.append(array(ident, "std::uint16_t", hi))
        total += 2 * len(hi)

    # ---- JIS X 0201: Romaji (G0, ISO-IR 14) and Katakana (G1, ISO-IR 13), 94-sets 0x21..0x7E
    m = dict(read_pairs(paths["JIS0201.TXT"], 0, 1))
    roman = [m.get(b, 0) for b in range(0x21, 0x7F)]
    kana = [m.get(b | 0x80, 0) for b in range(0x21, 0x7F)]
    if sum(1 for v in kana if v) != 63 or kana[0] != 0xFF61:
        fail("JIS0201: unexpected katakana range")
    out.append(array("kJisRoman", "std::uint16_t", roman))
    out.append(array("kJisKatakana", "std::uint16_t", kana))
    total += 4 * 94

    # ---- 94x94 double-byte sets
    db = {
        "JIS0208": double_byte(read_pairs(paths["JIS0208.TXT"], 1, 2), "JIS0208"),
        "JIS0212": double_byte(read_pairs(paths["JIS0212.TXT"], 0, 1), "JIS0212"),
    }
    ks = [((c - 0x8080), u) for c, u in read_pairs(paths["KSC5601.TXT"], 0, 1)
          if c > 0xFF and 0xA1 <= (c >> 8) <= 0xFE and 0xA1 <= (c & 0xFF) <= 0xFE]
    db["KSX1001"] = double_byte(ks, "KSX1001")

    gb_index = read_whatwg(paths["index-gb18030.txt"])
    gb = [0] * (126 * 190)
    for p, u in gb_index:
        if not 0 <= p < len(gb) or u > 0xFFFF or u < 0x80:
            fail(f"index-gb18030: bad entry {p} {u:#x}")
        gb[p] = u
    if any(v == 0 for v in gb):
        fail("index-gb18030: missing pointers")
    g2312 = []
    for row, ranges in sorted(GB2312_LAYOUT.items()):
        for a, b in ranges:
            for cell in range(a, b + 1):
                ptr = (row + 0xA0 - 0x81) * 190 + (cell + 0xA0 - 0x41)
                g2312.append(((row + 0x20) << 8 | (cell + 0x20), gb[ptr]))
    if len(g2312) != 7445:
        fail("GB 2312 layout does not give 7445 characters")
    db["GB2312"] = double_byte(g2312, "GB2312")

    for label in ("JIS0208", "JIS0212", "KSX1001", "GB2312"):
        t = db[label]
        rev = reverse_index(t)
        out.append(array("k" + label, "std::uint16_t", t))
        out.append(array("k" + label + "Rev", "std::uint16_t", rev))
        total += 2 * (len(t) + len(rev))
        print(f"{label}: {len(rev)} characters", file=sys.stderr)

    # ---- GB18030 / GBK two-byte index and four-byte ranges
    out.append(array("kGb18030Two", "std::uint16_t", gb))
    gb_rev = sorted(range(len(gb)), key=lambda i: (gb[i], i))
    out.append(array("kGb18030TwoRev", "std::uint16_t", gb_rev))
    total += 2 * (len(gb) + len(gb_rev))
    ranges = read_whatwg(paths["index-gb18030-ranges.txt"])
    if ranges != sorted(ranges) or sorted(c for _, c in ranges) != [c for _, c in ranges]:
        fail("index-gb18030-ranges: not monotonic")
    out.append(array("kGb18030RangePtr", "std::uint32_t", [p for p, _ in ranges], 8, "{}"))
    out.append(array("kGb18030RangeCp", "std::uint32_t", [c for _, c in ranges], 8, "0x{:05X}"))
    total += 8 * len(ranges)
    out.append(array("kGb18030EncodeExtraCp", "std::uint16_t", [c for c, _ in GB18030_ENCODE_EXTRA], 9))
    out.append(array("kGb18030EncodeExtraBytes", "std::uint16_t", [b for _, b in GB18030_ENCODE_EXTRA], 9))
    total += 4 * len(GB18030_ENCODE_EXTRA)
    out.append("")
    out.append(f"// Total table data: {total} bytes.")
    out.append(f"constexpr std::size_t kTableBytes = {total};")
    with open(out_path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {out_path}: {total} bytes of table data", file=sys.stderr)


if __name__ == "__main__":
    main()
