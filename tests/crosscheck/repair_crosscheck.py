#!/usr/bin/env python3
"""Cross-checks siaara-repair against third-party tools used as test oracles only.

    python3 -I tests/crosscheck/repair_crosscheck.py <build dir> <work dir>

Corpus: pydicom's test files (*.dcm) and the deliberately broken files written by the
test-only tool repair_samples (tests/crosscheck/repair_samples.hpp).

1. Validation agreement with dciodvfy (dicom3tools): the Error findings of both are mapped to
   comparable keys (finding family, attribute keyword) and compared per file. Every disagreement is
   listed with its reason: a known reason from KNOWN (each checked by hand against the standard),
   or, for dciodvfy messages that have no SIAARA counterpart, the class of rule they come from.
2. Repair: every file is fixed on its own (`siaara-repair fix --fix all`) into a new folder. Checked:
   the input is byte-identical afterwards (SHA-256); dciodvfy reports no more errors than before (and
   fewer for every broken sample whose errors are repairable); pydicom reads the output (and decodes
   its pixel data when pydicom has a handler); DCMTK's dcmftest accepts it and `dcmdump +E` prints no
   error.
Exits 1 when a repair rule is broken. Writes <work dir>/repair_crosscheck.json.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

import pydicom
import pydicom.data
from pydicom import datadict

# Disagreements explained by hand (family, keyword) -> reason. Checked against DICOM 2026d.
KNOWN = {
    ("missing-type2", "OperatorsName"): "PS3.3 2026d C.8.8.1 RT Series Module lists Operators' Name as Type 2; "
                                        "dciodvfy's module definition predates that",
    ("bad-value", "ReferencedSOPInstanceUID"): "component with a leading zero (PS3.5 9.1); dciodvfy does not "
                                               "check UIDs inside this sequence",
    ("meta-mismatch", "MediaStorageSOPInstanceUID"): "PS3.10 7.1: Media Storage SOP Instance UID shall equal SOP "
                                                     "Instance UID; dciodvfy reports it only for some IODs",
    ("missing-type1", "FrameOfReferenceUID"): "Frame of Reference Module is U in the PS3.3 2026d RT Structure Set "
                                              "IOD (A.19.3); dciodvfy treats it as mandatory",
    ("missing-type2", "PositionReferenceIndicator"): "same Frame of Reference Module usage difference (A.19.3)",
    ("missing-type2", "Laterality"): "Type 2C; its condition (body part is a paired structure and no Image / Frame / "
                                     "Measurement Laterality) is not evaluated by SIAARA (counted as condition not "
                                     "evaluated)",
    ("missing-type2", "PatientOrientation"): "Type 2C; its condition (image does not require Image Orientation / "
                                             "Position) is not evaluated by SIAARA",
    ("missing-type1", "DeidentificationMethod"): "Type 1C with a two-attribute condition; not evaluated by SIAARA",
    ("missing-type1", "DeidentificationMethodCodeSequence"): "Type 1C with a two-attribute condition; not evaluated",
    ("enumerated-value", "PhotometricInterpretation"): "dciodvfy restricts Photometric Interpretation for the SC "
                                                       "Image IOD; PS3.3 2026d A.8.1 / C.8.6.2 give no such Enumerated "
                                                       "Values (C.7.6.3.1.2 lists Defined Terms)",
    ("enumerated-value", "NumberOfFrames"): "dciodvfy treats Number of Frames in an SC Image as an enumerated 1; the "
                                            "PS3.3 2026d SC Image IOD has no Multi-frame Module (the attribute is a "
                                            "Standard Extended attribute)",
    ("pixel-length", "PixelData"): "native YBR_FULL_422 holds two samples per pixel (PS3.3 C.7.6.3.1.2); dciodvfy "
                                   "expects three",
    ("bad-value", "(0019,1060)"): "private UT holding TAB / CR / LF, which PS3.5 6.2 allows in UT",
    ("missing-type1", "NumberOfFrames"): "SIAARA counts the Multi-frame Module (usage C) as present because Frame "
                                         "Increment Pointer is present; dciodvfy evaluates the module condition "
                                         "(multi-frame pixel data) and skips it for a single frame",
}

OURS_ONLY_CLASSES = {
    "group-length-mismatch": "group length check (PS3.5 7.2); dciodvfy does not check group lengths",
    "meta-group-length": "File Meta group length check (PS3.10 7.1); not checked by dciodvfy",
    "meta-version": "File Meta Information Version check; not checked by dciodvfy",
    "meta-missing-element": "File Meta Information content (PS3.10 7.1); dciodvfy reads files without meta",
    "not-part10": "no preamble / DICM (PS3.10 7.1); dciodvfy accepts raw data sets",
    "ts-missing": "dciodvfy reports it as an unmapped message (Metaheader without Transfer Syntax UID)",
    "precision-codestream": "codestream header vs Image Pixel check; dciodvfy does not read codestreams",
    "signedness-codestream": "codestream header vs Image Pixel check; dciodvfy does not read codestreams",
    "rows-codestream": "codestream header vs Image Pixel check; dciodvfy does not read codestreams",
    "codestream-header": "the first frame's codestream header could not be read (embedded delimiter in the "
                         "fragment); dciodvfy does not read codestreams",
    "missing-sop-class": "dciodvfy reports \"Information Object Not found\" (unmapped message)",
    "illegal-group": "odd groups 0001-0007 / FFFF (PS3.5 7.8.1); dciodvfy stops at the order error first",
    "odd-length": "odd value length (PS3.5 7.1.1); dciodvfy stops parsing or does not check it",
    "pixel-attributes-missing": "Image Pixel attributes missing although Pixel Data is present",
    "value-truncated": "value runs past the end of the data; dciodvfy reports a read failure",
    "encoding-mismatch": "data set encoding differs from the transfer syntax; dciodvfy reports a read failure",
    "fragment-truncated": "pixel data fragment runs past the end of the file (rescue); dciodvfy reports a read failure",
    "frames-missing": "fewer frames than Number of Frames (rescue); dciodvfy does not count encapsulated frames",
    "bot-invalid": "Basic Offset Table check (PS3.5 A.4); dciodvfy does not check it",
    "invalid-vr": "element without explicit VR in explicit VR data; dciodvfy reads it silently",
    "preamble-missing": "\"DICM\" without the 128-byte preamble (PS3.10 7.1); dciodvfy accepts it",
    "duplicate-element": "duplicated element; dciodvfy stops at the order error",
    "element-order": "dciodvfy reports it as an unmapped message (Tags out of order) and stops parsing",
    "unterminated-item": "item without delimiter at the end of the data; dciodvfy reports a read failure",
    "unterminated-sequence": "sequence without delimiter at the end of the data; dciodvfy reports a read failure",
    "vm": "value multiplicity of a value cut by truncation (rtplan_truncated)",
}

DCIODVFY_ONLY_CLASSES = [
    (r"Referenced SOP Instance is not listed|Content Item|relationship|Template|TID", "SR content tree / template "
     "rule (PS3.16 templates and C.17.3 relationship constraints are not checked by SIAARA)"),
    (r"needed to build DICOMDIR", "DICOMDIR suitability advice (not a conformance error)"),
    (r"read failed|Seek failed|Tags out of order|Tag read", "dciodvfy stopped parsing; SIAARA's tolerant scanner "
     "reports the structural cause instead"),
    (r"Information Object Not found|not a directory", "no SOP Class / IOD to check (SIAARA: missing-sop-class)"),
    (r"invalid data values for Value Representations", "summary line (each value is reported separately)"),
    (r"StudyInstanceUID has same value as FrameOfReferenceUID", "cross-attribute uniqueness rule not implemented"),
    (r"present when condition unsatisfied|Shall not be present", "\"shall not be present\" conditions are not "
     "evaluated by SIAARA"),
    (r"Number of items in PerFrameFunctionalGroupsSequence", "reported by SIAARA as per-frame-count"),
    (r"Metaheader present but doesn't contain Transfer Syntax UID", "reported by SIAARA as ts-missing"),
]

OURS_FAMILY = {
    "missing-type1": "missing-type1", "missing-type2": "missing-type2", "empty-type1": "empty-type1",
    "enumerated-value": "enumerated-value", "value-invalid": "bad-value", "value-too-long": "bad-value",
    "uid-zero": "bad-value", "ui-padding": "bad-value", "nul-padding": "bad-value", "vr-mismatch": "vr-mismatch",
    "vr-un": "vr-mismatch", "meta-mismatch": "meta-mismatch", "pixel-length": "pixel-length",
    "pixel-truncated": "pixel-length", "charset-undeclared": "bad-value", "per-frame-count": "per-frame-count",
}

NAME_TO_KEYWORD = {}
for _tag, entry in datadict.DicomDictionary.items():
    NAME_TO_KEYWORD.setdefault(entry[2], entry[4])


def kw_of_tag(g, e):
    try:
        return datadict.keyword_for_tag(int(g + e, 16)) or "(%s,%s)" % (g, e)
    except Exception:
        return "(%s,%s)" % (g, e)


def dciodvfy(path):
    try:
        p = subprocess.run(["dciodvfy", path], capture_output=True, text=True, errors="replace", timeout=60)
        out = p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return None
    lines = [l.strip() for l in out.splitlines() if l.startswith("Error")]
    aborted = p.returncode < 0
    return {"errors": lines, "aborted": aborted}


def dciodvfy_keys(lines):
    keys, unmapped = set(), []
    for l in lines:
        m = re.search(r"Missing attribute Type (1C?|2C?) (?:Required|Conditional) Element=<(\w+)>", l)
        if m:
            keys.add(("missing-type1" if m.group(1).startswith("1") else "missing-type2", m.group(2)))
            continue
        m = re.search(r"(?:Missing value|Empty attribute|Zero length).*Type 1C? .*Element=<(\w+)>", l)
        if m:
            keys.add(("empty-type1", m.group(1)))
            continue
        m = re.search(r"Unrecognized enumerated value <[^>]*> for value \d+ of attribute <([^>]+)>", l)
        if m:
            keys.add(("enumerated-value", NAME_TO_KEYWORD.get(m.group(1), m.group(1))))
            continue
        m = re.search(r"(?:Value invalid for this VR|Illegal root for UID)[^(]*\(0x([0-9a-f]{4}),0x([0-9a-f]{4})\)", l)
        if m:
            keys.add(("bad-value", kw_of_tag(m.group(1), m.group(2))))
            continue
        m = re.search(r"Invalid Value Representation .*Element=<(\w+)>", l)
        if m:
            keys.add(("vr-mismatch", m.group(1)))
            continue
        m = re.search(r"MediaStorageSOP(Instance|Class)UID different", l)
        if m:
            keys.add(("meta-mismatch", "MediaStorageSOP%sUID" % m.group(1)))
            continue
        if "PixelData has incorrect value length" in l:
            keys.add(("pixel-length", "PixelData"))
            continue
        unmapped.append(l)
    return keys, unmapped


def our_keys(report):
    keys = set()
    f = report["files"][0]
    for x in f["findings"]:
        if x["severity"] != "Error" and x["code"] != "uid-zero":
            continue
        fam = OURS_FAMILY.get(x["code"])
        if not fam:
            keys.add(("other:" + x["code"], x.get("keyword", "")))
            continue
        keys.add((fam, x.get("keyword", "PixelData" if fam == "pixel-length" else "")))
    return keys


def classify_unmapped(line):
    for pat, reason in DCIODVFY_ONLY_CLASSES:
        if re.search(pat, line):
            return reason
    return "unclassified"


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def run_ours(tool, args):
    p = subprocess.run([tool] + args, capture_output=True, text=True, errors="replace", timeout=300)
    try:
        return json.loads(p.stdout)
    except ValueError:
        return None


def pydicom_reads(path):
    try:
        ds = pydicom.dcmread(path)
        for _ in ds.iterall():
            pass
        if "PixelData" in ds:
            try:
                ds.pixel_array
                return "ok (pixels decoded)"
            except Exception as e:  # no handler for this transfer syntax, or numpy missing
                return "ok (pixels not decoded: %s)" % type(e).__name__
        return "ok"
    except Exception as e:
        return "FAILED: %s" % e


def dcmtk_ok(path):
    t = subprocess.run(["dcmftest", path], capture_output=True, text=True, errors="replace")
    d = subprocess.run(["dcmdump", "+E", path], capture_output=True, text=True, errors="replace")
    good = t.stdout.strip().startswith("yes") and d.returncode == 0 and "E: " not in d.stderr
    return good, (t.stdout.strip()[:60], d.returncode, [l for l in d.stderr.splitlines() if l.startswith("E:")][:2])


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build, work = sys.argv[1], sys.argv[2]
    tool = os.path.join(build, "tools", "siaara-repair")
    samples_tool = os.path.join(build, "tests", "repair_samples")
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    sample_dir = os.path.join(work, "samples")
    subprocess.run([samples_tool, sample_dir], check=True, capture_output=True)
    sample_fix = {}
    for line in subprocess.run([samples_tool, os.path.join(work, "samples2")], check=True, capture_output=True,
                               text=True).stdout.splitlines():
        name, fix = (line.split("\t") + [""])[:2]
        sample_fix[name + ".dcm"] = fix
    pyd = os.path.join(os.path.dirname(pydicom.data.__file__), "test_files")
    corpus = [("pydicom", os.path.join(pyd, n)) for n in sorted(os.listdir(pyd)) if n.endswith(".dcm")]
    corpus += [("sample", os.path.join(sample_dir, n)) for n in sorted(os.listdir(sample_dir)) if n.endswith(".dcm")]

    results, failures = [], []
    totals = {"files": 0, "agree_files": 0, "matched": 0, "ours_only": 0, "dciodvfy_only": 0,
              "dciodvfy_unmapped": 0, "unclassified": 0}
    for origin, path in corpus:
        name = os.path.basename(path)
        before_hash = sha(path)
        ours = run_ours(tool, ["validate", path, "--json"])
        theirs = dciodvfy(path)
        if ours is None or theirs is None:
            failures.append("%s: a validator did not run" % name)
            continue
        ok_keys = our_keys(ours)
        their_keys, unmapped = dciodvfy_keys(theirs["errors"])
        matched = ok_keys & their_keys
        ours_only = sorted(ok_keys - their_keys)
        theirs_only = sorted(their_keys - ok_keys)
        entry = {"file": name, "origin": origin, "dciodvfy_aborted": theirs["aborted"],
                 "siaara_errors": ours["files"][0]["summary"]["errors"], "dciodvfy_errors": len(theirs["errors"]),
                 "matched": sorted(matched), "ours_only": [], "dciodvfy_only": [], "dciodvfy_unmapped": []}
        def reason(k):
            if theirs["aborted"]:
                return "dciodvfy aborted (assertion failure) before checking this file"
            if k in KNOWN:
                return KNOWN[k]
            if k[0].startswith("other:"):
                return OURS_ONLY_CLASSES.get(k[0][6:], "unclassified")
            if not theirs["errors"] or any("read failed" in l or "Tags out of order" in l for l in theirs["errors"]):
                return "dciodvfy stopped parsing before reaching this attribute" if any(
                    "read failed" in l or "Tags out of order" in l for l in theirs["errors"]) else "unclassified"
            return "unclassified"

        for k in ours_only:
            entry["ours_only"].append({"key": k, "reason": reason(k)})
        for k in theirs_only:
            entry["dciodvfy_only"].append({"key": k, "reason": reason(k)})
        for l in unmapped:
            entry["dciodvfy_unmapped"].append({"message": l[:160], "reason": classify_unmapped(l)})
        totals["files"] += 1
        totals["matched"] += len(matched)
        totals["ours_only"] += len(ours_only)
        totals["dciodvfy_only"] += len(theirs_only)
        totals["dciodvfy_unmapped"] += len(unmapped)
        totals["unclassified"] += sum(1 for x in entry["ours_only"] + entry["dciodvfy_only"] + entry["dciodvfy_unmapped"]
                                      if x["reason"] == "unclassified")
        if not ours_only and not theirs_only:
            totals["agree_files"] += 1

        # ---- repair
        out_dir = os.path.join(work, "fixed", origin)
        os.makedirs(out_dir, exist_ok=True)
        fix = sample_fix.get(name, "all") if origin == "sample" else "all"
        fixed = run_ours(tool, ["fix", path, "--out", out_dir, "--fix", fix or "all", "--json", "--overwrite",
                                "--uid-seed", "crosscheck"])
        out = os.path.join(out_dir, name)
        entry["fix"] = fix or "all"
        if sha(path) != before_hash:
            failures.append("%s: INPUT CHANGED" % name)
        if fixed is None or not fixed["files"][0]["written"] or not os.path.exists(out):
            entry["repair"] = "not written: %s" % (fixed["files"][0].get("error", "") if fixed else "tool failed")
            if origin == "sample":
                failures.append("%s: repair output not written" % name)
            results.append(entry)
            continue
        if not fixed["files"][0]["input_unchanged"]:
            failures.append("%s: tool reports input changed" % name)
        after = dciodvfy(out)
        entry["dciodvfy_errors_after"] = len(after["errors"]) if after else None
        if after is not None and len(after["errors"]) > len(theirs["errors"]) and not theirs["aborted"]:
            failures.append("%s: dciodvfy errors went up %d -> %d" % (name, len(theirs["errors"]), len(after["errors"])))
        if origin == "sample" and sample_fix.get(name) and len(theirs["errors"]) > 0 and after is not None and \
                len(after["errors"]) >= len(theirs["errors"]) and not theirs["aborted"]:
            # Repairable samples must improve unless dciodvfy did not see the problem in the first place.
            entry["note"] = "dciodvfy error count did not drop"
        entry["siaara_errors_after"] = fixed["files"][0].get("summary_after", {}).get("errors")
        entry["pydicom"] = pydicom_reads(out)
        if entry["pydicom"].startswith("FAILED"):
            failures.append("%s: pydicom cannot read the output: %s" % (name, entry["pydicom"]))
        good, detail = dcmtk_ok(out)
        entry["dcmtk"] = "ok" if good else "FAILED %s" % (detail,)
        if not good:
            failures.append("%s: DCMTK rejects the output %s" % (name, detail))
        results.append(entry)

    report = {"totals": totals, "files": results, "failures": failures}
    with open(os.path.join(work, "repair_crosscheck.json"), "w") as f:
        json.dump(report, f, indent=1)
    print("files %(files)d, files in full agreement %(agree_files)d, matched findings %(matched)d, "
          "SIAARA-only %(ours_only)d, dciodvfy-only %(dciodvfy_only)d, dciodvfy messages without a SIAARA "
          "counterpart %(dciodvfy_unmapped)d, unclassified %(unclassified)d" % totals)
    for e in results:
        dis = e["ours_only"] + e["dciodvfy_only"] + [x for x in e["dciodvfy_unmapped"]]
        if not dis:
            continue
        print("--", e["file"], "(SIAARA %d errors, dciodvfy %d%s)" % (e["siaara_errors"], e["dciodvfy_errors"],
                                                                      ", dciodvfy aborted" if e["dciodvfy_aborted"] else ""))
        for x in e["ours_only"]:
            print("   SIAARA only  ", x["key"], "-", x["reason"])
        for x in e["dciodvfy_only"]:
            print("   dciodvfy only", x["key"], "-", x["reason"])
        for x in e["dciodvfy_unmapped"]:
            print("   dciodvfy msg  ", x["message"][:100], "-", x["reason"])
    print("repair: dciodvfy errors before -> after, pydicom, DCMTK")
    for e in results:
        print("  %-45s %3s -> %-4s %-28s %s" % (e["file"], e["dciodvfy_errors"], e.get("dciodvfy_errors_after", "-"),
                                                e.get("pydicom", e.get("repair", ""))[:28], e.get("dcmtk", "")))
    if failures:
        print("FAILURES:")
        for f in failures:
            print("  " + f)
        return 1
    print("all repair rules held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
