#!/usr/bin/env python3
"""Generates the C# interop layer of the SIAARA bindings from include/siaara/siaara.h.

Outputs (all checked in; regenerate after any change to the C API or the dictionary):
  bindings/csharp/src/Siaara.Interop/Generated/Native.g.cs
      constants, blittable structs, [LibraryImport] declarations (class Raw, C-faithful pointer
      signatures) and safe overloads (class Native: spans, ref/in/out, nint handles).
  bindings/csharp/src/Siaara/Generated/DicomTags.g.cs
      DicomTag constants for every non-retired dictionary entry (src/dict/dictionary_data.inc).
  bindings/csharp/src/Siaara/Generated/DicomUids.g.cs
      Transfer Syntax and SOP Class UID constants (non-retired) from the same file.
  bindings/csharp/tests/native/struct_layout.c
      prints sizeof / offsetof of every struct in siaara.h (test-only CMake target).
  bindings/csharp/tests/Siaara.Tests/Generated/StructLayouts.g.cs
      the same numbers measured on the C# structs.

Usage:  python3 tools/gen_csharp_interop.py           (write the files)
        python3 tools/gen_csharp_interop.py --check   (exit 1 if any checked-in file differs)

Python standard library only.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "siaara", "siaara.h")
DICT = os.path.join(ROOT, "src", "dict", "dictionary_data.inc")
OUT_NATIVE = os.path.join(ROOT, "bindings", "csharp", "src", "Siaara.Interop", "Generated", "Native.g.cs")
OUT_TAGS = os.path.join(ROOT, "bindings", "csharp", "src", "Siaara", "Generated", "DicomTags.g.cs")
OUT_UIDS = os.path.join(ROOT, "bindings", "csharp", "src", "Siaara", "Generated", "DicomUids.g.cs")
OUT_LAYOUT_C = os.path.join(ROOT, "bindings", "csharp", "tests", "native", "struct_layout.c")
OUT_LAYOUT_CS = os.path.join(ROOT, "bindings", "csharp", "tests", "Siaara.Tests", "Generated", "StructLayouts.g.cs")

SKIPPED = []  # functions without a generated safe overload
BANNER = "GENERATED FILE - do not edit. Produced by tools/gen_csharp_interop.py"

SCALARS = {
    "uint32_t": "uint", "int32_t": "int", "uint16_t": "ushort", "int16_t": "short", "uint8_t": "byte",
    "int8_t": "sbyte", "uint64_t": "ulong", "int64_t": "long", "size_t": "nuint", "double": "double",
    "float": "float", "char": "byte", "siaara_status": "int", "void": "void",
}

# Arrays whose element count is another parameter that is not adjacent (or not a size_t), and
# arrays that share one capacity. Each entry: function -> {param: rule}.
#   ("count", name, factor, optional): span.Length must be >= factor * name (empty allowed if optional)
#   ("cap_primary",): this span supplies the cap argument
#   ("cap_secondary",): empty (NULL) or at least cap elements
ARRAY_RULES = {
    "siaara_volume_build": {"utf8_paths": ("count", "count", 1, False), "path_lens": ("count", "count", 1, True)},
    "siaara_dicomdir_write": {"files": ("count", "count", 1, False), "lens": ("count", "count", 1, False)},
    "siaara_view_set_curve": {"points": ("count", "count", 3, False)},
    "siaara_series_sort": {"out_instance": ("cap_primary",), "out_frame": ("cap_secondary",),
                           "out_group": ("cap_secondary",)},
}
LEN_NAME = re.compile(r"(^|_)(len|size|count)$")
CS_KEYWORDS = {"out", "params", "ref", "in", "base", "object", "string", "event", "fixed", "checked", "lock"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def ident(name):
    return "@" + name if name in CS_KEYWORDS else name


class CType:
    """A parsed C type: base name, const on the base, pointer depth, array length."""

    def __init__(self, base, const, ptr, array=None, inner_const=False):
        self.base, self.const, self.ptr, self.array, self.inner_const = base, const, ptr, array, inner_const

    def __repr__(self):
        return f"CType({self.base!r}, const={self.const}, ptr={self.ptr}, array={self.array})"


def parse_decl(decl):
    """'const siaara_file* const* files' -> (CType, 'files')."""
    decl = decl.strip()
    m = re.match(r"^(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*(\[\s*([A-Za-z0-9_]+)\s*\])?$", decl)
    if not m:
        raise ValueError("cannot parse declaration: " + decl)
    type_text, name, array = m.group(1), m.group(2), m.group(4)
    toks = re.findall(r"[A-Za-z_][A-Za-z0-9_]*|\*", type_text)
    const = False
    base = None
    ptr = 0
    inner_const = False
    for t in toks:
        if t == "const":
            if base is None:
                const = True
            else:
                inner_const = True
        elif t == "*":
            ptr += 1
        elif t == "struct":
            continue
        else:
            base = t
    return CType(base, const, ptr, array, inner_const), name


class Model:
    def __init__(self, text):
        self.raw = text
        src = strip_comments(text)
        self.defines = []      # (name, value text)
        self.enums = []        # (name, value text)
        self.opaque = []       # handle type names
        self.callbacks = {}    # name -> (ret CType, [(CType, name)])
        self.structs = []      # (name, [(CType, field)])
        self.functions = []    # (name, ret CType, [(CType, name)])
        skip_defines = {"SIAARA_SIAARA_H", "SIAARA_CALL", "SIAARA_API", "SIAARA_NOEXCEPT"}
        for m in re.finditer(r"^\s*#\s*define\s+(SIAARA_[A-Z0-9_]+)[ \t]+([^\n]+)$", src, flags=re.M):
            if m.group(1) not in skip_defines and "__" not in m.group(2):
                self.defines.append((m.group(1), m.group(2).strip()))
        for m in re.finditer(r"\benum\s*\{(.*?)\}\s*;", src, flags=re.S):
            for item in m.group(1).split(","):
                item = item.strip()
                if not item:
                    continue
                n, v = [s.strip() for s in item.split("=", 1)]
                self.enums.append((n, v))
        for m in re.finditer(r"typedef\s+struct\s+(\w+)\s+(\w+)\s*;", src):
            self.opaque.append(m.group(2))
        for m in re.finditer(r"typedef\s+([\w\s\*]+?)\s*\(\s*SIAARA_CALL\s*\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;", src, flags=re.S):
            ret, _ = parse_decl(m.group(1) + " _r")
            self.callbacks[m.group(2)] = (ret, self.parse_params(m.group(3)))
        for m in re.finditer(r"typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;", src, flags=re.S):
            fields = []
            for stmt in m.group(2).split(";"):
                stmt = stmt.strip()
                if not stmt:
                    continue
                first, *rest = [s.strip() for s in stmt.split(",")]
                ct, fname = parse_decl(first)
                fields.append((ct, fname))
                for r in rest:  # 'double sum, mean' : further declarators share the base type
                    ct2, n2 = parse_decl(("const " if ct.const else "") + ct.base + " " + "*" * ct.ptr + r)
                    fields.append((ct2, n2))
            self.structs.append((m.group(3), fields))
        body = src[src.index('extern "C"'):]
        for m in re.finditer(r"SIAARA_API\s+([\w\s\*]+?)\s*SIAARA_CALL\s+(\w+)\s*\((.*?)\)\s*(?:SIAARA_NOEXCEPT\s*)?;", body, flags=re.S):
            ret, _ = parse_decl(m.group(1) + " _r")
            self.functions.append((m.group(2), ret, self.parse_params(m.group(3))))
        self.struct_names = {s for s, _ in self.structs}
        self.const_values = {}
        for n, v in self.defines + self.enums:
            self.const_values[n] = v

    @staticmethod
    def parse_params(text):
        text = " ".join(text.split())
        if text in ("", "void"):
            return []
        return [parse_decl(p) for p in text.split(",")]

    def array_len(self, a):
        if a.isdigit():
            return int(a)
        return int(self.const_values[a].rstrip("uU"))


# ----------------------------------------------------------------------------------- C# types
def is_handle(model, base):
    return base in model.opaque


def raw_type(model, ct, in_struct=False):
    """C-faithful C# type for the Raw layer (pointers kept); struct fields use nint for pointers."""
    base = ct.base
    if base in model.callbacks:
        if in_struct:
            return "nint"
        return callback_fnptr(model, base)
    if in_struct and ct.ptr > 0:
        return "nint"
    if is_handle(model, base):
        if ct.ptr == 1:
            return "nint"
        return "nint" + "*" * (ct.ptr - 1)
    cs = SCALARS.get(base) or (base if base in model.struct_names else None)
    if cs is None:
        raise ValueError("unknown C type " + base)
    if ct.array and not in_struct:
        return cs + "*"
    return cs + "*" * ct.ptr


def callback_fnptr(model, name):
    ret, params = model.callbacks[name]
    args = [raw_type(model, p) for p, _ in params] + [raw_type(model, ret)]
    return "delegate* unmanaged[Cdecl]<" + ", ".join(args) + ">"


def literal(model, value):
    v = value.strip()
    uns = bool(re.search(r"\d[uU]\b", v)) or bool(re.match(r"^0x[0-9A-Fa-f]{8}u?$", v))
    v = re.sub(r"(\d)[uU]\b", r"\1u", v)
    return ("uint" if uns else "int"), v


# ----------------------------------------------------------------------------------- Native.g.cs
def gen_native(model):
    o = []
    w = o.append
    w("// <auto-generated/>")
    w("// " + BANNER + " from include/siaara/siaara.h.")
    w("#nullable enable")
    w("using System;")
    w("using System.Runtime.CompilerServices;")
    w("using System.Runtime.InteropServices;")
    w("")
    w("namespace Siaara.Interop;")
    w("")
    w("/// <summary>Constants of siaara.h (enumerators and macros).</summary>")
    w("internal static class C")
    w("{")
    for n, v in model.defines + model.enums:
        t, lit = literal(model, v)
        w(f"    public const {t} {n} = {lit};")
    w("}")
    w("")
    # inline arrays
    arrays = set()
    for sname, fields in model.structs:
        for ct, _ in fields:
            if ct.array:
                arrays.add((SCALARS[ct.base], model.array_len(ct.array)))
    for t, n in sorted(arrays):
        w(f"[InlineArray({n})]")
        w(f"internal struct {inline_name(t, n)}")
        w("{")
        w(f"    private {t} _element0;")
        w("}")
        w("")
    for sname, fields in model.structs:
        w(f"/// <summary>{sname} (blittable, C layout).</summary>")
        w("[StructLayout(LayoutKind.Sequential)]")
        w(f"internal struct {sname}")
        w("{")
        for ct, fname in fields:
            if ct.array:
                t = inline_name(SCALARS[ct.base], model.array_len(ct.array))
            else:
                t = raw_type(model, ct, in_struct=True)
            w(f"    public {t} {ident(fname)};")
        w("}")
        w("")
    # Raw layer
    w("/// <summary>[LibraryImport] declarations with the C signatures (pointers as pointers).</summary>")
    w("internal static unsafe partial class Raw")
    w("{")
    w("    internal const string Library = \"siaara\";")
    w("")
    w("    static Raw() => NativeLibraryLoader.EnsureRegistered();")
    w("")
    for fname, ret, params in model.functions:
        args = ", ".join(f"{raw_type(model, ct)} {ident(n)}" for ct, n in params)
        w(f"    [LibraryImport(Library, EntryPoint = \"{fname}\")]")
        w("    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]")
        rt = raw_type(model, ret)
        w(f"    internal static partial {rt} {fname}({args});")
        w("")
    w("}")
    w("")
    w("/// <summary>Safe overloads of every C function: spans instead of pointer + length, ref/in/out")
    w("/// instead of struct and scalar pointers, nint for handles. An empty span passes NULL; a")
    w("/// NUL-terminated string argument must contain a NUL byte. Pass")
    w("/// <c>in Unsafe.NullRef&lt;T&gt;()</c> for an optional struct argument to pass NULL.</summary>")
    w("internal static unsafe class Native")
    w("{")
    for fname, ret, params in model.functions:
        # A parameter pattern the rules below do not know (a future C API addition) must not
        # break generation: the Raw declaration is still emitted (its types are exact), only the
        # safe overload is left out and reported, so a hand-written wrapper can call Raw.
        lines = []
        try:
            gen_safe(model, lines.append, fname, ret, params)
        except ValueError as e:
            SKIPPED.append(f"{fname}: {e}")
            lines = [f"    // {fname}: no safe overload generated ({e}); call Raw.{fname}.", ""]
        for line in lines:
            w(line)
    w("}")
    return "\n".join(o) + "\n"


def inline_name(t, n):
    return f"FixedArray{n}Of{t[0].upper()}{t[1:]}"


def gen_safe(model, w, fname, ret, params):
    rules = ARRAY_RULES.get(fname, {})
    sig = []
    pre = []      # validation / assignment statements
    fixed = []    # fixed statements
    call = []
    collapsed = set()
    has_cap = any(n == "cap" and ct.base == "size_t" and ct.ptr == 0 for ct, n in params)
    cap_from = None
    for i, (ct, n) in enumerate(params):
        if n in collapsed:
            continue
        nm = ident(n)
        nxt = params[i + 1] if i + 1 < len(params) else None
        base = ct.base
        if base in model.callbacks:
            sig.append(f"nint {nm}")
            call.append(f"({callback_fnptr(model, base)}){nm}")
            continue
        if ct.ptr == 0 and not ct.array:
            if n == "cap" and has_cap and cap_from:
                call.append(f"(nuint){cap_from}.Length")
                continue
            sig.append(f"{raw_type(model, ct)} {nm}")
            call.append(nm)
            continue
        if base == "void" and ct.ptr == 1:
            sig.append(f"nint {nm}")
            call.append(f"(void*){nm}")
            continue
        # handles
        if is_handle(model, base) and ct.ptr == 1:
            sig.append(f"nint {nm}")
            call.append(nm)
            continue
        if is_handle(model, base) and ct.ptr == 2 and not ct.const:
            sig.append(f"out nint {nm}")
            pre.append(f"{nm} = 0;")
            fixed.append(f"fixed (nint* p_{n} = &{nm})")
            call.append(f"p_{n}")
            continue
        elem = "nint" if (is_handle(model, base) or (ct.ptr == 2 and base == "char")) else SCALARS.get(base)
        if base in model.struct_names:
            if ct.ptr != 1:
                raise ValueError("struct pointer depth")
            if not ct.const and has_cap and n == "buf":
                sig.append(f"Span<{base}> {nm}")
                cap_from = nm
                fixed.append(f"fixed ({base}* p_{n} = {nm})")
                call.append(f"p_{n}")
                continue
            if (ct.const and nxt and nxt[0].base == "size_t" and nxt[0].ptr == 0 and LEN_NAME.search(nxt[1])
                    and n not in rules):
                sig.append(f"ReadOnlySpan<{base}> {nm}")
                fixed.append(f"fixed ({base}* p_{n} = {nm})")
                call.append(f"p_{n}")
                call.append(f"(nuint){nm}.Length")
                collapsed.add(nxt[1])
                continue
            mod = "in" if ct.const else "ref"
            sig.append(f"{mod} {base} {nm}")
            fixed.append(f"fixed ({base}* p_{n} = &{nm})")
            call.append(f"p_{n}")
            continue
        # out pointers to pointers: const uint8_t** / const void**
        if ct.ptr == 2 and base in ("uint8_t", "void") and not ct.inner_const:
            sig.append(f"out nint {nm}")
            pre.append(f"{nm} = 0;")
            fixed.append(f"fixed (nint* p_{n} = &{nm})")
            call.append(f"({raw_type(model, ct)})p_{n}")
            continue
        # arrays of pointers (const T* const*)
        is_ptr_array = ct.ptr == 2 and ct.inner_const
        if ct.const or ct.array and ct.const or is_ptr_array:
            # input array / string
            span_t = f"ReadOnlySpan<{elem}>"
            rule = rules.get(n)
            if (nxt and nxt[0].base == "size_t" and nxt[0].ptr == 0 and LEN_NAME.search(nxt[1])
                    and not rule):
                sig.append(f"{span_t} {nm}")
                fixed.append(f"fixed ({elem}* p_{n} = {nm})")
                call.append(f"({raw_type(model, ct)})p_{n}")
                call.append(f"(nuint){nm}.Length")
                collapsed.add(nxt[1])
                continue
            sig.append(f"{span_t} {nm}")
            if ct.array:
                ln = model.array_len(ct.array)
                pre.append(f"if ({nm}.Length < {ln}) throw new ArgumentException(\"{n} needs {ln} elements\", nameof({nm}));")
            elif rule:
                add_rule(pre, nm, n, rule)
            elif base == "char" and ct.ptr == 1:
                pre.append(f"if (!{nm}.IsEmpty && {nm}.IndexOf((byte)0) < 0) "
                           f"throw new ArgumentException(\"{n} must be NUL-terminated\", nameof({nm}));")
            else:
                raise ValueError(f"{fname}: no rule for input array {n}")
            fixed.append(f"fixed ({elem}* p_{n} = {nm})")
            call.append(f"({raw_type(model, ct)})p_{n}")
            continue
        # non-const pointers: output arrays (with cap) or scalar outs
        rule = rules.get(n)
        if ct.array:
            ln = model.array_len(ct.array)
            sig.append(f"Span<{elem}> {nm}")
            pre.append(f"if ({nm}.Length < {ln}) throw new ArgumentException(\"{n} needs {ln} elements\", nameof({nm}));")
            fixed.append(f"fixed ({elem}* p_{n} = {nm})")
            call.append(f"p_{n}")
            continue
        if has_cap and (n == "buf" or rule):
            sig.append(f"Span<{elem}> {nm}")
            if rule and rule[0] == "cap_secondary":
                pre.append(f"if (!{nm}.IsEmpty && {nm}.Length < {cap_from}.Length) "
                           f"throw new ArgumentException(\"{n} is shorter than the first output array\", nameof({nm}));")
            else:
                cap_from = nm
            fixed.append(f"fixed ({elem}* p_{n} = {nm})")
            call.append(f"({raw_type(model, ct)})p_{n}")
            continue
        if ct.ptr == 1 and base in SCALARS and base not in ("char", "void"):
            sig.append(f"out {elem} {nm}")
            pre.append(f"{nm} = default;")
            fixed.append(f"fixed ({elem}* p_{n} = &{nm})")
            call.append(f"p_{n}")
            continue
        raise ValueError(f"{fname}: unhandled parameter {n} {ct}")
    rt = raw_type(model, ret)
    if ret.ptr:
        rt = "nint"
    w(f"    internal static {rt} {fname}({', '.join(sig)})")
    w("    {")
    for p in pre:
        w("        " + p)
    for f in fixed:
        w("        " + f)
    expr = f"Raw.{fname}({', '.join(call)})"
    if ret.ptr:
        expr = f"(nint){expr}"
    ind = "            " if fixed else "        "
    if rt == "void":
        w(f"{ind}{expr};")
    else:
        w(f"{ind}return {expr};")
    w("    }")
    w("")


def add_rule(pre, nm, n, rule):
    if rule[0] == "count":
        _, cnt, factor, optional = rule
        need = f"(long){cnt}" if factor == 1 else f"{factor}L * {cnt}"
        cond = f"{nm}.Length < {need}"
        if optional:
            cond = f"!{nm}.IsEmpty && " + cond
        pre.append(f"if ({cond}) throw new ArgumentException(\"{n} has fewer elements than {cnt} requires\", nameof({nm}));")
    else:
        raise ValueError("bad rule")


# ----------------------------------------------------------------------------------- layout files
def c_layout(model):
    o = ["/* " + BANNER + ". */",
         "/* Prints sizeof and offsetof of every struct in siaara.h for the C# struct-layout test. */",
         "#include <stddef.h>", "#include <stdio.h>", "", "#include \"siaara/siaara.h\"", "",
         "int main(void) {"]
    for sname, fields in model.structs:
        o.append(f"    printf(\"S {sname} %u\\n\", (unsigned)sizeof({sname}));")
        for _, f in fields:
            o.append(f"    printf(\"F {sname} {f} %u\\n\", (unsigned)offsetof({sname}, {f}));")
    o.append("    return 0;")
    o.append("}")
    return "\n".join(o) + "\n"


def cs_layout(model):
    o = ["// <auto-generated/>", "// " + BANNER + ".", "using System.Collections.Generic;",
         "using System.Runtime.CompilerServices;", "using System.Runtime.InteropServices;", "using Siaara.Interop;", "",
         "namespace Siaara.Tests;", "",
         "/// <summary>Sizes and field offsets of the interop structs, measured in C#.</summary>",
         "internal static class StructLayouts", "{",
         "    private static long Offset<TS, TF>(ref TS s, ref TF f) =>",
         "        (long)Unsafe.ByteOffset(ref Unsafe.As<TS, byte>(ref s), ref Unsafe.As<TF, byte>(ref f));", "",
         "    /// <summary>Lines in the format of struct_layout.c: \"S name size\" and \"F name field offset\".</summary>",
         "    internal static List<string> Measure()", "    {", "        var r = new List<string>();"]
    for sname, fields in model.structs:
        o.append("        {")
        o.append(f"            var s = default({sname});")
        o.append(f"            r.Add(\"S {sname} \" + Unsafe.SizeOf<{sname}>().ToString(System.Globalization.CultureInfo.InvariantCulture));")
        for _, f in fields:
            o.append(f"            r.Add(\"F {sname} {f} \" + Offset(ref s, ref s.{ident(f)}).ToString(System.Globalization.CultureInfo.InvariantCulture));")
        o.append("        }")
    o.append("        return r;")
    o.append("    }")
    o.append("")
    o.append("    /// <summary>Marshal.SizeOf of every struct (\"name size\").</summary>")
    o.append("    internal static List<string> MarshalSizes()")
    o.append("    {")
    o.append("        var r = new List<string>();")
    for sname, _ in model.structs:
        o.append(f"        r.Add(\"{sname} \" + Marshal.SizeOf<{sname}>().ToString(System.Globalization.CultureInfo.InvariantCulture));")
    o.append("        return r;")
    o.append("    }")
    o.append("}")
    return "\n".join(o) + "\n"


# ----------------------------------------------------------------------------------- dictionary
def pascal_ok(name):
    return re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name) is not None


def gen_tags(dict_text):
    entries = []
    m = re.search(r"kEntries\[\]\s*=\s*\{(.*?)\n\};", dict_text, flags=re.S)
    for e in re.finditer(r'Entry\{0x([0-9A-Fa-f]{8})u, 0xFFFFFFFFu, VR::(\w+), VR::(\w+), "([^"]*)", "((?:[^"\\]|\\.)*)", "([^"]*)", (true|false)\}', m.group(1)):
        tag, vr, vr2, kw, name, vm, retired = e.groups()
        if retired == "true" or not kw or not pascal_ok(kw):
            continue
        entries.append((int(tag, 16), vr, vr2, kw, name.replace('\\"', '"'), vm))
    o = ["// <auto-generated/>", "// " + BANNER + " from src/dict/dictionary_data.inc.", "",
         "namespace Siaara;", "",
         "/// <summary>Tags of the non-retired attributes of the DICOM data dictionary (PS3.6, PS3.7).</summary>",
         "public static class DicomTags", "{"]
    for tag, vr, vr2, kw, name, vm in entries:
        vrs = vr if vr2 == "None" else f"{vr} or {vr2}"
        o.append(f"    /// <summary>({tag >> 16:04X},{tag & 0xFFFF:04X}) {xml(name)}, VR {vrs}, VM {vm}.</summary>")
        o.append(f"    public static readonly DicomTag {kw} = new(0x{tag:08X}u);")
    o.append("}")
    return "\n".join(o) + "\n", len(entries)


def gen_uids(dict_text):
    m = re.search(r"kUids\[\]\s*=\s*\{(.*?)\n\};", dict_text, flags=re.S)
    o = ["// <auto-generated/>", "// " + BANNER + " from src/dict/dictionary_data.inc.", "",
         "namespace Siaara;", "",
         "/// <summary>Transfer Syntax UIDs (PS3.6 Table A-1, non-retired).</summary>",
         "public static class DicomTransferSyntaxes", "{"]
    sop = ["/// <summary>SOP Class UIDs (PS3.6 Table A-1, non-retired).</summary>",
           "public static class DicomSopClasses", "{"]
    seen_ts, seen_sop = set(), set()
    for e in re.finditer(r'UidEntry\{"([0-9.]+)", "((?:[^"\\]|\\.)*)", "([^"]*)", "([^"]*)", (true|false)\}', m.group(1)):
        uid, name, kw, kind, retired = e.groups()
        if retired == "true" or not kw or not pascal_ok(kw):
            continue
        line = [f"    /// <summary>{xml(name)} ({uid}).</summary>", f"    public const string {kw} = \"{uid}\";"]
        if kind == "Transfer Syntax" and kw not in seen_ts:
            seen_ts.add(kw)
            o += line
        elif kind == "SOP Class" and kw not in seen_sop:
            seen_sop.add(kw)
            sop += line
    o.append("}")
    o.append("")
    sop.append("}")
    return "\n".join(o + sop) + "\n"


def xml(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ----------------------------------------------------------------------------------- main
def outputs():
    with open(HEADER, encoding="utf-8") as f:
        model = Model(f.read())
    with open(DICT, encoding="utf-8") as f:
        dict_text = f.read()
    tags, _ = gen_tags(dict_text)
    return {
        OUT_NATIVE: gen_native(model),
        OUT_TAGS: tags,
        OUT_UIDS: gen_uids(dict_text),
        OUT_LAYOUT_C: c_layout(model),
        OUT_LAYOUT_CS: cs_layout(model),
    }, model


def main(argv):
    files, model = outputs()
    if "--check" in argv:
        bad = []
        for path, text in files.items():
            try:
                with open(path, encoding="utf-8", newline="") as f:
                    cur = f.read()
            except OSError:
                cur = None
            if cur != text:
                bad.append(os.path.relpath(path, ROOT))
        if bad:
            print("out of date (run tools/gen_csharp_interop.py): " + ", ".join(bad))
            return 1
        print(f"up to date: {len(model.functions)} functions, {len(model.structs)} structs")
        return 0
    for path, text in files.items():
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print(f"generated: {len(model.functions)} functions, {len(model.structs)} structs, "
          f"{len(model.callbacks)} callbacks, {len(model.opaque)} handle types")
    for msg in SKIPPED:
        print("warning: " + msg, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
