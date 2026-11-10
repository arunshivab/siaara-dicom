#!/usr/bin/env python3
"""Generates src/repair/iod_tables.inc from the DICOM standard's DocBook XML.

Usage:
    python3 -I tools/gen_iod.py <part03.xml> <part04.xml> <part16.xml> <output .inc>

Sources (https://dicom.nema.org/medical/dicom/current/source/docbook/):
    part03/part03.xml : Annex A Composite IOD Module tables ("... IOD Modules"), the Module and
                        Macro attribute tables of Annex C / Section 10 (Attribute Name, Tag,
                        Type, Attribute Description), their "Include Table ..." rows, the
                        "Enumerated Values:" lists in the descriptions, and the "Required if"
                        conditions of Type 1C/2C attributes.
    part04/part04.xml : Table B.5-1 Standard SOP Classes (SOP Class UID -> IOD section).
    part16/part16.xml : Tables L-1 and L-5 (Body Part Examined terms of paired structures), for
                        the Laterality condition.
    src/dict/dictionary_data.inc (PS3.6, already generated): attribute names, for conditions that
                        name an attribute without its tag.

What is extracted, per attribute row: nesting depth ('>' count), tag (with 'x' positions zeroed
and a mask), Type (1, 1C, 2, 2C, 3), an optional Include reference to another table, the
enumerated values (only lists titled exactly "Enumerated Values:", which apply to every value),
whether a 1C/2C attribute "may be present otherwise", and, for 1C/2C, a condition when its text
is machine-checkable: terms joined by only "and" or only "or", each term one of
    <Attribute> (gggg,eeee) is present                                 -> PRESENT
    ... has a value / is present and has a value                       -> HAS_VALUE
    ... is not present / is absent / are not present                   -> ABSENT
    ... [Value N] has a value of X [, Y or Z] / equals X / is X         -> EQUALS
    ... has a value other than X / is not X / equals other than X       -> NOT_EQUALS
    ... has a value greater than N / is non-zero                        -> GREATER
    ... is absent, or is present and has the Value X                    -> ABSENT_OR_EQUALS
    Frame Increment Pointer (0028,0009) includes the Tag for A (gggg,eeee) / points to A -> INCLUDES_TAG
("A or B is greater than 1" applies the predicate to both; an attribute named without its tag is
looked up in the dictionary), plus the phrases of PHRASES and REWRITES and the rules of
STRUCTURED (each with the PS3.3 / PS3.16 text it implements). Anything else is recorded as "not
evaluated". docs/VALIDATION.md describes the result. Only the Python standard library is used.
The output is deterministic (document order, no timestamps).
"""
import os
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


REF = re.compile(r"\(([0-9A-Fa-fx]{4}),([0-9A-Fa-fx]{4})\)")
# One value of a condition: a quoted string (a UID may be followed by its name in parentheses),
# an upper-case term (several words allowed: "PALETTE COLOR"), or a number.
ONE_VALUE = r"(?:\"[^\"]+\"|[A-Z0-9_]+(?: [A-Z0-9_]+)*|-?\d+(?:\.\d+)?)(?: ?\([^()]*\))?"
VALUES = ONE_VALUE + r"(?:(?:,| or|, or) " + ONE_VALUE + r")*"


# Condition phrases that name no attribute but whose meaning PS3.3 defines in terms of one.
#  - PS3.3 C.17.3 (Document Relationship Macro): a Content Item is included by-reference exactly
#    when Referenced Content Item Identifier (0040,DB73) is present, and by-value otherwise.
#  - PS3.3 C.7.6.1.1.5: Lossy Image Compression (0028,2110) "01" means the image has undergone
#    lossy compression.
PHRASES = [
    (r"the Target Content Item is included by-value in the Source Content Item",
     (False, [("ABSENT", 0x0040DB73, 0xFFFFFFFF, 0, 0, [])])),
    (r"(?:lossy|Lossy) (?:compression|Compression) has been performed on the (?:image|Image)s?(?:\.|$)",
     (False, [("EQUALS", 0x00282110, 0xFFFFFFFF, 0, 0, ["01"])])),
]

# Words that may start a condition's attribute reference ("the Value of Image Type (0008,0008)").
HEAD_PREFIX = re.compile(r"^(?:if |and if |or if )?(?:the [Vv]alue of |the [Vv]alues of |the [Vv]alue for |a [Vv]alue of |"
                         r"any [Vv]alue of |the Attribute |Attribute |the )?")
# Heads that move the condition into another context (an item, a referenced instance, a frame),
# or that hold a predicate (the split was wrong): those are not evaluated.
HEAD_REJECT = re.compile(r"\b(?:within|item|Item|Macro|Module|this|that|enclosing|referenced|is|are|has|have|"
                         r"equals|present|absent|not|value|in the|of the|in any|in an|of any|of an|of each|for this)\b")
HEAD_NAME = re.compile(r"[A-Z0-9][A-Za-z0-9'’\-/ ]*?(?: ?\([A-Za-z ]+\)[A-Za-z0-9 ]*)?")
CONNECTIVE = re.compile(r",? (and|or) ")


def head_ok(head):
    h = HEAD_PREFIX.sub("", head.strip()).strip()
    if not h or HEAD_REJECT.search(h):
        return False
    return HEAD_NAME.fullmatch(h) is not None


def split_candidates(between):
    """The ways to split the text between two attribute references into (this term's tail,
    connective, head of the next term): at a connective after which a plain attribute name
    follows, the last connective first."""
    out = []
    for m in reversed(list(CONNECTIVE.finditer(between))):
        nxt = between[m.end():]
        if head_ok(nxt):
            out.append((between[: m.start()], m.group(1), nxt))
    return out


def values_of_text(text):
    out = []
    for v in re.findall(ONE_VALUE, text):
        v = re.sub(r" ?\([^()]*\)$", "", v).strip()
        if v.startswith('"') and v.endswith('"'):
            v = v[1:-1]
        if v and v != "or":
            out.append(v)
    return out


def parse_term(tail, tag, mask):
    """(op, tag, mask, value index (0 = any value), top level, values) or None. An empty tail
    returns "SHARED": the predicate of the next term applies ("A or B is greater than 1")."""
    t = tail.strip().rstrip(" .,;")
    t = t.lstrip(", ").strip()
    if not t:
        return "SHARED"
    index = 0
    top = 0
    m = re.match(r"^[Vv]alue (\d+)(?: of this Frame)?,? ", t + " ")
    if m:
        index = int(m.group(1))
        t = t[m.end():].strip()
    m = re.match(r"^at the (?:image|top) level,? ", t + " ")
    if m:
        top = 1
        t = t[m.end():].strip()
    if re.match(r"^[Vv]alue (?:is|equals|has)\b", t):
        t = t[6:]
    t = t.strip()

    def term(op, vals=()):
        return (op, tag, mask, index, top, list(vals))

    if re.fullmatch(r"(?:is|are) present(?: in the Data ?[Ss]et)?", t):
        return term("PRESENT")
    if re.fullmatch(r"(?:is present and (?:has|contains) a [Vv]alue|has a [Vv]alue|is present with a [Vv]alue|"
                    r"is present and is not empty|is not empty|is present and not empty|is sent with a [Vv]alue)", t):
        return term("HAS_VALUE")
    if re.fullmatch(r"(?:is|are) (?:not present|absent|not sent)(?: in the Data ?[Ss]et)?", t):
        return term("ABSENT")
    if re.fullmatch(r"(?:is present and )?(?:is non-zero|has a non-zero [Vv]alue|is greater than zero|is not zero)", t):
        return term("GREATER", ["0"])
    mm = re.fullmatch(r"(?:is present and )?(?:is|has a [Vv]alue|[Vv]alue is|has a [Vv]alue that is) greater than (-?\d+)", t)
    if mm:
        return term("GREATER", [mm.group(1)])
    mm = re.fullmatch(r"(?:is|has a [Vv]alue) greater than or equal to (-?\d+)", t)
    if mm:
        return term("GREATER", [str(int(mm.group(1)) - 1)])
    mm = re.fullmatch(r"is (?:absent|not present),? or (?:is present and )?has (?:the|a) [Vv]alue(?: of)? (" + VALUES + ")", t)
    if mm:
        return term("ABSENT_OR_EQUALS", values_of_text(mm.group(1)))
    mm = re.fullmatch(r"(?:is present and )?(?:has a [Vv]alue other than|is not|is other than|does not equal|"
                      r"is not equal to|equals other than|has a [Vv]alue that is not|is present with a [Vv]alue other than)"
                      r" (" + VALUES + ")", t)
    if mm:
        return term("NOT_EQUALS", values_of_text(mm.group(1)))
    mm = re.fullmatch(r"(?:is present and |is present with )?(?:has a [Vv]alue of|has the [Vv]alue of|has the [Vv]alue|"
                      r"has [Vv]alue|has a [Vv]alue|equals|is equal to|is|=|[Vv]alue is|the [Vv]alue|[Vv]alue|"
                      r"has the [Vv]alues?|has [Vv]alues of|has a [Vv]alue of|has one of the [Vv]alues|equals one of the [Vv]alues|is one of)"
                      r":? (" + VALUES + ")", t)
    if mm:
        vals = values_of_text(mm.group(1))
        if vals:
            return term("EQUALS", vals)
    return None


INCLUDES_TAG = re.compile(r",? (?:[Vv]alue )?includes the (?:Data Element )?Tag (?:for|of) [A-Z][A-Za-z0-9'\- ]*? ")


def parse_expr(cond):
    """(any, [terms]) for a condition built from attribute terms joined by only 'and' or only
    'or', or None. Every way of splitting at the connectives is tried (an attribute name may
    itself contain 'and'); the first full parse wins."""
    refs = list(REF.finditer(cond))
    if not refs:
        return None
    for r in refs:
        if parse_tag(r.group(0)) is None:
            return None

    def walk(i, head, terms, conns, shared):
        if not head_ok(head):
            return None
        r = refs[i]
        pt = parse_tag(r.group(0))
        j = i + 1
        includes = None
        if j < len(refs) and INCLUDES_TAG.fullmatch(cond[r.end():refs[j].start()]):
            p2 = parse_tag(refs[j].group(0))
            if p2[1] != 0xFFFFFFFF or shared:
                return None
            includes = ("INCLUDES_TAG", pt[0], pt[1], 0, 0, ["%08X" % p2[0]])
            r = refs[j]
            j += 1
        last = j >= len(refs)
        between = cond[r.end():(refs[j].start() if not last else len(cond))]
        options = [(between, None, "")] if last else split_candidates(between)
        for tail, conn, nhead in options:
            if includes:
                if tail.strip(" ,."):
                    continue
                t = includes
            else:
                t = parse_term(tail, pt[0], pt[1])
                if t is None:
                    continue
            if t == "SHARED":
                if last:
                    continue
                nterms, nshared = terms, shared + [pt]
            else:
                nterms = terms + [(t[0], s[0], s[1], t[3], t[4], t[5]) for s in shared] + [t]
                nshared = []
            nconns = conns + ([conn] if conn else [])
            if len(set(nconns)) > 1:
                continue
            if last:
                if nshared or not nterms:
                    continue
                return (bool(nconns) and nconns[0] == "or", nterms)
            res = walk(j, nhead, nterms, nconns, nshared)
            if res is not None:
                return res
        return None

    return walk(0, cond[: refs[0].start()], [], [], [])


# Attribute names of the data dictionary (src/dict/dictionary_data.inc, itself generated from
# PS3.6), for conditions that name an attribute without its tag ("Rescale Intercept is present").
DICT_INC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "dict", "dictionary_data.inc")
_NAMES = None


def dictionary_names():
    global _NAMES
    if _NAMES is None:
        _NAMES = []
        pat = re.compile(r'Entry\{0x([0-9A-F]{8})u, 0xFFFFFFFFu, VR::\w+, VR::\w+, "\w*", "([^"]+)", "[^"]*", false\}')
        with open(DICT_INC, encoding="utf-8") as f:
            for line in f:
                m = pat.search(line)
                # Names of a few characters ("Item") or with a lower-case start are not looked for.
                if m and len(m.group(2)) >= 8 and m.group(2)[0].isupper() and m.group(1)[:4] not in ("0000", "FFFE"):
                    _NAMES.append((m.group(2), int(m.group(1), 16)))
        _NAMES.sort(key=lambda x: -len(x[0]))
    return _NAMES


_NAME_RE = None


def add_missing_tags(cond):
    """Appends "(gggg,eeee)" to dictionary attribute names that the condition names without a tag
    (the longest name wins where names overlap)."""
    global _NAME_RE
    if _NAME_RE is None:
        names = dictionary_names()
        _NAME_RE = (re.compile(r"(?<![A-Za-z0-9])(?:" + "|".join(re.escape(n) for n, _ in names) + r")(?![A-Za-z0-9])"),
                    dict(names))
    rx, tags = _NAME_RE
    taken = [(m.start(), m.end()) for m in REF.finditer(cond)]
    inserts = []
    for m in rx.finditer(cond):
        s, e = m.start(), m.end()
        if any(s < b and e > a for a, b in taken):
            continue
        if REF.match(cond, e) or (cond[e:e + 1] == " " and REF.match(cond, e + 1)):
            continue
        tag = tags[m.group(0)]
        inserts.append((e, " (%04X,%04X)" % (tag >> 16, tag & 0xFFFF)))
    for pos, text in sorted(inserts, reverse=True):
        cond = cond[:pos] + text + cond[pos:]
    return cond


# Wordings that PS3.3 uses for a fixed attribute test, rewritten before parsing.
REWRITES = [
    # PS3.3 C.7.6.6: the number of frames is Number of Frames (0028,0008).
    (r"\bPixel Data \(7FE0,0010\) contains more than one Frame\b", "Number of Frames (0028,0008) is greater than 1"),
    (r"\b(?:multi-frame|Multi-frame) pixel data (?:are|is) present\b", "Number of Frames (0028,0008) is greater than 1"),
    (r"\bthe (?:image|Image) is a (?:multi-frame|Multi-frame) (?:image|Image)\b", "Number of Frames (0028,0008) is greater than 1"),
    (r"^either ", ""),
    (r"\bwhose SOP Class UID \(0008,0016\) is one of the following:", "SOP Class UID (0008,0016) is"),
    (r" points to ", " includes the Tag for "),
]

# Conditions written as prose for which PS3.3 itself gives the machine-checkable meaning, by
# (table, tag). Each entry cites the text it implements.
STRUCTURED = {
    # General Image Patient Orientation (0020,0020) 2C: "Required if image does not require Image
    # Orientation (Patient) (0020,0037) and Image Position (Patient) (0020,0032) or if image does not
    # require Image Orientation (Slide) (0048,0102)". An image carrying none of those three is one
    # that does not require them (an IOD that requires them is checked for them by its own module).
    # The three are looked for anywhere in the data set (scope 2: NM puts them in Detector
    # Information Sequence items, enhanced images in functional groups).
    ("table_C.7-9", 0x00200020): (False, [("ABSENT", 0x00200037, 0xFFFFFFFF, 0, 2, []),
                                          ("ABSENT", 0x00200032, 0xFFFFFFFF, 0, 2, []),
                                          ("ABSENT", 0x00480102, 0xFFFFFFFF, 0, 2, [])]),
}

REQUIRED_IF = re.compile(r"(?:Required (?:only )?if|Shall be present (?:only )?if|Required for images where) (.*?)(?:\.(?:\s|$)|;|$)")
# "may be present otherwise": a 1C / 2C attribute that may also be present when its condition does
# not hold (PS3.5 7.4: without these words it shall not be present then).
OTHERWISE = re.compile(r"(?:[Mm]ay be present otherwise|[Oo]therwise(?:,)? (?:it )?may be present|"
                       r"[Mm]ay be present (?:if|when|for|in|even)|[Oo]therwise optional|may also be present)")


def parse_condition(desc):
    """(any, [(op, tag, mask, value index, top level, values)]) for the machine-checkable shapes,
    or None (not evaluated)."""
    for phrase, cond in PHRASES:
        if re.search(r"Required if " + phrase, desc):
            return cond
    m = REQUIRED_IF.search(desc)
    if not m:
        return None
    cond = m.group(1).strip()
    # Strip a trailing ", may be present otherwise" / "otherwise ..." clause.
    cond = re.split(r",? (?:otherwise|overriding|may be present otherwise|May be present otherwise)", cond)[0].strip()
    for pat, rep in REWRITES:
        cond = re.sub(pat, rep, cond)
    return parse_expr(add_missing_tags(cond))


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
        self.cond_terms = []
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
        any_, terms = c
        key = (any_, tuple((t[0], t[1], t[2], t[3], t[4], tuple(t[5])) for t in terms))
        if key not in self.cond_index:
            first_term = len(self.cond_terms)
            for op, tag, mask, index, top, values in terms:
                first = len(self.enum_values)
                self.enum_values.extend(values)
                self.cond_terms.append((op, tag, mask, index, top, first, len(values)))
            self.cond_index[key] = len(self.conditions)
            self.conditions.append((1 if any_ else 0, first_term, len(terms)))
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
                    out.append((depth, 0, 0, 6, inc, self.cond(pc), -1, 0, 0))
                else:
                    out.append((depth, 0, 0, 0, inc, -1, -1, 0, 0))
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
            strict = 0
            if ty in (2, 4):
                structured = STRUCTURED.get((tid, pt[0]))
                pc = structured or parse_condition(desc)
                # Without "may be present otherwise" the attribute shall not be present when the
                # condition does not hold (PS3.5 7.4.4, 7.4.6). Not for the STRUCTURED rules: they
                # decide "required" only where the prose can be decided, so "false" may mean unknown.
                strict = 0 if structured or OTHERWISE.search(desc) else 1
                if pc is None:
                    self.stats["unparsed_conditions"] += 1
                else:
                    self.stats["parsed_conditions"] += 1
                c = self.cond(pc)
            e = self.enum(enumerated_values(desc_td)) if desc_td is not None else -1
            # "This type definition shall override the definition in the <other> Module" (e.g. SC
            # Equipment Modality): recorded so the other module's Type is replaced in this IOD.
            ov = 1 if re.search(r"type definition shall override the definition", desc) else 0
            out.append((depth, pt[0], pt[1], ty, -1, c, e, ov, strict))
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
        L.append("// (Module and Macro Attribute tables); DICOM %s Table B.5-1 (Standard SOP Classes);" % e4)
        L.append("// DICOM PS3.16 Tables L-1 / L-5 (paired structures, Laterality condition).")
        L.append("// Included by src/repair/iod.cpp inside namespace siaara::repair::detail.")
        L.append("// %d attribute tables, %d attribute rows, %d IODs, %d IOD module rows, %d SOP classes," % (
            len(self.attr_tables), len(self.rows), len(self.iods), len(self.module_rows), len(self.sop)))
        L.append("// %d conditions of %d terms (%d 1C/2C rows parsed, %d not evaluated), %d enumerated value lists, %d strings." % (
            len(self.conditions), len(self.cond_terms), self.stats["parsed_conditions"], self.stats["unparsed_conditions"],
            len(self.enum_lists), len(self.enum_values)))
        L.append("// %d conditional Include rows (\"Include ... if ...\")." % self.stats["conditional_includes"])
        L.append("// Row fields: depth, tag, mask, type (1=1, 2=1C, 3=2, 4=2C, 5=3; 0 = Include row, 6 = conditional")
        L.append("// Include row whose condition is the row's condition, -1 when not evaluated),")
        L.append("// included table (-1), condition (-1), enumerated value list (-1), overrides the Type given")
        L.append("// by another module of the IOD (\"This type definition shall override ...\"), 1C/2C without")
        L.append("// \"may be present otherwise\" (shall not be present when the condition does not hold).")
        L.append("")
        L.append("constexpr AttrRow kAttrRows[] = {")
        for r in self.rows:
            L.append("    {%d, 0x%08Xu, 0x%08Xu, %d, %d, %d, %d, %d, %d}," % r)
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
        L.append("constexpr CondTermRow kCondTerms[] = {")
        if not self.cond_terms:
            L.append("    {CondOp::None, 0u, 0u, 0, 0, 0u, 0u},")
        for op, tag, mask, index, top, first, count in self.cond_terms:
            opname = {"PRESENT": "Present", "ABSENT": "Absent", "EQUALS": "Equals", "NOT_EQUALS": "NotEquals",
                      "GREATER": "Greater", "HAS_VALUE": "HasValue", "ABSENT_OR_EQUALS": "AbsentOrEquals",
                      "INCLUDES_TAG": "IncludesTag"}[op]
            L.append("    {CondOp::%s, 0x%08Xu, 0x%08Xu, %d, %d, %du, %du}," % (opname, tag, mask, index, top, first, count))
        L.append("};")
        L.append("")
        L.append("constexpr ConditionRow kConditions[] = {")
        if not self.conditions:
            L.append("    {0, 0u, 0u},")
        for any_, first, count in self.conditions:
            L.append("    {%d, %du, %du}," % (any_, first, count))
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


def paired_body_parts(p16):
    """Body Part Examined terms (PS3.16 Table L-1) whose anatomic concept is a paired structure
    (PS3.16 Table L-5 "Pairedness of Anatomic Concepts", Paired Structure Y)."""
    tables = {t.get(XML_ID): t for t in p16.iter(DB + "table")}

    def rows(tid):
        t = tables.get(tid)
        if t is None:
            raise SystemExit("PS3.16 %s not found" % tid)
        return [[text(td) for td in tr.findall(DB + "td")] for tr in body_rows(t)]

    paired = {r[0] for r in rows("table_L-5") if len(r) > 2 and r[2] == "Y"}
    return sorted({r[3] for r in rows("table_L-1") if len(r) > 3 and r[1] in paired and r[3]})


def add_laterality_rule(p16):
    # General Series Laterality (0020,0060) 2C: "Required if the body part examined is a paired
    # structure and Image Laterality (0020,0062) or Frame Laterality (0020,9072) or Measurement
    # Laterality (0024,0113) are not present". Paired: Body Part Examined (0018,0015) is one of
    # the PS3.16 Annex L terms for a paired structure; an image without Body Part Examined is not
    # judged (the Equals term is false). The other three are looked for anywhere (scope 2).
    STRUCTURED[("table_C.7-5a", 0x00200060)] = (False, [
        ("EQUALS", 0x00180015, 0xFFFFFFFF, 0, 0, paired_body_parts(p16)),
        ("ABSENT", 0x00200062, 0xFFFFFFFF, 0, 2, []),
        ("ABSENT", 0x00209072, 0xFFFFFFFF, 0, 2, []),
        ("ABSENT", 0x00240113, 0xFFFFFFFF, 0, 2, []),
    ])


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    p3 = ET.parse(sys.argv[1]).getroot()
    p4 = ET.parse(sys.argv[2]).getroot()
    add_laterality_rule(ET.parse(sys.argv[3]).getroot())
    out = sys.argv[4]
    g = Gen(p3, p4)
    g.run()
    st = g.write(out)
    print("attribute tables %d, rows %d, IODs %d, module rows %d, SOP classes %d, conditions %d, "
          "enum lists %d, conditional includes %d, unresolved includes %d, 1C/2C parsed %d, not evaluated %d" % (
              len(g.attr_tables), len(g.rows), len(g.iods), len(g.module_rows), len(g.sop),
              len(g.conditions), len(g.enum_lists), st["conditional_includes"], st["unresolved_includes"],
              st["parsed_conditions"], st["unparsed_conditions"]))


if __name__ == "__main__":
    main()
