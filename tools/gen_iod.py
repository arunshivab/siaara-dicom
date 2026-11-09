#!/usr/bin/env python3
"""Generates src/repair/iod_tables.inc from the DICOM standard's DocBook XML.

Usage:
    python3 -I tools/gen_iod.py <part03.xml> <part04.xml> <output .inc>

Sources (https://dicom.nema.org/medical/dicom/current/source/docbook/):
    part03/part03.xml : Annex A Composite IOD Module tables ("... IOD Modules"), the Module and
                        Macro attribute tables of Annex C / Section 10 (Attribute Name, Tag,
                        Type, Attribute Description), their "Include Table ..." rows, the
                        "Enumerated Values:" lists in the descriptions, and the "Required if"
                        conditions of Type 1C/2C attributes.
    part04/part04.xml : Table B.5-1 Standard SOP Classes (SOP Class UID -> IOD section).

What is extracted, per attribute row: nesting depth ('>' count), tag (with 'x' positions zeroed
and a mask), Type (1, 1C, 2, 2C, 3), an optional Include reference to another table, the
enumerated values (only lists titled exactly "Enumerated Values:", which apply to every value),
and, for 1C/2C, a condition when its text has one of a few simple shapes:
    <Attribute> (gggg,eeee) is present            -> PRESENT
    <Attribute> (gggg,eeee) is not present        -> ABSENT
    <Attribute> (gggg,eeee) has a value of X [or Y ...] / equals X / is X -> EQUALS
    <Attribute> (gggg,eeee) has a value other than X / is not X      -> NOT_EQUALS
    <Attribute> (gggg,eeee) has a value greater than N / is greater than N -> GREATER
plus the phrases listed in PHRASES (each with the PS3.3 section that defines it).
Anything else is recorded as "not evaluated" (op NONE). Only the Python standard library is used.
The output is deterministic (document order, no timestamps).
"""
import re
import sys
import xml.etree.ElementTree as ET

DB = "{http://docbook.org/ns/docbook}"
XML_ID = "{http://www.w3.org/XML/1998/namespace}id"
TYPES = {"1": 1, "1C": 2, "2": 3, "2C": 4, "3": 5}
USAGE = {"M": 1, "U": 2, "C": 3}


def text(node):
    s = "".join(node.itertext())
    s = s.replace("​", "").replace(" ", " ")
    return re.sub(r"\s+", " ", s).strip()


def edition(root):
    sub = root.find(DB + "subtitle")
    m = re.match(r"DICOM (PS3\.\d+ \d{4}[a-z]?)", text(sub) if sub is not None else "")
    if not m:
        raise SystemExit("edition not found")
    return m.group(1)


def c_str(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif 32 <= o < 127:
            out.append(ch)
        else:
            # Keep the generated file ASCII: UTF-8 bytes as octal escapes.
            for b in ch.encode("utf-8"):
                out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def parse_tag(s):
    m = re.fullmatch(r"\(([0-9A-Fa-fx]{4}),([0-9A-Fa-fx]{4})\)", s.strip())
    if not m:
        return None
    hexs = (m.group(1) + m.group(2)).upper()
    tag = 0
    mask = 0
    for ch in hexs:
        tag <<= 4
        mask <<= 4
        if ch in "xX":
            continue
        tag |= int(ch, 16)
        mask |= 0xF
    return tag, mask


class Tables:
    """All tables of part03 indexed by xml:id, and the tables inside each section."""

    def __init__(self, root):
        self.by_id = {}
        self.section_tables = {}
        self.order = []
        self._walk(root, [])

    def _walk(self, node, open_sections):
        for child in node:
            if child.tag == DB + "section":
                sid = child.get(XML_ID)
                if sid:
                    self.section_tables[sid] = []
                self._walk(child, open_sections + ([sid] if sid else []))
            elif child.tag == DB + "table":
                tid = child.get(XML_ID)
                if tid:
                    self.by_id[tid] = child
                    self.order.append(tid)
                    for sid in open_sections:
                        self.section_tables[sid].append(tid)
                # tables do not contain sections; no need to descend
            else:
                self._walk(child, open_sections)


def header(tbl):
    th = tbl.find(DB + "thead")
    if th is None:
        return []
    return [text(c) for c in th.iter(DB + "th")]


def body_rows(tbl):
    tb = tbl.find(DB + "tbody")
    if tb is None:
        return []
    return tb.findall(DB + "tr")


def is_attribute_table(tbl):
    h = header(tbl)
    return len(h) >= 3 and h[0] == "Attribute Name" and h[1] == "Tag" and h[2] == "Type"


def enumerated_values(td):
    out = []
    for vl in td.iter(DB + "variablelist"):
        t = vl.find(DB + "title")
        if t is None or text(t) != "Enumerated Values:":
            continue
        for entry in vl.findall(DB + "varlistentry"):
            term = entry.find(DB + "term")
            if term is None:
                continue
            v = text(term)
            if v and len(v) <= 64 and "\\" not in v:
                out.append(v)
    return out


ATTR_REF = r"[A-Za-z0-9'’ ,/\-\(\)]*?\(([0-9A-Fa-fx]{4}),([0-9A-Fa-fx]{4})\)"
VALUE = r"(?:\"([^\"]+)\"|([A-Z0-9_]+|-?\d+(?:\.\d+)?))"


# Condition phrases that name no attribute but whose meaning PS3.3 defines in terms of one.
# PS3.3 C.17.3 (Document Relationship Macro): a Content Item is included by-reference exactly when
# Referenced Content Item Identifier (0040,DB73) is present, and by-value otherwise.
PHRASES = [
    (r"the Target Content Item is included by-value in the Source Content Item", ("ABSENT", 0x0040DB73, 0xFFFFFFFF, [])),
]


def parse_condition(desc):
    """Returns (op, tag, mask, values) for the simple shapes, or None."""
    for phrase, cond in PHRASES:
        if re.search(r"Required if " + phrase, desc):
            return cond
    m = re.search(r"Required if (.*?)(?:\.(?:\s|$)|;|$)", desc)
    if not m:
        return None
    cond = m.group(1).strip()
    # Strip a trailing ", may be present otherwise" / "otherwise ..." clause.
    cond = re.split(r",? (?:otherwise|may be present otherwise)", cond)[0].strip()
    # Exactly one attribute reference.
    refs = re.findall(r"\(([0-9A-Fa-fx]{4}),([0-9A-Fa-fx]{4})\)", cond)
    if len(refs) != 1:
        return None
    pt = parse_tag("(%s,%s)" % refs[0])
    if pt is None:
        return None
    tag, mask = pt
    tail = cond[cond.index(")", cond.index("(" + refs[0][0])) + 1:].strip()
    # Must start with the attribute name (no leading "the value of ... in the ... Sequence").
    head = cond[: cond.index("(" + refs[0][0])].strip()
    if re.search(r"\b(and|or|in|of the|within|item|Sequence|Item|Macro|Module)\b", head):
        return None
    tail = tail.rstrip(" .")
    if re.fullmatch(r"is present", tail) or re.fullmatch(r"is present and has a value", tail):
        return ("PRESENT", tag, mask, [])
    if re.fullmatch(r"is (?:not present|absent)", tail):
        return ("ABSENT", tag, mask, [])
    mm = re.fullmatch(r"(?:is|has a value) greater than (\d+)", tail)
    if mm:
        return ("GREATER", tag, mask, [mm.group(1)])
    mm = re.fullmatch(r"(?:has a value other than|is not|is other than|does not equal|is not equal to) " + VALUE, tail)
    if mm:
        return ("NOT_EQUALS", tag, mask, [mm.group(1) or mm.group(2)])
    mm = re.fullmatch(r"(?:has a value of|has the value|has the value of|equals|is equal to|is|=|value is) "
                      r"(" + VALUE + r"(?:(?:,| or|, or) " + VALUE + r")*)", tail)
    if mm:
        vals = re.findall(VALUE, mm.group(1))
        values = [a or b for a, b in vals]
        values = [v for v in values if v not in ("or",)]
        if values:
            return ("EQUALS", tag, mask, values)
    return None


def include_target(td):
    for x in td.iter(DB + "xref"):
        le = x.get("linkend", "")
        if le.startswith("table_"):
            return le
    return None


class Gen:
    def __init__(self, p3, p4):
        self.p3 = p3
        self.p4 = p4
        self.tables = Tables(p3)
        self.attr_tables = []      # list of table ids in output order
        self.attr_index = {}       # table id -> index
        self.rows = []             # (depth, tag, mask, type, include_index or -1, cond, enum)
        self.table_rows = {}       # table id -> (first, count)
        self.conditions = []
        self.cond_index = {}
        self.enum_lists = []
        self.enum_index = {}
        self.enum_values = []
        self.iods = []
        self.module_rows = []
        self.sop = []
        self.stats = {"conditional_includes": 0, "unresolved_includes": 0, "unparsed_conditions": 0, "parsed_conditions": 0}

    # ---- attribute tables
    def attr_table(self, tid):
        if tid in self.attr_index:
            return self.attr_index[tid]
        tbl = self.tables.by_id.get(tid)
        if tbl is None or not is_attribute_table(tbl):
            return -1
        idx = len(self.attr_tables)
        self.attr_index[tid] = idx
        self.attr_tables.append(tid)
        return idx

    def emit_rows(self):
        # Attribute tables are resolved transitively; rows are emitted in table index order.
        done = 0
        pending_rows = {}
        while done < len(self.attr_tables):
            tid = self.attr_tables[done]
            pending_rows[tid] = self.table_rows_of(tid)
            done += 1
        for tid in self.attr_tables:
            rows = pending_rows[tid]
            self.table_rows[tid] = (len(self.rows), len(rows))
            self.rows.extend(rows)

    def cond(self, c):
        if c is None:
            return -1
        key = (c[0], c[1], c[2], tuple(c[3]))
        if key not in self.cond_index:
            first = len(self.enum_values)
            self.enum_values.extend(c[3])
            self.cond_index[key] = len(self.conditions)
            self.conditions.append((c[0], c[1], c[2], first, len(c[3])))
        return self.cond_index[key]

    def enum(self, values):
        if not values:
            return -1
        key = tuple(values)
        if key not in self.enum_index:
            first = len(self.enum_values)
            self.enum_values.extend(values)
            self.enum_index[key] = len(self.enum_lists)
            self.enum_lists.append((first, len(values)))
        return self.enum_index[key]

    def table_rows_of(self, tid):
        tbl = self.tables.by_id[tid]
        out = []
        for tr in body_rows(tbl):
            tds = tr.findall(DB + "td")
            if not tds:
                continue
            first = text(tds[0])
            depth = len(first) - len(first.lstrip(">"))
            rest = first.lstrip(">").strip()
            if rest.startswith("Include") and (len(tds) < 2 or parse_tag(text(tds[1])) is None):
                target = include_target(tds[0])
                inc = self.attr_table(target) if target else -1
                if inc < 0:
                    self.stats["unresolved_includes"] += 1
                    continue
                # "Include ... if [and only if] <condition>": a conditional include (type 6).
                m = re.search(r"\b(?:if and only if|only if|if)\b (.*)$", rest)
                if m:
                    pc = parse_condition("Required if " + m.group(1))
                    self.stats["conditional_includes"] += 1
                    out.append((depth, 0, 0, 6, inc, self.cond(pc), -1))
                else:
                    out.append((depth, 0, 0, 0, inc, -1, -1))
                continue
            if len(tds) < 3:
                continue
            pt = parse_tag(text(tds[1]))
            if pt is None:
                continue
            ty = TYPES.get(text(tds[2]))
            if ty is None:
                continue
            desc_td = tds[3] if len(tds) > 3 else None
            desc = text(desc_td) if desc_td is not None else ""
            c = -1
            if ty in (2, 4):
                pc = parse_condition(desc)
                if pc is None:
                    self.stats["unparsed_conditions"] += 1
                else:
                    self.stats["parsed_conditions"] += 1
                c = self.cond(pc)
            e = self.enum(enumerated_values(desc_td)) if desc_td is not None else -1
            out.append((depth, pt[0], pt[1], ty, -1, c, e))
        return out

    # ---- IODs
    def iod_table_for_section(self, sid):
        for tid in self.tables.section_tables.get(sid, []):
            cap = self.tables.by_id[tid].find(DB + "caption")
            if cap is not None and text(cap).endswith("IOD Modules"):
                return tid
        return None

    def module_table_for_section(self, sid):
        for tid in self.tables.section_tables.get(sid, []):
            if is_attribute_table(self.tables.by_id[tid]):
                return tid
        return None

    def parse_iod(self, tid):
        tbl = self.tables.by_id[tid]
        cap = text(tbl.find(DB + "caption"))
        name = cap[: -len(" IOD Modules")]
        h = header(tbl)
        if len(h) < 3 or "Module" not in h or "Usage" not in h[-1]:
            return None
        ncols = len(h)
        mods = []
        ie = ""
        for tr in body_rows(tbl):
            tds = tr.findall(DB + "td")
            cells = [text(t) for t in tds]
            if len(tds) == ncols:
                ie = cells[0]
                tds = tds[1:]
                cells = cells[1:]
            if len(tds) != ncols - 1:
                continue
            ref = None
            for x in tds[1].iter(DB + "xref"):
                ref = x.get("linkend")
                break
            if not ref:
                continue
            mt = None
            if ref.startswith("sect_"):
                mt = self.module_table_for_section(ref)
            elif ref.startswith("table_"):
                mt = ref
            if not mt:
                continue
            ti = self.attr_table(mt)
            if ti < 0:
                continue
            usage = USAGE.get(cells[-1][:1], 2)
            mods.append((ie, cells[0], ti, usage))
        return name, mods

    def run(self):
        iod_by_section = {}
        for sid in self.tables.section_tables:
            if not sid.startswith("sect_A."):
                continue
            tid = self.iod_table_for_section(sid)
            if tid:
                iod_by_section[sid] = tid
        # Every IOD module table, in document order.
        seen = {}
        for tid in self.tables.order:
            cap = self.tables.by_id[tid].find(DB + "caption")
            if cap is None or not text(cap).endswith("IOD Modules"):
                continue
            r = self.parse_iod(tid)
            if r is None or not r[1]:
                continue
            seen[tid] = len(self.iods)
            first = len(self.module_rows)
            self.module_rows.extend(r[1])
            self.iods.append((r[0], tid[len("table_"):], first, len(r[1])))
        # SOP classes (PS3.4 Table B.5-1).
        b51 = None
        for tbl in self.p4.iter(DB + "table"):
            if tbl.get(XML_ID) == "table_B.5-1":
                b51 = tbl
                break
        if b51 is None:
            raise SystemExit("PS3.4 table B.5-1 not found")
        for tr in body_rows(b51):
            tds = tr.findall(DB + "td")
            if len(tds) < 3:
                continue
            uid = text(tds[1])
            if not re.fullmatch(r"[0-9.]+", uid):
                continue
            target = None
            for x in tds[2].iter():
                if x.tag in (DB + "olink", DB + "xref"):
                    target = x.get("targetptr") or x.get("linkend")
                    break
            if not target:
                continue
            # The target section may be the IOD itself or one of its ancestors' children.
            sid = target
            tid = iod_by_section.get(sid)
            if tid is None:
                # Look for a table in the section's subtree.
                tid = self.iod_table_for_section(sid)
            if tid is None or tid not in seen:
                continue
            self.sop.append((uid, text(tds[0]), seen[tid]))
        self.emit_rows()

    def write(self, path):
        e3 = edition(self.p3)
        e4 = edition(self.p4)
        L = []
        L.append("// Generated by tools/gen_iod.py - do not edit.")
        L.append("// Sources: DICOM %s Annex A (Composite IOD Module tables), Annex C and Section 10" % e3)
        L.append("// (Module and Macro Attribute tables); DICOM %s Table B.5-1 (Standard SOP Classes)." % e4)
        L.append("// Included by src/repair/iod.cpp inside namespace siaara::repair::detail.")
        L.append("// %d attribute tables, %d attribute rows, %d IODs, %d IOD module rows, %d SOP classes," % (
            len(self.attr_tables), len(self.rows), len(self.iods), len(self.module_rows), len(self.sop)))
        L.append("// %d conditions (%d 1C/2C rows parsed, %d not evaluated), %d enumerated value lists, %d strings." % (
            len(self.conditions), self.stats["parsed_conditions"], self.stats["unparsed_conditions"],
            len(self.enum_lists), len(self.enum_values)))
        L.append("// %d conditional Include rows (\"Include ... if ...\")." % self.stats["conditional_includes"])
        L.append("// Row fields: depth, tag, mask, type (1=1, 2=1C, 3=2, 4=2C, 5=3; 0 = Include row, 6 = conditional")
        L.append("// Include row whose condition is the row's condition, -1 when not evaluated),")
        L.append("// included table (-1), condition (-1), enumerated value list (-1).")
        L.append("")
        L.append("constexpr AttrRow kAttrRows[] = {")
        for r in self.rows:
            L.append("    {%d, 0x%08Xu, 0x%08Xu, %d, %d, %d, %d}," % r)
        L.append("};")
        L.append("")
        L.append("constexpr AttrTable kAttrTables[] = {")
        for tid in self.attr_tables:
            tbl = self.tables.by_id[tid]
            cap = text(tbl.find(DB + "caption")) if tbl.find(DB + "caption") is not None else ""
            first, count = self.table_rows[tid]
            L.append("    {%s, %s, %du, %du}," % (c_str(tid[len("table_"):]), c_str(cap), first, count))
        L.append("};")
        L.append("")
        L.append("constexpr ModuleRow kModuleRows[] = {")
        for ie, name, ti, usage in self.module_rows:
            L.append("    {%s, %s, %d, %d}," % (c_str(ie), c_str(name), ti, usage))
        L.append("};")
        L.append("")
        L.append("constexpr IodDef kIods[] = {")
        for name, tid, first, count in self.iods:
            L.append("    {%s, %s, %du, %du}," % (c_str(name), c_str(tid), first, count))
        L.append("};")
        L.append("")
        L.append("constexpr SopIod kSopClasses[] = {")
        for uid, name, iod in sorted(self.sop, key=lambda s: s[0]):
            L.append("    {%s, %s, %d}," % (c_str(uid), c_str(name), iod))
        L.append("};")
        L.append("")
        L.append("constexpr Condition kConditions[] = {")
        if not self.conditions:
            L.append("    {CondOp::None, 0u, 0u, 0u, 0u},")
        for op, tag, mask, first, count in self.conditions:
            opname = {"PRESENT": "Present", "ABSENT": "Absent", "EQUALS": "Equals",
                      "NOT_EQUALS": "NotEquals", "GREATER": "Greater"}[op]
            L.append("    {CondOp::%s, 0x%08Xu, 0x%08Xu, %du, %du}," % (opname, tag, mask, first, count))
        L.append("};")
        L.append("")
        L.append("constexpr EnumList kEnumLists[] = {")
        for first, count in self.enum_lists:
            L.append("    {%du, %du}," % (first, count))
        L.append("};")
        L.append("")
        L.append("constexpr std::string_view kStrings[] = {")
        for v in self.enum_values:
            L.append("    %s," % c_str(v))
        L.append("};")
        with open(path, "w", encoding="ascii", newline="\n") as f:
            f.write("\n".join(L) + "\n")
        return self.stats


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    p3 = ET.parse(sys.argv[1]).getroot()
    p4 = ET.parse(sys.argv[2]).getroot()
    g = Gen(p3, p4)
    g.run()
    st = g.write(sys.argv[3])
    print("attribute tables %d, rows %d, IODs %d, module rows %d, SOP classes %d, conditions %d, "
          "enum lists %d, conditional includes %d, unresolved includes %d, 1C/2C parsed %d, not evaluated %d" % (
              len(g.attr_tables), len(g.rows), len(g.iods), len(g.module_rows), len(g.sop),
              len(g.conditions), len(g.enum_lists), st["conditional_includes"], st["unresolved_includes"],
              st["parsed_conditions"], st["unparsed_conditions"]))


if __name__ == "__main__":
    main()
