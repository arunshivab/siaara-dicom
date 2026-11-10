#!/usr/bin/env python3
"""Generates the tables of docs/CONFORMANCE.md from the code (standard library only).

    python3 tools/release/gen_conformance.py --dump build/tools/siaara_conformance_dump          # rewrite
    python3 tools/release/gen_conformance.py --dump build/tools/siaara_conformance_dump --check  # ctest

Sources of the tables:
  - src/core/transfer_syntax.cpp   the transfer syntax registry (UID list; parsed here)
  - src/charset/charset.cpp        the Specific Character Set terms (kTerms; parsed here)
  - src/capi/store_services.cpp    the Query/Retrieve keys of the store-backed SCP (parsed here)
  - src/capi/capi_net.cpp          the compressed syntaxes the C API SCP / C-GET SCU offer (parsed here)
  - the built library, through tools/release/conformance_dump.cpp: names from the UID registry,
    what the codecs decode and encode, the storage SOP class list (net::storage_sop_classes), the
    SOP classes of the other services, the proposal order of the SCU, and the default limits.
The text between "<!-- BEGIN GENERATED: name -->" and "<!-- END GENERATED: name -->" in
docs/CONFORMANCE.md is replaced; everything else in that file is written by hand. --check fails
(exit 1) when a block differs from what the code gives, listing the blocks.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DOC = os.path.join(ROOT, "docs", "CONFORMANCE.md")


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def registry_uids():
    """UIDs of kTransferSyntaxes then kRetiredOnly, in source order."""
    src = read("src/core/transfer_syntax.cpp")
    uids = re.findall(r'TransferSyntaxInfo\{"([0-9.]+)"', src)
    if len(uids) < 40:
        raise SystemExit("gen_conformance: transfer syntax table not found in src/core/transfer_syntax.cpp")
    return uids


def capi_compressed():
    src = read("src/capi/capi_net.cpp")
    m = re.search(r"compressed_syntaxes\(\)\s*\{\s*static const std::vector<std::string> v = \{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit("gen_conformance: compressed_syntaxes() not found in src/capi/capi_net.cpp")
    return re.findall(r'"([0-9.]+)"', m.group(1))


GSETS = {
    "Ascii": "ISO 646 (ISO-IR 6)", "None": "-", "Latin1": "ISO 8859-1 Latin alphabet No. 1 (ISO-IR 100)",
    "Latin2": "ISO 8859-2 Latin alphabet No. 2 (ISO-IR 101)", "Latin3": "ISO 8859-3 Latin alphabet No. 3 (ISO-IR 109)",
    "Latin4": "ISO 8859-4 Latin alphabet No. 4 (ISO-IR 110)", "Cyrillic": "ISO 8859-5 Cyrillic (ISO-IR 144)",
    "Arabic": "ISO 8859-6 Arabic (ISO-IR 127)", "Greek": "ISO 8859-7 Greek (ISO-IR 126)",
    "Hebrew": "ISO 8859-8 Hebrew (ISO-IR 138)", "Latin5": "ISO 8859-9 Latin alphabet No. 5 (ISO-IR 148)",
    "Latin9": "ISO 8859-15 Latin alphabet No. 9 (ISO-IR 203)", "JisRoman": "JIS X 0201 Romaji (ISO-IR 14)",
    "JisKatakana": "JIS X 0201 Katakana (ISO-IR 13)", "Thai": "TIS 620-2533 Thai (ISO-IR 166)",
    "Jis0208": "JIS X 0208 Kanji (ISO-IR 87)", "Jis0212": "JIS X 0212 Supplementary Kanji (ISO-IR 159)",
    "Ksx1001": "KS X 1001 Hangul and Hanja (ISO-IR 149)", "Gb2312": "GB 2312 Simplified Chinese (ISO-IR 58)",
}
WHOLE = {"IR192": "Unicode in UTF-8", "GB18030": "GB 18030", "GBK": "GBK"}


def charset_rows():
    src = read("src/charset/charset.cpp")
    rows = re.findall(r'\{Term::(\w+),\s*"([^"]*)",\s*"([^"]*)",\s*GSet::(\w+),\s*GSet::(\w+)\}', src)
    if len(rows) < 15:
        raise SystemExit("gen_conformance: kTerms not found in src/charset/charset.cpp")
    out = []
    for term, plain, ext, g0, g1 in rows:
        for g in (g0, g1):
            if g not in GSETS:
                raise SystemExit("gen_conformance: unknown character set G-set %s: add it to GSETS" % g)
        if term in WHOLE:
            what = WHOLE[term] + " (no code extensions)"
        elif term == "IR6":
            what = "G0: " + GSETS[g0] + " (the default repertoire; also when (0008,0005) is absent or empty)"
        else:
            what = "; ".join(x for x in ("G0: " + GSETS[g0] if g0 != "None" else "", "G1: " + GSETS[g1] if g1 != "None" else "") if x)
        out.append((plain or "-", ext or "-", what))
    return out


def qr_key_rows():
    """The Q/R key table of the store-backed SCP (src/capi/store_services.cpp key_table)."""
    src = read("src/capi/store_services.cpp")
    m = re.search(r"key_table\(\)\s*\{\s*static const std::vector<KeyDef> t = \{(.*?)\n    \};", src, re.S)
    if not m:
        raise SystemExit("gen_conformance: key_table() not found in src/capi/store_services.cpp")
    consts = {"kRetrieveAeTitle": 0x00080054, "kInstanceAvailability": 0x00080056}
    for name in consts:
        c = re.search(r"%s\s*=\s*(0x[0-9A-Fa-f]+)" % name, src)
        if c:
            consts[name] = int(c.group(1), 16)
    names = dict((int(t, 16), n) for t, n in re.findall(
        r'Entry\{0x([0-9A-Fa-f]{8})u, 0xFFFFFFFFu, VR::\w+, VR::\w+, "[^"]*", "([^"]*)"', read("src/dict/dictionary_data.inc")))
    levels = {"kPatient": "PATIENT", "kStudy": "STUDY", "kSeries": "SERIES", "kImage": "IMAGE"}
    rows = []
    for tag, vr, level, src_kind, ret in re.findall(
            r"\{(0x[0-9A-Fa-f]{8}|k\w+), VR::(\w+), (k\w+), Src::(\w+), Str::\w+, (true|false)\}", m.group(1)):
        t = consts[tag] if tag in consts else int(tag, 16)
        if tag in consts:
            lvl, kind = "every level", "set by the SCP in responses"
        else:
            lvl, kind = levels[level], ("index field" if src_kind == "Field" else "computed from the index")
        rows.append((names.get(t, "?"), "(%04X,%04X)" % (t >> 16, t & 0xFFFF), vr, lvl, kind,
                     "yes" if ret == "true" else "no"))
    if len(rows) < 20:
        raise SystemExit("gen_conformance: the Q/R key table could not be parsed")
    return rows


def yn(b):
    return "yes" if b else "no"


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for r in rows:
        lines.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(lines)


def uid_rows(items):
    return [(i["name"] or "(not in the UID registry)", "`%s`" % i["uid"]) for i in items]


def blocks(d):
    b = {}
    df = d["defaults"]
    ts = {t["uid"]: t for t in d["transfer_syntaxes"]}

    b["identity"] = table(["Item", "Value"], [
        ("Application Context Name", "`%s`" % df["application_context"]),
        ("Implementation Class UID", "`%s`" % d["implementation_class_uid"]),
        ("Implementation Version Name", "`%s`" % d["implementation_version_name"]),
        ("Toolkit version", d["version"]),
    ])

    # ---- overview: network services. Roles are what the C API offers (hand-checked against
    # src/capi/capi_net*.cpp); the UIDs and names come from the code.
    ov = []
    def add(key, scu, scp):
        for i in d[key]:
            ov.append((i["name"], "`%s`" % i["uid"], scu, scp))
    add("verification", "Yes", "Yes")
    ov.append(("Storage: every SOP class of table B.1 (%d), or any list the application gives" % len(d["storage_sop_classes"]),
               "see B.1", "Yes", "Yes"))
    add("qr_find", "Yes", "Yes")
    add("qr_move", "Yes", "Yes")
    add("qr_get", "Yes", "Yes")
    add("composite_retrieve_move", "C++ layer only", "Yes")
    add("composite_retrieve_get", "C++ layer only", "Yes")
    add("worklist", "Yes", "Yes")
    add("mpps", "Yes", "Yes")
    add("storage_commitment", "Yes", "Yes")
    for i in d["ups"]:
        event = i["uid"].endswith(".6.4")
        ov.append((i["name"], "`%s`" % i["uid"],
                   "C++ layer only (receiving event reports)" if event else "Yes (generic DIMSE-N, C-FIND)",
                   "Yes (sends event reports to subscribers)" if event else "Yes"))
    add("instance_availability", "Yes (generic DIMSE-N)", "Yes")
    add("print", "C++ layer only (not in the C API)", "No")
    b["overview-network"] = table(["SOP Class", "UID", "User of Service (SCU)", "Provider of Service (SCP)"], ov)

    b["storage-sop-classes"] = table(["#", "SOP Class Name", "SOP Class UID"],
                                     [(n + 1,) + r for n, r in enumerate(uid_rows(d["storage_sop_classes"]))]) + \
        "\n\n%d storage SOP classes in the default list (`net::storage_sop_classes()`, src/net/dimse.cpp)." % len(d["storage_sop_classes"])

    rows = []
    n_read = n_dec = n_enc = 0
    for t in d["transfer_syntaxes"]:
        note = []
        private = not t["uid"].startswith("1.2.840.10008.")
        if t["retired"] and not private:
            note.append("retired")
        if not t["dataset"]:
            note.append("named only: not read or written")
        elif t["encapsulated"] and not t["decode"]:
            note.append("pixel data carried as it is (no codec)")
        if t["encode_always_lossy"]:
            note.append("lossy")
        elif t["encode"] and t["lossy"] and t["uid"] == "1.2.840.10008.1.2.4.203":
            # The HTJ2K encoder is lossless only (src/codec/transcode.cpp): a request for lossy
            # coding in this syntax is refused.
            note.append("written losslessly only; lossy coding on request is refused (the encoder is lossless only)")
        elif t["encode"] and t["lossy"]:
            note.append("written losslessly unless lossy coding is requested")
        if private:
            note.append("private transfer syntax of another vendor (GE): read only")
        n_read += t["dataset"]
        n_dec += t["decode"]
        n_enc += t["encode"]
        rows.append((t["name"], "`%s`" % t["uid"], yn(t["dataset"]), yn(t["decode"]), yn(t["encode"]), "; ".join(note)))
    b["transfer-syntaxes"] = table(
        ["Transfer Syntax", "UID", "Data set read / written", "Pixel data decoded", "Pixel data encoded (transcode target)", "Notes"],
        rows) + ("\n\n%d transfer syntaxes in the registry: %d whose data sets are read and written, %d whose pixel data "
                 "is decoded, %d that the transcoder writes." % (len(rows), n_read, n_dec, n_enc))

    def ts_list(uids):
        return "\n".join("%d. %s (`%s`)" % (n + 1, ts[u]["name"] if u in ts else u, u) for n, u in enumerate(uids))

    native = [i["uid"] for i in d["native_transfer_syntaxes"]]
    comp = capi_compressed()
    for u in comp:
        if u not in ts:
            raise SystemExit("gen_conformance: %s (capi_net.cpp) is not in the transfer syntax registry" % u)
    b["ts-native"] = ts_list(native)
    b["ts-scp-compressed"] = ts_list(comp)
    b["ts-scu-lossless"] = ts_list(["1.2.840.10008.1.2.1.99"] + [i["uid"] for i in d["transcode_targets_lossless"]])
    b["ts-scu-lossy"] = ts_list([i["uid"] for i in d["transcode_targets_lossy"]])

    keys = qr_key_rows()
    b["qr-keys"] = table(["Attribute", "Tag", "VR", "Level", "Source", "In the default identifier (`siaara_qr_identifier`)"], keys) + \
        "\n\n%d keys (`key_table()`, src/capi/store_services.cpp)." % len(keys)

    cs = charset_rows()
    b["charsets"] = table(["Defined Term (single value / first value)", "Defined Term with code extensions", "Character sets"], cs) + \
        "\n\n%d Specific Character Set defined terms." % len(cs)

    def s(ms):
        return "%g s" % (ms / 1000.0)
    b["defaults"] = table(["Parameter", "Default"], [
        ("Local AE title, acceptor / initiator", "`%s` / `%s`" % (df["scp_ae_title"], df["scu_ae_title"])),
        ("Maximum PDU length offered (received)", "%d bytes (configurable; hard cap %d bytes)" % (df["max_receive_pdu"], df["max_pdu_cap"])),
        ("Presentation contexts per association", "at most %d" % df["max_presentation_contexts"]),
        ("Simultaneous associations accepted", "%d, and %d per peer address" % (df["max_associations"], df["max_associations_per_ip"])),
        ("Association lifetime / operations per association (acceptor)", "%g h / %d, then A-ABORT" % (df["max_association_lifetime_ms"] / 3600000.0, df["max_operations_per_association"])),
        ("Asynchronous Operations Window accepted", "up to %d operations invoked, performed in order of arrival" % df["max_async_operations"]),
        ("TCP connect timeout", s(df["connect_ms"])),
        ("ACSE timeout (A-ASSOCIATE / A-RELEASE response)", s(df["acse_ms"])),
        ("DIMSE timeout (response or next fragment)", s(df["dimse_ms"])),
        ("ARTIM timer", s(df["artim_ms"])),
        ("Idle timeout (acceptor waiting for a request)", s(df["idle_ms"])),
        ("Write timeout (send-buffer space)", s(df["write_ms"])),
        ("Incomplete data set received", ("quarantined" if df["incomplete_policy_quarantine"] else "see docs/NETWORKING.md 2.3b") +
         "; sender answered warning 0x%04X (failure status when refused: 0x%04X)" % (df["incomplete_warning_status"], df["incomplete_failure_status"])),
        ("Overlong-length recovery heuristic on network input", "on" if df["network_overlong_recovery"] else "off"),
        ("QIDO-RS results per request (server)", "at most %d" % df["web_max_qido_results"]),
    ])
    return b


def apply(text, b):
    seen = set()

    def sub(m):
        name = m.group(1)
        if name not in b:
            raise SystemExit("gen_conformance: docs/CONFORMANCE.md has an unknown generated block '%s'" % name)
        seen.add(name)
        return "<!-- BEGIN GENERATED: %s -->\n%s\n<!-- END GENERATED: %s -->" % (name, b[name], name)

    out = re.sub(r"<!-- BEGIN GENERATED: ([\w-]+) -->\n.*?<!-- END GENERATED: \1 -->", sub, text, flags=re.S)
    missing = sorted(set(b) - seen)
    if missing:
        raise SystemExit("gen_conformance: docs/CONFORMANCE.md has no block for: " + ", ".join(missing))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True, help="path of the built siaara_conformance_dump program")
    ap.add_argument("--check", action="store_true", help="do not write; fail when the document differs")
    ap.add_argument("--doc", default=DOC)
    args = ap.parse_args()
    raw = subprocess.run([args.dump] + registry_uids(), check=True, stdout=subprocess.PIPE).stdout
    b = blocks(json.loads(raw.decode("utf-8")))
    with open(args.doc, encoding="utf-8") as f:
        old = f.read()
    new = apply(old, b)
    if args.check:
        if new != old:
            changed = []
            for name in sorted(b):
                m = re.search(r"<!-- BEGIN GENERATED: %s -->\n(.*?)\n<!-- END GENERATED: %s -->" % (name, name), old, re.S)
                if not m or m.group(1) != b[name]:
                    changed.append(name)
            print("docs/CONFORMANCE.md differs from the code in: %s\nRun: python3 tools/release/gen_conformance.py --dump %s"
                  % (", ".join(changed), args.dump), file=sys.stderr)
            return 1
        print("docs/CONFORMANCE.md matches the code (%d generated blocks)" % len(b))
        return 0
    if new != old:
        with open(args.doc, "w", encoding="utf-8", newline="\n") as f:
            f.write(new)
        print("docs/CONFORMANCE.md updated")
    else:
        print("docs/CONFORMANCE.md is up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
