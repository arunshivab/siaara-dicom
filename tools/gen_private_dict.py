#!/usr/bin/env python3
"""Generates src/dict/private_dictionary_data.inc from data/private-tags/*.tsv.

Usage:
    python3 tools/gen_private_dict.py [--check] [--stats] [--data DIR] [--out FILE]

The only input is the owner's vendor DICOM Conformance Statement extraction in
data/private-tags/ (see data/private-tags/README.md and summary.md). No other toolkit's private
dictionary is used, as a source or as a check.

Rules (owner decisions; docs/PRIVATE_DICTIONARY.md explains them):
  * Rows with no creator stated are not in the lookup (they stay in the source data only).
  * A key is creator + group + element_xx (the low byte of the element within the creator's block).
    Rows whose creator was "assumed from preceding group" are LOWER CONFIDENCE: a key uses them only
    when no row of that key has a stated creator.
  * VR: decided when every chosen row that states a VR states the same one. Otherwise the VR is
    undecided (implicit-VR reads keep the value as UN bytes) and every candidate VR is listed with
    its documents ("conflict"). A document that prints two VRs ("OW/OB") gives two candidates.
  * VM: from the rows that agree with the decided VR (all rows when the VR is undecided); decided
    when they agree ignoring case and spaces, written in the most common printed spelling.
  * Name: the most common description; keyword: the most common keyword_suggestion of the rows
    with that description. Retired only where a document says so.
  * Creator spellings: the lookup tries the exact string first, then a case- and whitespace-
    insensitive fold (ASCII upper case, ASCII whitespace removed); the fold table is generated here.

Only the Python standard library is used. The output is deterministic, so CI can check that the
committed file matches the generator (--check, ctest private_dict_up_to_date).
"""
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data", "private-tags")
OUT = os.path.join(ROOT, "src", "dict", "private_dictionary_data.inc")
VENDOR_FILES = ["siemens.tsv", "ge.tsv", "philips.tsv", "canon_toshiba.tsv", "varian.tsv", "elekta.tsv"]
COLUMNS = ["creator", "group", "element_xx", "keyword_suggestion", "description", "vr", "vm", "retired",
           "conflict_flag", "vendor", "product", "software_version", "document", "page", "notes"]
KNOWN_VRS = {
    "AE", "AS", "AT", "CS", "DA", "DS", "DT", "FL", "FD", "IS", "LO", "LT", "OB", "OD", "OF",
    "OL", "OV", "OW", "PN", "SH", "SL", "SQ", "SS", "ST", "SV", "TM", "UC", "UI", "UL", "UN",
    "UR", "US", "UT", "UV",
}
POOL_ROW = 8192  # bytes per string-pool row; a string never crosses a row (MSVC literal limits)

# Flag bits of PrivEntry::flags (keep in step with src/dict/private_dictionary.cpp).
F_ASSUMED = 1      # creator assumed from a nearby declaration (lower confidence)
F_VR_CONFLICT = 2  # documents disagree on the VR: VR undecided
F_VM_CONFLICT = 4  # documents disagree on the VM: VM undecided
F_RETIRED = 8


def fold(s):
    """Case- and whitespace-insensitive form of a creator string."""
    return "".join(ch.upper() if "a" <= ch <= "z" else ch for ch in s if ch not in " \t\n\v\f\r")


def read_rows(data_dir):
    rows = []
    for name in VENDOR_FILES:
        path = os.path.join(data_dir, name)
        with open(path, encoding="utf-8", newline="") as fh:
            lines = fh.read().split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        header = lines[0].split("\t")
        if header != COLUMNS:
            raise SystemExit(f"{name}: unexpected columns {header}")
        for n, line in enumerate(lines[1:], start=2):
            cells = line.split("\t")
            if len(cells) != len(COLUMNS):
                raise SystemExit(f"{name}:{n}: {len(cells)} columns")
            r = dict(zip(COLUMNS, cells))
            if not re.fullmatch(r"[0-9A-F]{4}", r["group"]) or not re.fullmatch(r"[0-9A-F]{2}", r["element_xx"]):
                raise SystemExit(f"{name}:{n}: bad group/element {r['group']},{r['element_xx']}")
            if not r["page"].isdigit():
                raise SystemExit(f"{name}:{n}: bad page {r['page']!r}")
            rows.append(r)
    return rows


def vr_tokens(vr):
    """VRs a row states: [] when blank, two for "OW/OB"."""
    if not vr:
        return []
    toks = vr.split("/")
    for t in toks:
        if t not in KNOWN_VRS:
            raise SystemExit(f"unknown VR {vr!r}")
    return toks


def most_common(values):
    """Most common non-blank value; ties go to the smallest string. None when all blank."""
    c = collections.Counter(v for v in values if v)
    if not c:
        return None
    return min(c.items(), key=lambda kv: (-kv[1], kv[0]))[0]


def vm_key(vm):
    return "".join(vm.split()).lower()


def build(rows):
    keys = collections.defaultdict(list)
    blank_creator = 0
    for r in rows:
        if not r["creator"]:
            blank_creator += 1
            continue
        keys[(r["creator"], int(r["group"], 16), int(r["element_xx"], 16))].append(r)

    entries = []
    for key in sorted(keys):
        allrows = keys[key]
        stated = [r for r in allrows if "creator assumed" not in r["notes"]]
        assumed = not stated
        chosen = stated if stated else allrows
        cand = collections.defaultdict(dict)  # vr -> {document: lowest page}
        for r in chosen:
            for t in vr_tokens(r["vr"]):
                d = cand[t]
                p = int(r["page"])
                if r["document"] not in d or p < d[r["document"]]:
                    d[r["document"]] = p
        vr = next(iter(cand)) if len(cand) == 1 else None
        vr_conflict = len(cand) > 1
        agree = [r for r in chosen if vr is None or r["vr"] in ("", vr)]
        vm_groups = collections.defaultdict(list)
        for r in agree:
            if r["vm"]:
                vm_groups[vm_key(r["vm"])].append(r["vm"])
        vm = most_common(next(iter(vm_groups.values()))) if len(vm_groups) == 1 else None
        vm_conflict = len(vm_groups) > 1
        name = most_common(r["description"] for r in chosen) or ""
        kw_rows = [r for r in chosen if r["description"] == name] if name else chosen
        keyword = most_common(r["keyword_suggestion"] for r in kw_rows) or ""
        retired = any(r["retired"] == "yes" for r in chosen)
        vendor = most_common(r["vendor"] for r in chosen)
        prim = [r for r in chosen if (vr is None or vr in vr_tokens(r["vr"])) and (not name or r["description"] == name)]
        if not prim:
            prim = chosen
        p = min(prim, key=lambda r: (r["document"], int(r["page"])))
        docs = {r["document"] for r in chosen}
        candidates = []
        if vr_conflict:
            for t in sorted(cand):
                for d in sorted(cand[t]):
                    candidates.append((t, d, cand[t][d]))
        entries.append({
            "creator": key[0], "group": key[1], "el": key[2], "vr": vr, "vm": vm or "",
            "name": name, "keyword": keyword, "assumed": assumed, "vr_conflict": vr_conflict,
            "vm_conflict": vm_conflict, "retired": retired, "vendor": vendor,
            "doc": p["document"], "page": int(p["page"]), "sources": len(docs),
            "candidates": candidates,
        })
    return entries, blank_creator


class Pool:
    """NUL-terminated strings packed into fixed rows; offset = row * POOL_ROW + column."""

    def __init__(self):
        self.rows = [bytearray()]
        self.index = {}

    def add(self, s):
        if s in self.index:
            return self.index[s]
        b = s.encode("utf-8") + b"\0"
        if len(b) >= POOL_ROW:
            raise SystemExit(f"string too long for the pool: {s[:40]!r}")
        if len(self.rows[-1]) + len(b) >= POOL_ROW:  # room for the literal's own NUL
            self.rows.append(bytearray())
        off = (len(self.rows) - 1) * POOL_ROW + len(self.rows[-1])
        self.rows[-1] += b
        self.index[s] = off
        return off

    def size(self):
        return sum(len(r) for r in self.rows)


def c_bytes(b):
    """C string literal body for bytes (no terminating NUL): octal escapes, never hex (greedy)."""
    out = []
    for ch in b:
        if ch == 0x5C:
            out.append("\\\\")
        elif ch == 0x22:
            out.append('\\"')
        elif ch == 0x3F:
            out.append("\\?")  # no trigraphs in older modes
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)
    return "".join(out)


def decode_literals(lines):
    """Decodes the C string literals of one pool row as a C++ compiler does (self-check)."""
    out = bytearray()
    for line in lines:
        body = line.strip()
        assert body.startswith('"') and body.endswith('"'), body
        body = body[1:-1]
        i = 0
        while i < len(body):
            ch = body[i]
            if ch == '"':  # end of one literal and start of the next adjacent one: '" "'
                j = body.index('"', i + 1)
                assert body[i + 1:j].strip() == "", body
                i = j + 1
                continue
            if ch != "\\":
                out += ch.encode("ascii")
                i += 1
                continue
            nx = body[i + 1]
            if nx in "01234567":
                j = i + 1
                while j < len(body) and j < i + 4 and body[j] in "01234567":
                    j += 1
                out.append(int(body[i + 1:j], 8))
                i = j
            else:
                out += {"\\": b"\\", '"': b'"', "?": b"?"}[nx]
                i += 2
    return bytes(out)


def generate(data_dir):
    rows = read_rows(data_dir)
    entries, blank_creator = build(rows)

    creators = sorted({e["creator"] for e in entries}, key=lambda s: s.encode("utf-8"))
    cindex = {c: i for i, c in enumerate(creators)}
    entries.sort(key=lambda e: (cindex[e["creator"]], e["group"], e["el"]))
    vendors = sorted({e["vendor"] for e in entries})
    vms = sorted({e["vm"] for e in entries})
    docs = sorted({e["doc"] for e in entries} | {c[1] for e in entries for c in e["candidates"]},
                  key=lambda s: s.encode("utf-8"))
    if len(creators) > 0xFFFF or len(docs) > 0xFFFF or len(vms) > 0xFF or len(vendors) > 0xFF:
        raise SystemExit("table too large for the index widths")

    pool = Pool()
    pool.add("")  # offset 0 is the empty string
    creator_off = [pool.add(c) for c in creators]
    fold_off = [pool.add(fold(c)) for c in creators]
    vendor_off = [pool.add(v) for v in vendors]
    vm_off = [pool.add(v) for v in vms]
    doc_off = [pool.add(d) for d in docs]
    dindex = {d: i for i, d in enumerate(docs)}
    vindex = {v: i for i, v in enumerate(vendors)}
    vmindex = {v: i for i, v in enumerate(vms)}

    first = {}
    count = collections.Counter()
    for i, e in enumerate(entries):
        first.setdefault(cindex[e["creator"]], i)
        count[cindex[e["creator"]]] += 1

    cand_rows = []
    lines_e = []
    for e in entries:
        flags = (F_ASSUMED if e["assumed"] else 0) | (F_VR_CONFLICT if e["vr_conflict"] else 0) | \
                (F_VM_CONFLICT if e["vm_conflict"] else 0) | (F_RETIRED if e["retired"] else 0)
        cf = len(cand_rows)
        for vr, d, pg in e["candidates"]:
            cand_rows.append((vr, dindex[d], pg))
        nc = len(cand_rows) - cf
        if cf > 0xFFFF or nc > 0xFFFF or e["page"] > 0xFFFF or e["sources"] > 0xFFFF:
            raise SystemExit("candidate/page/source count too large")
        lines_e.append("    PrivEntry{%du, %du, %du, 0x%04Xu, VR::%s, %du, %du, %du, %du, %du, 0x%02Xu, %du, %du, %du},"
                       % (pool.add(e["name"]), pool.add(e["keyword"]), cindex[e["creator"]], e["group"],
                          e["vr"] or "None", dindex[e["doc"]], e["page"], e["sources"], cf, nc, e["el"],
                          vmindex[e["vm"]], flags, vindex[e["vendor"]]))

    fold_order = sorted(range(len(creators)), key=lambda i: (fold(creators[i]).encode("utf-8"), creators[i].encode("utf-8")))
    folds = collections.Counter(fold(c) for c in creators)
    alias_groups = sum(1 for v in folds.values() if v > 1)
    alias_creators = sum(v for v in folds.values() if v > 1)

    n_assumed = sum(1 for e in entries if e["assumed"])
    stats = {
        "rows": len(rows), "blank_creator_rows": blank_creator, "creators": len(creators),
        "entries": len(entries), "stated": len(entries) - n_assumed, "assumed": n_assumed,
        "vr_conflicts": sum(1 for e in entries if e["vr_conflict"]),
        "vm_undecided_conflict": sum(1 for e in entries if e["vm_conflict"]),
        "vr_not_stated": sum(1 for e in entries if e["vr"] is None and not e["vr_conflict"]),
        "fold_alias_groups": alias_groups, "fold_alias_creators": alias_creators,
        "candidates": len(cand_rows), "documents": len(docs), "pool_bytes": pool.size(),
        "retired": sum(1 for e in entries if e["retired"]),
    }

    o = []
    o.append("// GENERATED FILE - do not edit. Produced by tools/gen_private_dict.py from data/private-tags/*.tsv")
    o.append("// (vendor DICOM Conformance Statements supplied by the project owner; see docs/PRIVATE_DICTIONARY.md).")
    o.append("// %(entries)d entries (%(stated)d stated, %(assumed)d assumed creator), %(creators)d creators, "
             "%(vr_conflicts)d VR conflicts, %(fold_alias_groups)d spelling-variant groups." % stats)
    o.append("// %(rows)d source rows; %(blank_creator_rows)d rows with no creator stated are not in the lookup." % stats)
    o.append("")
    o.append("constexpr std::uint32_t kPrivPoolRow = %du;" % POOL_ROW)
    o.append("constexpr char kPrivPool[][kPrivPoolRow] = {")
    for r in pool.rows:
        body = bytes(r)
        # One literal per row, split into source lines at string ends (adjacent literals concatenate).
        parts = body.split(b"\0")[:-1]
        o.append("    {")
        chunk = []
        row_lines = []
        for i, pbytes in enumerate(parts):
            chunk.append(c_bytes(pbytes) + "\\000")  # 3 octal digits: a following digit is not absorbed
            if len(chunk) == 8 or i == len(parts) - 1:
                # MSVC C4125 (an error under /WX) fires when a decimal digit follows an
                # octal escape, although the meaning is right; split the literal there.
                line = re.sub(r'(\\[0-7]{3})(?=[0-9])', r'\1" "', "".join(chunk))
                row_lines.append('        "' + line + '"')
                chunk = []
        if decode_literals(row_lines) != body:
            raise SystemExit("internal error: pool literal does not decode to the pool bytes")
        o.extend(row_lines)
        o.append("    },")
    o.append("};")
    o.append("")
    o.append("constexpr std::uint32_t kPrivVendors[] = {" + ", ".join("%du" % v for v in vendor_off) + "};")
    o.append("constexpr std::uint32_t kPrivVms[] = {" + ", ".join("%du" % v for v in vm_off) + "};")
    o.append("constexpr std::uint32_t kPrivDocs[] = {")
    for i in range(0, len(doc_off), 12):
        o.append("    " + ", ".join("%du" % v for v in doc_off[i:i + 12]) + ",")
    o.append("};")
    o.append("")
    o.append("// Sorted by creator bytes: {creator, fold, first entry, entry count}.")
    o.append("constexpr PrivCreator kPrivCreators[] = {")
    for i in range(len(creators)):
        o.append("    PrivCreator{%du, %du, %du, %du}," % (creator_off[i], fold_off[i], first[i], count[i]))
    o.append("};")
    o.append("// Creator indices sorted by (fold, creator bytes).")
    o.append("constexpr std::uint16_t kPrivFoldOrder[] = {")
    for i in range(0, len(fold_order), 16):
        o.append("    " + ", ".join("%du" % v for v in fold_order[i:i + 16]) + ",")
    o.append("};")
    o.append("")
    o.append("// Sorted by (creator index, group, element low byte):")
    o.append("// {name, keyword, creator, group, VR, document, page, sources, first candidate, candidates, element, VM, flags, vendor}.")
    o.append("constexpr PrivEntry kPrivEntries[] = {")
    o.extend(lines_e)
    o.append("};")
    o.append("")
    o.append("// VR candidates of the VR-conflict entries: {VR, document, page}.")
    o.append("constexpr PrivCandidate kPrivCandidates[] = {")
    for vr, d, pg in cand_rows:
        o.append("    PrivCandidate{VR::%s, %du, %du}," % (vr, d, pg))
    o.append("};")
    return "\n".join(o) + "\n", stats


def main(argv):
    check = "--check" in argv
    data_dir, out = DATA, OUT
    if "--data" in argv:
        data_dir = argv[argv.index("--data") + 1]
    if "--out" in argv:
        out = argv[argv.index("--out") + 1]
    text, stats = generate(data_dir)
    if "--stats" in argv:
        for k, v in stats.items():
            print(f"{k}: {v}")
    if check:
        try:
            with open(out, encoding="utf-8", newline="") as fh:
                cur = fh.read()
        except OSError:
            cur = None
        if cur != text:
            print(f"{out} is out of date: run python3 tools/gen_private_dict.py", file=sys.stderr)
            return 1
        return 0
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
