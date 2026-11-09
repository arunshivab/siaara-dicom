#!/usr/bin/env python3
"""Cross-checks SIAARA de-identification (tests/crosscheck/deid_tool) on pydicom's test files.

Oracles (test-only, never used by the library):
  1. PS3.15 expectations checked with pydicom on SIAARA's output, against Table E.1-1 read
     straight from part15.xml by the small parser below (independent of tools/gen_deid.py):
     X attributes absent, Z attributes empty, D/U attributes changed, UIDs 2.25.<decimal> and
     consistent across the whole file set, no private attributes, Patient Identity Removed YES,
     the De-identification Method Code Sequence, and the file meta matching the data set.
  2. DCMTK's dcmdump (if installed) must read every SIAARA output without error.
  3. The `deid` package (pydicom/deid) with its default recipe: per top-level attribute, which
     tool removes, changes or keeps it. The default recipe is a site recipe, not PS3.15, so
     differences are expected; they are listed by category with the reason.

Usage: deid_crosscheck.py <deid_tool> <part15.xml> [--verbose]
Exit status 1 if any PS3.15 expectation fails.
"""
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile
import warnings
import xml.etree.ElementTree as ET

warnings.filterwarnings("ignore")
import pydicom  # noqa: E402
from pydicom.data import get_testdata_files  # noqa: E402

DB = "{http://docbook.org/ns/docbook}"
XML_ID = "{http://www.w3.org/XML/1998/namespace}id"
CONFORMANT = {"X/Z": "Z", "X/D": "D", "X/Z/D": "D", "Z/D": "D", "X/Z/U*": "U"}


def load_table(path):
    """{(value, mask): action} from Table E.1-1, Basic Profile column."""
    root = ET.parse(path).getroot()
    for tbl in root.iter(DB + "table"):
        if tbl.get(XML_ID) != "table_E.1-1":
            continue
        rules = {}
        for tr in tbl.find(DB + "tbody").iter(DB + "tr"):
            cells = [re.sub(r"\s+", " ", "".join(td.itertext()).replace("​", "")).strip()
                     for td in tr.findall(DB + "td")]
            m = re.fullmatch(r"\(([0-9A-Fx]{4}),([0-9A-Fx]{4})\)", cells[1])
            if not m:
                continue
            raw = (m.group(1) + m.group(2)).upper()
            value = int(raw.replace("X", "0"), 16)
            mask = int("".join("0" if c == "X" else "F" for c in raw), 16)
            rules[(value, mask)] = cells[4]
        return rules
    raise SystemExit("Table E.1-1 not found")


def action_for(rules, tag):
    if (tag, 0xFFFFFFFF) in rules:
        return rules[(tag, 0xFFFFFFFF)]
    for (value, mask), act in rules.items():
        if mask != 0xFFFFFFFF and (tag & mask) == value:
            return act
    return None


def top_level(ds):
    return {int(e.tag): e for e in ds}


def value_text(e):
    if e.VR == "SQ":
        return f"<{len(e.value)} items>"
    v = e.value
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    if isinstance(v, (list, pydicom.multival.MultiValue)):
        return "\\".join(str(x) for x in v)
    return "" if v is None else str(v)


def walk_uids(ds, out):
    for e in ds:
        if e.VR == "SQ":
            for item in e.value:
                walk_uids(item, out)
        elif e.VR == "UI" and e.value:
            vals = e.value if isinstance(e.value, (list, pydicom.multival.MultiValue)) else [e.value]
            for v in vals:
                out.append((int(e.tag), str(v)))


def has_private(ds):
    for e in ds:
        if e.tag.is_private:
            return True
        if e.VR == "SQ" and any(has_private(i) for i in e.value):
            return True
    return False


def run_siaara(tool, src, dst):
    r = subprocess.run([tool, src, dst], capture_output=True, text=True)
    return r.returncode == 0, (r.stderr or r.stdout).strip()


def run_deid_package(src, outdir):
    from deid.config import DeidRecipe
    from deid.dicom import replace_identifiers
    recipe = DeidRecipe()  # default recipe (deid.dicom)
    res = replace_identifiers(dicom_files=[src], deid=recipe, save=True, output_folder=outdir,
                              overwrite=True, remove_private=True, strip_sequences=False)
    return res[0] if res else None


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    tool, part15 = argv[1], argv[2]
    verbose = "--verbose" in argv
    rules = load_table(part15)
    files = sorted(set(get_testdata_files("*.dcm")))
    ps315_failures = []
    uid_map = collections.defaultdict(set)   # original UID -> replacements seen
    stats = collections.Counter()
    disagreements = collections.defaultdict(collections.Counter)  # category -> keyword counts
    deid_available = True
    try:
        import deid  # noqa: F401
    except ImportError:
        deid_available = False

    with tempfile.TemporaryDirectory() as tmp:
        for n, src in enumerate(files):
            try:
                orig = pydicom.dcmread(src)
            except Exception:
                stats["unreadable by pydicom"] += 1
                continue
            if getattr(orig, "file_meta", None) is not None and \
                    str(orig.file_meta.get("MediaStorageSOPClassUID", "")) == "1.2.840.10008.1.3.10":
                stats["DICOMDIR skipped"] += 1
                continue
            dst = os.path.join(tmp, f"s{n}.dcm")
            ok, msg = run_siaara(tool, src, dst)
            if not ok:
                stats["siaara refused"] += 1
                if verbose:
                    print(f"  {os.path.basename(src)}: siaara refused: {msg}")
                continue
            stats["files compared"] += 1
            if shutil.which("dcmdump"):
                r = subprocess.run(["dcmdump", "-q", dst], capture_output=True, text=True)
                if r.returncode != 0 or "E: " in r.stderr:
                    ps315_failures.append(f"{os.path.basename(src)}: dcmdump cannot read the output")
                else:
                    stats["outputs read by dcmdump"] += 1
            out = pydicom.dcmread(dst)
            o_top, s_top = top_level(orig), top_level(out)
            name = os.path.basename(src)

            # ---- oracle 1: PS3.15 expectations
            def fail(why):
                ps315_failures.append(f"{name}: {why}")
            for tag, e in o_top.items():
                act = action_for(rules, tag)
                if act is None or (tag >> 16) in (0x0002, 0x0004):
                    continue
                act = CONFORMANT.get(act, act)
                got = s_top.get(tag)
                if act == "X" and got is not None:
                    fail(f"({tag >> 16:04X},{tag & 0xFFFF:04X}) X but present")
                elif act == "Z" and got is not None and got.VR != "SQ" and got.value not in (None, "", b""):
                    fail(f"({tag >> 16:04X},{tag & 0xFFFF:04X}) Z but has a value")
                elif act in ("D", "U") and got is None:
                    fail(f"({tag >> 16:04X},{tag & 0xFFFF:04X}) {act} but removed")
                elif act == "D" and got is not None and got.VR not in ("SQ",) and e.value not in (None, "", b"") \
                        and value_text(got) == value_text(e) and value_text(e) != "DEIDENTIFIED":
                    fail(f"({tag >> 16:04X},{tag & 0xFFFF:04X}) D but unchanged")
            # Identifying strings of the original (PN values anywhere, IDs, institution) must not
            # appear anywhere in the written bytes.
            with open(dst, "rb") as fh:
                out_bytes = fh.read()
            phi = set()

            def collect(ds):
                for el in ds:
                    if el.VR == "SQ":
                        for it in el.value:
                            collect(it)
                    elif el.VR == "PN" or el.keyword in ("PatientID", "OtherPatientIDs", "InstitutionName",
                                                          "InstitutionAddress", "AccessionNumber",
                                                          "PatientAddress", "StationName"):
                        vals = el.value if isinstance(el.value, (list, pydicom.multival.MultiValue)) else [el.value]
                        for v in vals:
                            v = str(v).strip()
                            if len(v) >= 5 and v not in ("ANONYMOUS", "DEIDENTIFIED"):
                                phi.add(v)
            collect(orig)
            # Values that are also the value of an attribute Table E.1-1 does not list (e.g. an
            # Institution Name "TOSHIBA" inside the Manufacturer "TOSHIBA_MEC") legitimately stay.
            kept_values = {str(el.value).strip() for el in orig
                           if el.VR != "SQ" and action_for(rules, int(el.tag)) is None and not el.tag.is_private}
            shared = {v for v in phi if any(v in k for k in kept_values)}
            stats["identifying strings contained in a kept attribute (skipped)"] += len(shared)
            phi -= shared
            for v in sorted(phi):
                if v.encode("latin-1", "ignore") in out_bytes:
                    fail(f"identifying value of length {len(v)} found in the output bytes")
            stats["identifying strings checked"] += len(phi)
            if has_private(out):
                fail("private attributes remain")
            if str(out.get("PatientIdentityRemoved", "")) != "YES":
                fail("Patient Identity Removed is not YES")
            codes = [str(i.CodeValue) for i in out.get("DeidentificationMethodCodeSequence", [])]
            if "113100" not in codes:
                fail("113100 missing from De-identification Method Code Sequence")
            if "SOPInstanceUID" in out and \
                    str(out.file_meta.get("MediaStorageSOPInstanceUID", "")) != str(out.SOPInstanceUID):
                fail("Media Storage SOP Instance UID does not match SOP Instance UID")
            for k in ("SourceApplicationEntityTitle", "SendingApplicationEntityTitle", "PrivateInformation"):
                if k in out.file_meta:
                    fail(f"file meta still has {k}")
            o_uids, s_uids = [], []
            walk_uids(orig, o_uids)
            walk_uids(out, s_uids)
            replaced = {u for _, u in s_uids}
            for tag, u in o_uids:
                if u.startswith("1.2.840.10008."):
                    continue
                if u in replaced and not u.startswith("2.25."):
                    fail(f"original UID survives in ({tag >> 16:04X},{tag & 0xFFFF:04X})")
            if "StudyInstanceUID" in orig and "StudyInstanceUID" in out:
                uid_map[str(orig.StudyInstanceUID)].add(str(out.StudyInstanceUID))
            if "FrameOfReferenceUID" in orig and "FrameOfReferenceUID" in out:
                uid_map[str(orig.FrameOfReferenceUID)].add(str(out.FrameOfReferenceUID))
            for _, u in s_uids:
                if u.startswith("2.25.") and (len(u) > 64 or not re.fullmatch(r"2\.25\.(0|[1-9][0-9]*)", u)):
                    fail(f"malformed replacement UID {u}")

            # ---- oracle 3: the deid package
            if not deid_available:
                continue
            ddir = os.path.join(tmp, f"d{n}")
            os.makedirs(ddir, exist_ok=True)
            try:
                dpath = run_deid_package(src, ddir)
                dout = pydicom.dcmread(dpath)
            except Exception as ex:  # the package fails on some files
                stats["deid package failed"] += 1
                if verbose:
                    print(f"  {name}: deid package failed: {type(ex).__name__}")
                continue
            stats["files compared with deid"] += 1
            d_top = top_level(dout)
            for tag, e in o_top.items():
                if pydicom.tag.Tag(tag).is_private or (tag >> 16) == 0x0002:
                    continue
                kw = pydicom.datadict.keyword_for_tag(tag) or f"({tag >> 16:04X},{tag & 0xFFFF:04X})"

                def fate(top):
                    g = top.get(tag)
                    if g is None:
                        return "removed"
                    v = value_text(g)
                    if v in ("", b"", "<0 items>"):
                        return "empty"
                    return "kept" if v == value_text(e) else "changed"
                s_f, d_f = fate(s_top), fate(d_top)
                if s_f == d_f or (value_text(e) in ("", b"") and {s_f, d_f} <= {"removed", "empty", "kept"}):
                    stats["attribute fates agree (or both carry no value)"] += 1
                    continue
                stats["attribute fates differ"] += 1
                act = action_for(rules, tag)
                if tag == 0x7FE00010 and str(orig.file_meta.get("TransferSyntaxUID", "")) == "1.2.840.10008.1.2.2":
                    disagreements["Pixel Data bytes differ (Explicit VR Big Endian, 32-bit OW: the core "
                                  "reader/writer swaps in 16-bit units; not a de-identification action)"][kw] += 1
                    continue
                if s_f in ("kept", "empty") and d_f == "removed":
                    why = "deid recipe pattern (e.g. endswith:ID/Date/Time, contains:UID) hits an attribute " \
                          "Table E.1-1 does not list" if act is None else f"Table E.1-1 lists it as {act}"
                    disagreements[f"deid removed, SIAARA {s_f} ({why})"][kw] += 1
                elif s_f == "kept" and d_f != "kept":
                    why = "deid recipe pattern (e.g. endswith:ID/Date/Time, contains:UID) hits an attribute " \
                          "Table E.1-1 does not list" if act is None else f"Table E.1-1 lists it as {act}"
                    disagreements[f"deid {d_f}, SIAARA kept ({why})"][kw] += 1
                elif d_f == "kept" and s_f != "kept":
                    why = f"Table E.1-1 action {act}" if act else "SIAARA extension: unlisted DA/DT/TM/PN or UI, or the rest of an overlay group whose " \
                          "Overlay Data is removed"
                    disagreements[f"SIAARA {s_f}, deid kept ({why})"][kw] += 1
                else:
                    why = f"Table E.1-1 action {act}" if act else "unlisted"
                    disagreements[f"SIAARA {s_f}, deid {d_f} ({why})"][kw] += 1

    for orig_uid, news in uid_map.items():
        if len(news) != 1:
            ps315_failures.append(f"UID mapped inconsistently across files: {len(news)} replacements")

    print("SIAARA de-identification cross-check on pydicom test files")
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}")
    print(f"  PS3.15 expectation failures: {len(ps315_failures)}")
    for f in ps315_failures[:50]:
        print(f"    {f}")
    if deid_available:
        print("  Differences from the deid package's default recipe (expected; reasons in brackets):")
        for cat in sorted(disagreements):
            kws = disagreements[cat]
            print(f"    {cat}: {sum(kws.values())}")
            print("      " + ", ".join(f"{k} x{c}" if c > 1 else k for k, c in kws.most_common(25)))
    else:
        print("  deid package not installed: comparison skipped")
    return 1 if ps315_failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
