#!/usr/bin/env python3
"""Generates the named data layer from the DICOM standard's DocBook XML.

Usage:
    python3 -I tools/gen_modules.py <part03.xml> <part06.xml> [--check] [--out-root DIR]

Sources (https://dicom.nema.org/medical/dicom/current/source/docbook/):
    part03/part03.xml : every Module and Macro attribute table of PS3.3 (tables whose header is
                        "Attribute Name | Tag | [Type |] ... Description"), their nested sequence
                        rows ('>' depth), their "Include Table ..." rows, and the IOD "Functional
                        Group Macros" tables of Annex A (the items of the Shared / Per-Frame
                        Functional Groups Sequences can hold any of those macros).
    part06/part06.xml : tables 6-1, 7-1, 8-1, 9-1 for each attribute's keyword, VR and VM.

Writes (relative to the repository root, or --out-root):
    src/modules/module_tables.inc                   compact C++ tables (nodes, entries, attributes)
    bindings/csharp/src/Siaara/Generated/Modules.g.cs  typed, read-only C# classes

Model. Every attribute table is a node; the rows of a sequence's item ('>' one deeper) form an
anonymous item node, unless the item consists of exactly one Include row, in which case the item
node is the included table's node. Include rows stay references in the C++ table (the runtime
resolves them recursively, with cycle protection); the C# classes flatten them (recursively, a
table already on the include stack is skipped, so recursive definitions such as the SR content
tree terminate; the sequence still refers to the class, so the recursion is reachable at run time).
An attribute that occurs twice in one flattened node keeps its first definition.

--check compares the generated text with the committed files and exits 1 on any difference.
Only the Python standard library is used; the output is deterministic (document order).
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True  # importing gen_iod must not leave a .pyc in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_iod as gi  # noqa: E402  (shared PS3.3 parsing: text, tags, tables, rows, Include targets)

DB = gi.DB
XML_ID = gi.XML_ID
TYPES = {"1": 1, "1C": 2, "2": 3, "2C": 4, "3": 5}
KNOWN_VRS = {
    "AE", "AS", "AT", "CS", "DA", "DS", "DT", "FL", "FD", "IS", "LO", "LT", "OB", "OD", "OF",
    "OL", "OV", "OW", "PN", "SH", "SL", "SQ", "SS", "ST", "SV", "TM", "UC", "UI", "UL", "UN",
    "UR", "US", "UT", "UV",
}
KIND_MODULE, KIND_MACRO, KIND_ITEM, KIND_TABLE, KIND_GROUP = 1, 2, 3, 4, 5
FG_SEQUENCES = (0x52009229, 0x52009230)  # Shared / Per-Frame Functional Groups Sequence


# ---------------------------------------------------------------- PS3.6

def dictionary(p6):
    """(tag, mask) -> (keyword, vr text, vm, retired)."""
    out = {}
    for tid in ("table_6-1", "table_7-1", "table_8-1", "table_9-1"):
        tbl = None
        for t in p6.iter(DB + "table"):
            if t.get(XML_ID) == tid:
                tbl = t
                break
        if tbl is None:
            raise SystemExit("PS3.6 %s not found" % tid)
        for tr in gi.body_rows(tbl):
            cells = [gi.text(td) for td in tr.findall(DB + "td")]
            if len(cells) < 5:
                continue
            pt = gi.parse_tag(cells[0])
            if pt is None or not cells[2]:
                continue
            vrs = [v for v in re.findall(r"\b[A-Z]{2}\b", cells[3]) if v in KNOWN_VRS]
            retired = len(cells) > 5 and "RET" in cells[5].upper()
            out[pt] = (cells[2], "/".join(vrs), cells[4], retired)
    return out


class Dict:
    def __init__(self, entries):
        self.exact = {t: v for (t, m), v in entries.items() if m == 0xFFFFFFFF}
        self.masked = [(t, m, v) for (t, m), v in sorted(entries.items()) if m != 0xFFFFFFFF]

    def get(self, tag, mask):
        if mask == 0xFFFFFFFF and tag in self.exact:
            return self.exact[tag]
        for t, m, v in self.masked:
            if (tag & m) == t and (mask & m) == m:
                return v
        return self.exact.get(tag)


# ---------------------------------------------------------------- PS3.3 model

class Attr:
    __slots__ = ("tag", "mask", "type", "child")

    def __init__(self, tag, mask, ty):
        self.tag, self.mask, self.type, self.child = tag, mask, ty, None


class Include:
    __slots__ = ("target", "conditional")

    def __init__(self, target, conditional):
        self.target, self.conditional = target, conditional


class Node:
    def __init__(self, kind, name, table_id, owner=None, seq_tag=None):
        self.kind = kind
        self.name = name
        self.table_id = table_id
        self.owner = owner        # table node an item node belongs to
        self.seq_tag = seq_tag    # sequence of an item node
        self.entries = []
        self.index = -1
        self.cs = None            # C# class name


def is_named_table(tbl):
    h = gi.header(tbl)
    if len(h) < 3 or h[0].lower() != "attribute name" or h[1] != "Tag":
        return False
    return True


def caption(tbl):
    c = tbl.find(DB + "caption")
    return gi.text(c) if c is not None else ""


def table_name(cap):
    n = re.sub(r"\s+Attributes?$", "", cap).strip()
    return n or cap


class Model:
    def __init__(self, p3, d):
        self.p3 = p3
        self.d = d
        self.tables = gi.Tables(p3)
        self.nodes = []
        self.by_table = {}
        self.stats = {"unresolved_includes": 0, "unknown_tags": 0, "fg_includes": 0}
        for tid in self.tables.order:
            tbl = self.tables.by_id[tid]
            if not is_named_table(tbl):
                continue
            cap = caption(tbl)
            name = table_name(cap)
            if name.startswith("Example"):
                continue  # PS3.3 Section 5 notation examples, not modules
            kind = KIND_MODULE if name.endswith("Module") else KIND_MACRO if "Macro" in name else KIND_TABLE
            n = Node(kind, name, tid[len("table_"):])
            self.by_table[tid] = n
            self.add(n)
        self.fg = Node(KIND_GROUP, "Functional Group Macros", "")
        self.add(self.fg)
        for tid in self.tables.order:
            self.parse_table(tid)
        self.collect_functional_groups()

    def add(self, n):
        n.index = len(self.nodes)
        self.nodes.append(n)

    def module_table_for_section(self, sid):
        for tid in self.tables.section_tables.get(sid, []):
            if tid in self.by_table:
                return tid
        return None

    def collect_functional_groups(self):
        seen = set()
        for tid in self.tables.order:
            tbl = self.tables.by_id[tid]
            if not caption(tbl).endswith("Functional Group Macros"):
                continue
            h = gi.header(tbl)
            if not h or h[0] != "Functional Group Macro":
                continue
            for tr in gi.body_rows(tbl):
                for x in tr.iter(DB + "xref"):
                    le = x.get("linkend", "")
                    t = self.module_table_for_section(le) if le.startswith("sect_") else le
                    if t in self.by_table and t not in seen:
                        seen.add(t)
                        self.fg.entries.append(Include(self.by_table[t], False))
                    break

    def parse_table(self, tid):
        node = self.by_table.get(tid)
        if node is None:
            return
        tbl = self.tables.by_id[tid]
        h = gi.header(tbl)
        type_col = h.index("Type") if "Type" in h else -1
        # stack[d] = node receiving rows of depth d
        stack = [node]
        last_attr = {}
        for tr in gi.body_rows(tbl):
            tds = tr.findall(DB + "td")
            if not tds:
                continue
            first = gi.text(tds[0])
            depth = len(first) - len(first.lstrip(">"))
            rest = first.lstrip(">").strip()
            if depth >= len(stack):
                # Item rows: the item node of the last sequence one level up.
                parent = last_attr.get(depth - 1)
                if parent is None or depth != len(stack):
                    continue
                if parent.child is None:
                    parent.child = Node(KIND_ITEM, "", "", owner=node, seq_tag=parent.tag)
                stack.append(parent.child)
            else:
                del stack[depth + 1:]
            target_node = stack[depth]
            if rest.startswith("Include") and (len(tds) < 2 or gi.parse_tag(gi.text(tds[1])) is None):
                target = gi.include_target(tds[0])
                if target is None and "Functional Group Macro" in rest:
                    self.stats["fg_includes"] += 1
                    target_node.entries.append(Include(self.fg, True))
                    continue
                if target is None or target not in self.by_table:
                    self.stats["unresolved_includes"] += 1
                    continue
                cond = re.search(r"\b(?:if and only if|only if|if)\b", rest) is not None
                target_node.entries.append(Include(self.by_table[target], cond))
                continue
            if len(tds) < 2:
                continue
            pt = gi.parse_tag(gi.text(tds[1]))
            if pt is None:
                continue
            ty = TYPES.get(gi.text(tds[type_col]), 0) if 0 <= type_col < len(tds) else 0
            a = Attr(pt[0], pt[1], ty)
            target_node.entries.append(a)
            last_attr[depth] = a
            for k in [k for k in last_attr if k > depth]:
                del last_attr[k]

    def finish(self):
        """Registers item nodes (document order) and replaces single-Include items by the table."""
        order = []

        def visit(n):
            for e in n.entries:
                if isinstance(e, Attr) and e.child is not None:
                    c = e.child
                    if len(c.entries) == 1 and isinstance(c.entries[0], Include):
                        e.child = c.entries[0].target
                        continue
                    order.append(c)
                    visit(c)

        for n in list(self.nodes):
            visit(n)
        for c in order:
            self.add(c)
        for n in self.nodes:
            if n.kind == KIND_ITEM:
                kw = self.keyword(n.seq_tag, 0xFFFFFFFF) or "%08X" % n.seq_tag
                n.name = "%s > %s item" % (n.owner.name, kw)
        # Functional Groups Sequences: their items hold any Functional Group Macro.
        for n in self.nodes:
            for e in n.entries:
                if isinstance(e, Attr) and e.tag in FG_SEQUENCES and e.child is None:
                    e.child = self.fg

    def keyword(self, tag, mask):
        v = self.d.get(tag, mask)
        return v[0] if v else None

    def info(self, tag, mask):
        return self.d.get(tag, mask)


# ---------------------------------------------------------------- flattening (C#)

def flatten(node, stack=None):
    """[(Attr, conditional)] with Includes expanded; first definition of a tag wins."""
    if stack is None:
        stack = []
    if node in stack:
        return []
    stack.append(node)
    out = []
    seen = set()
    for e in node.entries:
        items = []
        if isinstance(e, Attr):
            items = [e]
        else:
            items = flatten(e.target, stack)
        for a in items:
            key = (a.tag, a.mask)
            if key in seen:
                continue
            seen.add(key)
            out.append(a)
    stack.pop()
    return out


# ---------------------------------------------------------------- naming

def pascal(name):
    name = name.replace("X-Ray", "XRay").replace("x-ray", "XRay").replace("'", "").replace("’", "")
    words = re.split(r"[^A-Za-z0-9]+", name)
    out = "".join(w[:1].upper() + w[1:] for w in words if w)
    if not out:
        out = "Unnamed"
    if out[0].isdigit():
        out = "M" + out
    return out


# Members of ModuleBase / object a generated property must not hide.
RESERVED = {"Dataset", "IsPresent", "TableId", "SourceOf", "Item", "Equals", "GetHashCode", "ToString", "GetType",
            "MemberwiseClone", "Finalize", "ReferenceEquals"}


class CSharp:
    def __init__(self, m):
        self.m = m
        self.used = set(["DicomModules", "ModuleBase", "PersonName", "PersonNameGroup"])
        self.stats = {"classes": 0, "properties": 0, "skipped_unknown": 0}
        for n in m.nodes:
            if n.kind == KIND_ITEM:
                continue
            base = pascal(n.name) if n.kind != KIND_GROUP else "FunctionalGroupMacros"
            n.cs = self.unique(base, n.table_id)
        for n in m.nodes:
            if n.kind != KIND_ITEM:
                continue
            kw = m.keyword(n.seq_tag, 0xFFFFFFFF) or ("Seq%08X" % n.seq_tag)
            stem = re.sub(r"Sequence$", "", kw)
            base = stem + "Item"
            if base in self.used:
                owner = pascal(re.sub(r"\s*(Module|Macro)$", "", n.owner.name))
                # "Beam Limiting Device Position Macro" > BeamLimitingDevicePositionSequence ->
                # BeamLimitingDevicePositionMacroItem, not ...PositionBeamLimitingDevicePositionItem.
                base = (pascal(n.owner.name) + "Item") if owner.endswith(stem) else owner + base
            n.cs = self.unique(base, None)

    def unique(self, base, table_id):
        name = base
        if name in self.used and table_id:
            name = base + "_" + re.sub(r"[^A-Za-z0-9]", "_", table_id)
        k = 2
        while name in self.used:
            name = "%s%d" % (base, k)
            k += 1
        self.used.add(name)
        return name

    @staticmethod
    def vr_kind(vr, vm):
        multi = vm.strip() != "1"
        alts = vr.split("/") if vr else []
        if not alts:
            return "bytes", True
        if alts == ["SQ"]:
            return "seq", True
        if any(v.startswith("O") or v == "UN" for v in alts):
            return "bytes", True
        if all(v in ("US", "SS", "UL", "SL", "SV", "UV", "IS") for v in alts):
            return "int", multi
        if all(v in ("DS", "FL", "FD") for v in alts):
            return "double", multi
        v = alts[0]
        if v == "PN":
            return "pn", multi
        if v == "DA":
            return "date", multi
        if v == "TM":
            return "time", multi
        if v == "DT":
            return "datetime", multi
        if v == "AT":
            return "tag", multi
        return "string", multi

    def render(self):
        m = self.m
        L = []
        L.append("// <auto-generated>")
        L.append("// Generated by tools/gen_modules.py from DICOM %s (module / macro attribute tables) and" % gi.edition(m.p3))
        L.append("// DICOM PS3.6 (keywords, VR, VM). Do not edit.")
        L.append("// </auto-generated>")
        L.append("#nullable enable")
        L.append("#pragma warning disable CS1591")
        L.append("using System;")
        L.append("using System.Collections.Generic;")
        L.append("")
        L.append("namespace Siaara.Modules;")
        L.append("")
        L.append("/// <summary>Every PS3.3 module and macro of a data set (see docs/DATA_ACCESS.md).</summary>")
        L.append("public sealed partial class DicomModules")
        L.append("{")
        for n in m.nodes:
            if n.kind in (KIND_ITEM,):
                continue
            prop = re.sub(r"Module$", "", n.cs) if n.kind == KIND_MODULE and n.cs != "Module" else n.cs
            if prop == "DicomModules" or not prop:
                prop = n.cs
            L.append("    /// <summary>%s%s.</summary>" % (xml_esc(n.name), (" (PS3.3 Table %s)" % n.table_id) if n.table_id else ""))
            L.append("    public %s %s => new(_ds);" % (n.cs, prop))
        L.append("}")
        L.append("")
        for n in m.nodes:
            self.render_class(n, L)
        return "\n".join(L) + "\n"

    def render_class(self, n, L):
        m = self.m
        self.stats["classes"] += 1
        where = (" (PS3.3 Table %s)" % n.table_id) if n.table_id else ""
        L.append("/// <summary>%s%s. Read-only and lazy: each property reads the data set when accessed.</summary>" %
                 (xml_esc(n.name), where))
        L.append("public sealed partial class %s : ModuleBase" % n.cs)
        L.append("{")
        L.append("    internal %s(DicomDataset ds) : base(ds) { }" % n.cs)
        if n.table_id:
            L.append("    /// <summary>The PS3.3 table this class is generated from.</summary>")
            L.append("    public const string TableId = \"%s\";" % n.table_id)
            L.append("    /// <summary>True when at least one attribute of this table (Includes resolved) is present.</summary>")
            L.append("    public bool IsPresent => IsTablePresent(TableId);")
        names = set()
        for a in flatten(n):
            inf = m.info(a.tag, a.mask)
            if inf is None:
                self.stats["skipped_unknown"] += 1
                continue
            kw, vr, vm, retired = inf
            if not kw:
                continue
            prop = kw
            if prop in RESERVED or prop == n.cs or prop in names or prop.startswith("Get") or prop.startswith("IsTable"):
                prop = kw + "Value"
            names.add(prop)
            kind, multi = self.vr_kind(vr, vm)
            t = "0x%08Xu" % a.tag
            ty = {0: "", 1: "1", 2: "1C", 3: "2", 4: "2C", 5: "3"}[a.type]
            doc = "%s (%04X,%04X) %s, VM %s%s%s." % (kw, a.tag >> 16, a.tag & 0xFFFF, vr.replace("/", " or ") or "-", vm,
                                                      (", Type " + ty) if ty else "", ", retired" if retired else "")
            L.append("    /// <summary>%s</summary>" % xml_esc(doc))
            self.stats["properties"] += 1
            if kind == "seq":
                child = a.child
                if child is None:
                    L.append("    public IReadOnlyList<DicomDataset> %s => GetItems(%s);" % (prop, t))
                else:
                    L.append("    public IReadOnlyList<%s> %s => GetSeq(%s, static d => new %s(d));" % (child.cs, prop, t, child.cs))
                continue
            if kind == "bytes":
                L.append("    public byte[]? %s => GetBytes(%s);" % (prop, t))
                continue
            single = {"int": ("long?", "GetInt"), "double": ("double?", "GetDbl"), "string": ("string?", "GetStr"),
                      "pn": ("PersonName?", "GetPn"), "date": ("DateOnly?", "GetDate"), "time": ("TimeOnly?", "GetTime"),
                      "datetime": ("System.DateTime?", "GetDT"), "tag": ("DicomTag?", "GetTag")}
            multi_t = {"int": ("long[]?", "GetInts"), "double": ("double[]?", "GetDbls"), "string": ("string[]?", "GetStrs"),
                       "pn": ("PersonName[]?", "GetPns"), "date": ("DateOnly?[]?", "GetDates"), "time": ("TimeOnly?[]?", "GetTimes"),
                       "datetime": ("System.DateTime?[]?", "GetDTs"), "tag": ("DicomTag[]?", "GetTags")}
            ctype, fn = (multi_t if multi else single)[kind]
            L.append("    public %s %s => %s(%s);" % (ctype, prop, fn, t))
            if kind in ("date", "time", "datetime"):
                raw = prop + "Raw"
                if raw in names:
                    raw = prop + "RawValue"
                names.add(raw)
                L.append("    /// <summary>%s as stored (all values, backslash separated), never parsed.</summary>" % kw)
                L.append("    public string? %s => GetStr(%s);" % (raw, t))
        L.append("}")
        L.append("")


def xml_esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ---------------------------------------------------------------- C++

def c_str(s):
    return gi.c_str(s)


def render_cpp(m):
    attrs = []
    attr_index = {}
    entries = []
    node_rows = []
    for n in m.nodes:
        first = len(entries)
        for e in n.entries:
            if isinstance(e, Attr):
                key = (e.tag, e.mask)
                if key not in attr_index:
                    inf = m.info(e.tag, e.mask)
                    attr_index[key] = len(attrs)
                    attrs.append((e.tag, e.mask, inf))
                entries.append((attr_index[key], e.child.index if e.child is not None else -1, e.type))
            else:
                entries.append((-1, e.target.index, 7 if e.conditional else 6))
        node_rows.append((n, first, len(entries) - first))
    L = []
    L.append("// Generated by tools/gen_modules.py - do not edit.")
    L.append("// Sources: DICOM %s module and macro attribute tables (and the Annex A Functional Group" % gi.edition(m.p3))
    L.append("// Macros tables), DICOM PS3.6 keywords / VR / VM. Included by src/modules/modules.cpp.")
    nm = sum(1 for n in m.nodes if n.kind == KIND_MODULE)
    nma = sum(1 for n in m.nodes if n.kind == KIND_MACRO)
    nit = sum(1 for n in m.nodes if n.kind == KIND_ITEM)
    L.append("// %d nodes: %d modules, %d macros, %d other tables, 1 functional-group set, %d sequence item nodes;" % (
        len(m.nodes), nm, nma, len(m.nodes) - nm - nma - nit - 1, nit))
    L.append("// %d entries, %d distinct attributes." % (len(entries), len(attrs)))
    L.append("// Entry fields: attribute (-1 = Include row), node (item node of a sequence, or the included")
    L.append("// node), type (0 = not given, 1 = 1, 2 = 1C, 3 = 2, 4 = 2C, 5 = 3, 6 = Include, 7 = conditional Include).")
    L.append("")
    L.append("constexpr ModAttr kModAttrs[] = {")
    for tag, mask, inf in attrs:
        kw, vr, vm, retired = inf if inf else ("", "", "", False)
        L.append("    {0x%08Xu, 0x%08Xu, %s, %s, %s, %s}," % (tag, mask, c_str(kw), c_str(vr), c_str(vm),
                                                         "true" if retired else "false"))
    L.append("};")
    L.append("")
    L.append("constexpr ModEntry kModEntries[] = {")
    for a, c, t in entries:
        L.append("    {%d, %d, %d}," % (a, c, t))
    L.append("};")
    L.append("")
    L.append("constexpr ModNode kModNodes[] = {")
    for n, first, count in node_rows:
        L.append("    {%s, %s, %s, %d, %du, %du}," % (c_str(n.name), c_str(n.table_id), c_str(n.cs or ""), n.kind, first, count))
    L.append("};")
    return "\n".join(L) + "\n", len(entries), len(attrs)


# ---------------------------------------------------------------- main

def main():
    args = [a for a in sys.argv[1:]]
    check = "--check" in args
    if check:
        args.remove("--check")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if "--out-root" in args:
        i = args.index("--out-root")
        root = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    p3 = ET.parse(args[0]).getroot()
    p6 = ET.parse(args[1]).getroot()
    m = Model(p3, Dict(dictionary(p6)))
    m.finish()
    cs = CSharp(m)
    cpp_text, n_entries, n_attrs = render_cpp(m)
    cs_text = cs.render()
    outputs = [(os.path.join(root, "src", "modules", "module_tables.inc"), cpp_text),
               (os.path.join(root, "bindings", "csharp", "src", "Siaara", "Generated", "Modules.g.cs"), cs_text)]
    bad = 0
    for path, txt in outputs:
        if check:
            try:
                with open(path, "r", encoding="utf-8", newline="") as f:
                    if f.read() != txt:
                        print("%s differs from tools/gen_modules.py output" % path)
                        bad = 1
            except OSError:
                print("%s missing" % path)
                bad = 1
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(txt)
    nm = sum(1 for n in m.nodes if n.kind == KIND_MODULE)
    nma = sum(1 for n in m.nodes if n.kind == KIND_MACRO)
    nit = sum(1 for n in m.nodes if n.kind == KIND_ITEM)
    print("modules %d, macros %d, other tables %d, item nodes %d, entries %d, distinct attributes %d, "
          "C# classes %d, C# properties %d, unresolved includes %d, functional-group includes %d, "
          "attributes not in PS3.6 %d" % (
              nm, nma, len(m.nodes) - nm - nma - nit - 1, nit, n_entries, n_attrs, cs.stats["classes"],
              cs.stats["properties"], m.stats["unresolved_includes"], m.stats["fg_includes"], cs.stats["skipped_unknown"]))
    if check:
        print("generated files %s" % ("differ" if bad else "match"))
    return bad


if __name__ == "__main__":
    sys.exit(main())
