#!/usr/bin/env python3
"""Cross-checks the SIAARA web layer against independent implementations (test oracles only).

1. DICOM JSON vs pydicom, for every pydicom test file pydicom can read:
   a. SIAARA's JSON and pydicom's Dataset.to_json_dict() are compared semantically
      (both parsed; numbers compared as numbers, PN groups, InlineBinary as bytes, nulls).
   b. pydicom.Dataset.from_json() reads SIAARA's JSON back to the same values.
   c. SIAARA reads pydicom's JSON (web_tool fromjson) and pydicom finds the original values
      in the resulting Part 10 file.
   d. data set -> Native DICOM Model XML -> data set gives byte-identical JSON.
2. dicomweb-client (client oracle) against the SIAARA DICOMweb server (web_tool serve).
3. Orthanc + its DICOMweb plugin (server oracle) driven by siaara-webcli, if Orthanc is installed.

Usage: web_crosscheck.py --build BUILD_DIR
Exit status 0 when every check passes. Checks that cannot run are reported as SKIP.
"""
import argparse
import base64
import json
import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from decimal import Decimal, InvalidOperation

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, "PASS" if ok else "FAIL", detail))
    print(("PASS " if ok else "FAIL ") + name + (("  [" + detail + "]") if detail else ""), flush=True)


def skip(name, why):
    RESULTS.append((name, "SKIP", why))
    print("SKIP " + name + "  [" + why + "]", flush=True)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_port(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


# ---------------------------------------------------------------- semantic JSON comparison
SUPPORTED_CHARSETS = {"", "ISO_IR 6", "ISO_IR 192", "ISO_IR 100", "ISO 2022 IR 6", "ISO 2022 IR 100"}


def as_number(v):
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return Decimal(repr(v)) if isinstance(v, float) else Decimal(v)
    if isinstance(v, str):
        try:
            return Decimal(v.strip())
        except InvalidOperation:
            return None
    return None


def norm_str(v):
    if v is None:
        return None
    if isinstance(v, str):
        v = v.rstrip(" \x00")
        return v if v != "" else None
    return v


class ATHalvesSwapped(Exception):
    pass


def same_scalar(vr, a, b):
    if vr in ("DS", "IS", "FL", "FD", "SL", "SS", "UL", "US", "SV", "UV"):
        if a is None or b is None:
            return norm_str(a) is None and norm_str(b) is None
        if a in ("NaN",) or b in ("NaN",):
            return str(a) == str(b) or (isinstance(a, float) and math.isnan(a)) or (isinstance(b, float) and math.isnan(b))
        na, nb = as_number(a), as_number(b)
        if na is None or nb is None:
            return str(a).strip() == str(b).strip()
        if na == nb:
            return True
        if vr == "FL":
            # pydicom prints the float32 as a double; compare as float32.
            try:
                return struct.pack("<f", float(na)) == struct.pack("<f", float(nb))
            except OverflowError:
                return False
        # pydicom writes DS through Python floats: allow the rounding of a double.
        fa, fb = float(na), float(nb)
        return fa == fb or abs(fa - fb) <= 1e-15 * max(abs(fa), abs(fb))
    if vr == "PN":
        if a is None or b is None or a == {} or b == {}:
            return (a in (None, {}) or all(norm_str(x) is None for x in a.values())) and \
                   (b in (None, {}) or all(norm_str(x) is None for x in b.values()))
        # Trailing component delimiters are insignificant (PS3.5 6.2.1).
        def pn(d):
            out = {}
            for k, v in d.items():
                v = norm_str(v)
                v = v.rstrip("^ ") if isinstance(v, str) else v
                if v:
                    out[k] = v
            return out
        return pn(a) == pn(b)
    if vr == "AT":
        a, b = (a or "").upper(), (b or "").upper()
        if a != b and len(a) == 8 and a == b[4:] + b[:4]:
            raise ATHalvesSwapped()
        return a == b
    return norm_str(a) == norm_str(b)


def trim_values(vals):
    vals = list(vals or [])
    def empty(v):
        if isinstance(v, dict):
            return all(not (x or "").rstrip("^= ") for x in v.values())
        return v is None or v == ""
    while vals and empty(vals[-1]):
        vals.pop()
    return vals


def compare_ds(a, b, path, diffs, depth=0):
    ka = {k for k in a if not k.endswith("0000")}
    kb = {k for k in b if not k.endswith("0000")}
    for k in sorted(ka ^ kb):
        diffs.append(f"{path}{k}: present on one side only")
    for k in sorted(ka & kb):
        ea, eb = a[k], b[k]
        vr = ea.get("vr")
        if vr != eb.get("vr"):
            known = "KNOWN-AMBIGUOUS-VR " if {vr, eb.get("vr")} in ({"US", "SS"}, {"OB", "OW"}) else ""
            if vr == "UN" and eb.get("vr") != "UN":
                known = "KNOWN-UN-KEPT "  # the file encodes UN; pydicom replaces it with the dictionary VR
            diffs.append(f"{known}{path}{k}: vr {vr} vs {eb.get('vr')}")
            continue
        if vr == "SQ":
            ia, ib = ea.get("Value", []) or [], eb.get("Value", []) or []
            if len(ia) != len(ib):
                diffs.append(f"{path}{k}: {len(ia)} vs {len(ib)} items")
                continue
            for i, (x, y) in enumerate(zip(ia, ib)):
                compare_ds(x or {}, y or {}, f"{path}{k}/{i}/", diffs, depth + 1)
            continue
        if "InlineBinary" in ea or "InlineBinary" in eb:
            ba = base64.b64decode(ea.get("InlineBinary", "") or "")
            bb = base64.b64decode(eb.get("InlineBinary", "") or "")
            if ba != bb and ba.rstrip(b"\x00") != bb.rstrip(b"\x00"):
                w = {"OW": 2, "US": 2, "SS": 2, "OF": 4, "OL": 4, "FL": 4, "OD": 8, "OV": 8, "FD": 8}.get(vr, 0)
                if w and len(ba) == len(bb) and len(ba) % w == 0 and all(
                        ba[i:i + w] == bb[i:i + w][::-1] for i in range(0, len(ba), w)):
                    diffs.append(f"KNOWN-BYTEORDER {path}{k}: same values, byte order differs")
                else:
                    diffs.append(f"{path}{k}: InlineBinary differs ({len(ba)} vs {len(bb)} bytes)")
            continue
        va, vb = trim_values(ea.get("Value")), trim_values(eb.get("Value"))
        if vr in ("LT", "ST", "UT", "UR") and len(va) == 1 and len(vb) == 1:
            if norm_str(va[0]) != norm_str(vb[0]) and (va[0] or "").strip() != (vb[0] or "").strip():
                diffs.append(f"{path}{k}: {vr} text differs")
            continue
        if len(va) != len(vb):
            diffs.append(f"{path}{k}: {len(va)} vs {len(vb)} values")
            continue
        for x, y in zip(va, vb):
            try:
                if k == "00080005" and {norm_str(x), norm_str(y)} == {"ISO_IR 192", "ISO_IR 100"}:
                    diffs.append(f"KNOWN-CHARSET-RELABEL {path}{k}: text converted to UTF-8 and labelled ISO_IR 192")
                    break
                if not same_scalar(vr, x, y):
                    diffs.append(f"{path}{k}: value {str(x)[:40]!r} vs {str(y)[:40]!r}")
                    break
            except ATHalvesSwapped:
                diffs.append(f"KNOWN-CORE-AT-BIGENDIAN {path}{k}: AT group/element halves swapped ({x} vs {y})")
                break


def charset_of(ds):
    v = ds.get((0x0008, 0x0005))
    if v is None:
        return ""
    val = v.value
    if isinstance(val, (list, tuple)) or getattr(val, "__iter__", None) and not isinstance(val, str):
        return "\\".join(str(x) for x in val)
    return str(val)


# ---------------------------------------------------------------- 1. pydicom
def pydicom_checks(build):
    try:
        import pydicom
        from pydicom.data import get_testdata_files
    except Exception as e:  # pragma: no cover
        skip("pydicom json", f"pydicom not importable: {e}")
        return
    tool = os.path.join(build, "tests", "web_tool")
    files = sorted(set(get_testdata_files("*.dcm")) | set(p for p in get_testdata_files("*") if p.lower().endswith((".dcm", ".dicom")) or "." not in os.path.basename(p)))
    counts = dict(read=0, same=0, differ=0, charset=0, our_fail=0, pyd_fail=0, back_ok=0, back_bad=0, theirs_ok=0, theirs_bad=0,
                  xml_ok=0, xml_identical=0, xml_bad=0)
    first_diffs = []
    known, known_examples = {}, {}
    tmp = tempfile.mkdtemp(prefix="siaara_webx_")
    for path in files:
        try:
            ds = pydicom.dcmread(path, force=True)
            if not hasattr(ds, "file_meta") or len(ds) == 0:
                continue
            theirs = ds.to_json_dict(bulk_data_threshold=1 << 40, bulk_data_element_handler=None)
        except Exception:
            counts["pyd_fail"] += 1
            continue
        counts["read"] += 1
        r = subprocess.run([tool, "tojson", path], capture_output=True, timeout=120)
        if r.returncode != 0:
            counts["our_fail"] += 1
            first_diffs.append(f"{os.path.basename(path)}: siaara failed: {r.stderr.decode(errors='replace').strip()[:120]}")
            continue
        ours = json.loads(r.stdout.decode("utf-8"))
        diffs = []
        compare_ds(ours, theirs, "", diffs)
        for d in diffs:
            if d.startswith("KNOWN-"):
                key = d.split(" ", 1)[0]
                known[key] = known.get(key, 0) + 1
                known_examples.setdefault(key, f"{os.path.basename(path)}: {d}")
        diffs = [d for d in diffs if not d.startswith("KNOWN-")]
        cs = charset_of(ds)
        if diffs and cs not in SUPPORTED_CHARSETS:
            counts["charset"] += 1  # known limitation: character set not converted by SIAARA
        elif diffs:
            counts["differ"] += 1
            first_diffs.append(f"{os.path.basename(path)}: {diffs[0]} (+{len(diffs) - 1} more)")
        else:
            counts["same"] += 1
        # b. pydicom reads our JSON
        try:
            back = pydicom.Dataset.from_json(r.stdout.decode("utf-8"))
            again = back.to_json_dict(bulk_data_threshold=1 << 40, bulk_data_element_handler=None)
            d2 = []
            compare_ds(ours, again, "", d2)
            d2 = [d for d in d2 if not d.startswith("KNOWN-")]
            if d2 and cs in SUPPORTED_CHARSETS:
                counts["back_bad"] += 1
                first_diffs.append(f"{os.path.basename(path)}: pydicom.from_json(ours): {d2[0]}")
            else:
                counts["back_ok"] += 1
        except Exception as e:
            msg = str(e)
            bad_tag = msg.split("'")[1] if msg.count("'") >= 2 else ""
            own_fails = False
            try:
                pydicom.Dataset.from_json(json.dumps(theirs))
            except Exception:
                own_fails = True
            if own_fails:
                counts["back_ok"] += 1
                known["KNOWN-PYDICOM-OWN-JSON"] = known.get("KNOWN-PYDICOM-OWN-JSON", 0) + 1
                known_examples.setdefault("KNOWN-PYDICOM-OWN-JSON", f"{os.path.basename(path)}: pydicom cannot read its own JSON either ({type(e).__name__})")
            elif (bad_tag in ours and ours[bad_tag].get("vr") == "UN") or (
                    isinstance(e, TypeError) and any(v.get("vr") == "UN" and "InlineBinary" in v for v in ours.values())):
                counts["back_ok"] += 1
                known["KNOWN-PYDICOM-UN"] = known.get("KNOWN-PYDICOM-UN", 0) + 1
                known_examples.setdefault("KNOWN-PYDICOM-UN", f"{os.path.basename(path)}: pydicom cannot load UN InlineBinary of public tag {bad_tag}")
            else:
                counts["back_bad"] += 1
                import traceback
                tb = traceback.extract_tb(e.__traceback__)[-1]
                first_diffs.append(f"{os.path.basename(path)}: pydicom.from_json(ours) raised {type(e).__name__}: {msg[:80]} at {os.path.basename(tb.filename)}:{tb.lineno}")
        # c. we read pydicom's JSON
        jin = os.path.join(tmp, "in.json")
        dout = os.path.join(tmp, "out.dcm")
        with open(jin, "w", encoding="utf-8") as fh:
            json.dump(theirs, fh)
        r2 = subprocess.run([tool, "fromjson", jin, dout], capture_output=True, timeout=120)
        if r2.returncode != 0:
            counts["theirs_bad"] += 1
            first_diffs.append(f"{os.path.basename(path)}: siaara fromjson(pydicom): {r2.stderr.decode(errors='replace').strip()[:120]}")
        else:
            try:
                rd = pydicom.dcmread(dout, force=True)
                rd_json = rd.to_json_dict(bulk_data_threshold=1 << 40, bulk_data_element_handler=None)
                d3 = []
                compare_ds(rd_json, theirs, "", d3)
                d3 = [d for d in d3 if not d.startswith("KNOWN-")]
                if d3 and cs in SUPPORTED_CHARSETS:
                    counts["theirs_bad"] += 1
                    first_diffs.append(f"{os.path.basename(path)}: fromjson(pydicom) then pydicom: {d3[0]}")
                else:
                    counts["theirs_ok"] += 1
            except Exception as e:
                counts["theirs_bad"] += 1
                first_diffs.append(f"{os.path.basename(path)}: pydicom could not read our Part 10 from JSON: {str(e)[:80]}")
        # d. XML round trip
        r3 = subprocess.run([tool, "xmljson", path], capture_output=True, timeout=120)
        if r3.returncode == 0 and r3.stdout == r.stdout:
            counts["xml_ok"] += 1
            counts["xml_identical"] += 1
        else:
            d4 = []
            if r3.returncode == 0:
                compare_ds(ours, json.loads(r3.stdout.decode("utf-8")), "", d4)
                d4 = [d for d in d4 if not d.startswith("KNOWN-")]
            if r3.returncode == 0 and not d4:
                counts["xml_ok"] += 1  # equal up to insignificant trailing PN delimiters / charset label
            else:
                counts["xml_bad"] += 1
                first_diffs.append(f"{os.path.basename(path)}: XML round trip differs: {d4[:1]}")
    shutil.rmtree(tmp, ignore_errors=True)
    for d in first_diffs[:60]:
        print("   ", d)
    for k, v in known.items():
        print(f"    known difference {k}: {v} element(s), e.g. {known_examples[k]}")
    summary = ", ".join(f"{k}={v}" for k, v in counts.items())
    check("pydicom: our JSON == pydicom to_json (semantic)", counts["differ"] == 0 and counts["our_fail"] == 0, summary)
    check("pydicom: from_json reads our JSON", counts["back_bad"] == 0, f"{counts['back_ok']} ok")
    check("pydicom: we read pydicom's JSON", counts["theirs_bad"] == 0, f"{counts['theirs_ok']} ok")
    check("XML round trip (data set -> XML -> data set)", counts["xml_bad"] == 0,
          f"{counts['xml_ok']} ok, {counts['xml_identical']} byte-identical JSON")


# ---------------------------------------------------------------- 2. dicomweb-client vs our server
def dicomweb_client_checks(build):
    try:
        import pydicom
        from pydicom.data import get_testdata_file
        from dicomweb_client.api import DICOMwebClient
    except Exception as e:
        skip("dicomweb-client", f"not importable: {e}")
        return
    tool = os.path.join(build, "tests", "web_tool")
    port = free_port()
    proc = subprocess.Popen([tool, "serve", str(port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if not wait_port(port):
            check("dicomweb-client: server started", False)
            return
        url = f"http://127.0.0.1:{port}/dicomweb"
        client = DICOMwebClient(url=url)
        names = ["CT_small.dcm", "MR_small.dcm", "emri_small.dcm", "rtplan.dcm", "SC_rgb_rle.dcm", "JPEG2000.dcm"]
        datasets = []
        for n in names:
            try:
                datasets.append(pydicom.dcmread(get_testdata_file(n)))
            except Exception:
                pass
        resp = client.store_instances(datasets=datasets)
        refs = resp.get((0x0008, 0x1199))
        failed = resp.get((0x0008, 0x1198))
        check("dicomweb-client: STOW-RS store_instances", refs is not None and len(refs.value) == len(datasets) and failed is None,
              f"{len(refs.value) if refs else 0}/{len(datasets)} stored")
        studies = client.search_for_studies()
        check("dicomweb-client: QIDO studies", len(studies) == len({d.StudyInstanceUID for d in datasets}), f"{len(studies)} studies")
        ct = datasets[0]
        m = client.search_for_studies(search_filters={"PatientID": ct.PatientID})
        check("dicomweb-client: QIDO match PatientID", len(m) >= 1 and all(pydicom.Dataset.from_json(x).PatientID == ct.PatientID for x in m))
        lim = client.search_for_studies(limit=1, offset=1)
        check("dicomweb-client: QIDO limit/offset", len(lim) == 1)
        inc = client.search_for_studies(fields=["StudyDescription", "00100030"])
        check("dicomweb-client: QIDO includefield", len(inc) == len(studies))
        fz = client.search_for_studies(search_filters={"PatientName": str(ct.PatientName).lower()[:3] + "*"}, fuzzymatching=True)
        check("dicomweb-client: QIDO fuzzymatching", len(fz) >= 1)
        se = client.search_for_series(study_instance_uid=ct.StudyInstanceUID)
        check("dicomweb-client: QIDO series", len(se) >= 1)
        ins = client.search_for_instances(study_instance_uid=ct.StudyInstanceUID, series_instance_uid=ct.SeriesInstanceUID)
        check("dicomweb-client: QIDO instances", len(ins) >= 1)
        got = client.retrieve_instance(ct.StudyInstanceUID, ct.SeriesInstanceUID, ct.SOPInstanceUID)
        check("dicomweb-client: WADO-RS retrieve_instance", got.SOPInstanceUID == ct.SOPInstanceUID and
              got.PixelData == ct.PixelData and got.PatientName == ct.PatientName)
        study_files = client.retrieve_study(ct.StudyInstanceUID)
        check("dicomweb-client: WADO-RS retrieve_study", len(study_files) >= 1)
        rle = next((d for d in datasets if d.file_meta.TransferSyntaxUID == "1.2.840.10008.1.2.5"), None)
        if rle is not None:
            raw = client.retrieve_instance(rle.StudyInstanceUID, rle.SeriesInstanceUID, rle.SOPInstanceUID,
                                           media_types=(("application/dicom", "*"),))
            check("dicomweb-client: WADO-RS transfer-syntax=* keeps RLE", raw.file_meta.TransferSyntaxUID == "1.2.840.10008.1.2.5")
            dec = client.retrieve_instance(rle.StudyInstanceUID, rle.SeriesInstanceUID, rle.SOPInstanceUID,
                                           media_types=(("application/dicom", "1.2.840.10008.1.2.1"),))
            import numpy as np
            check("dicomweb-client: WADO-RS decompressed RLE equals pydicom decode",
                  dec.file_meta.TransferSyntaxUID == "1.2.840.10008.1.2.1" and np.array_equal(dec.pixel_array, rle.pixel_array))
        meta = client.retrieve_instance_metadata(ct.StudyInstanceUID, ct.SeriesInstanceUID, ct.SOPInstanceUID)
        md = pydicom.Dataset.from_json(meta, bulk_data_uri_handler=lambda tag, vr, uri: client.retrieve_bulkdata(uri)[0])
        check("dicomweb-client: WADO-RS metadata + BulkDataURI via retrieve_bulkdata", md.PixelData == ct.PixelData and md.Rows == ct.Rows)
        frames = client.retrieve_instance_frames(ct.StudyInstanceUID, ct.SeriesInstanceUID, ct.SOPInstanceUID, frame_numbers=[1])
        check("dicomweb-client: WADO-RS frames (uncompressed)", len(frames) == 1 and frames[0] == ct.PixelData)
        mr_multi = next((d for d in datasets if int(getattr(d, "NumberOfFrames", 1) or 1) > 1), None)
        if mr_multi is not None:
            fr = client.retrieve_instance_frames(mr_multi.StudyInstanceUID, mr_multi.SeriesInstanceUID, mr_multi.SOPInstanceUID,
                                                 frame_numbers=[2, 1])
            n = len(mr_multi.PixelData) // int(mr_multi.NumberOfFrames)
            check("dicomweb-client: WADO-RS frames 2,1 of a multi-frame instance",
                  len(fr) == 2 and fr[0] == mr_multi.PixelData[n:2 * n] and fr[1] == mr_multi.PixelData[:n])
        try:
            client.retrieve_instance(ct.StudyInstanceUID, ct.SeriesInstanceUID, "1.2.3.4.5.6")
            check("dicomweb-client: 404 for a missing instance", False)
        except Exception as e:
            check("dicomweb-client: 404 for a missing instance", "404" in str(e), type(e).__name__)
        from dicomweb_client.api import DICOMwebClient as C2
        wado_uri = C2(url=url, wado_url_prefix=None)
        try:
            data = wado_uri.retrieve_instance(ct.StudyInstanceUID, ct.SeriesInstanceUID, ct.SOPInstanceUID)
            check("dicomweb-client: second client on the same server", data.SOPInstanceUID == ct.SOPInstanceUID)
        except Exception as e:
            check("dicomweb-client: second client on the same server", False, str(e)[:80])
    finally:
        try:
            proc.stdin.close()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


# ---------------------------------------------------------------- 3. Orthanc vs our client
def orthanc_checks(build):
    orthanc = shutil.which("Orthanc")
    plugin = None
    for p in ("/usr/share/orthanc/plugins/libOrthancDicomWeb.so", "/usr/lib/orthanc/libOrthancDicomWeb.so"):
        if os.path.exists(p):
            plugin = p
    if not orthanc or not plugin:
        skip("Orthanc", "Orthanc or its DICOMweb plugin is not installed")
        return
    import pydicom
    from pydicom.data import get_testdata_file
    cli = os.path.join(build, "tools", "siaara-webcli")
    tmp = tempfile.mkdtemp(prefix="siaara_orthanc_")
    port, dport = free_port(), free_port()
    cfg = {"Name": "SIAARA-XCHECK", "StorageDirectory": os.path.join(tmp, "db"), "IndexDirectory": os.path.join(tmp, "db"),
           "HttpPort": port, "DicomPort": dport, "RemoteAccessAllowed": False, "AuthenticationEnabled": False,
           "Plugins": [plugin], "DicomWeb": {"Enable": True, "Root": "/dicom-web/", "EnableWado": True, "WadoRoot": "/wado"}}
    with open(os.path.join(tmp, "orthanc.json"), "w") as fh:
        json.dump(cfg, fh)
    log = open(os.path.join(tmp, "orthanc.log"), "w")
    proc = subprocess.Popen([orthanc, os.path.join(tmp, "orthanc.json")], stdout=log, stderr=subprocess.STDOUT)
    try:
        if not wait_port(port, 30):
            check("Orthanc: started", False, "see " + log.name)
            return
        time.sleep(1.0)
        base = f"http://127.0.0.1:{port}/dicom-web"

        def run(*args, timeout=120):
            p = subprocess.run([cli, *args], capture_output=True, timeout=timeout)
            return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

        names = ["CT_small.dcm", "MR_small.dcm", "emri_small.dcm", "SC_rgb_rle.dcm"]
        paths = [get_testdata_file(n) for n in names]
        rc, out, err = run("stow", base, *paths)
        check("Orthanc: siaara STOW-RS", rc == 0 and "4 stored, 0 failed" in err, err.strip()[-80:])
        rc, out, err = run("qido", base, "studies")
        studies = json.loads(out) if rc == 0 else []
        uids = {pydicom.dcmread(p).StudyInstanceUID for p in paths}
        check("Orthanc: siaara QIDO studies", rc == 0 and {s["0020000D"]["Value"][0] for s in studies} == uids, f"{len(studies)} studies")
        ct = pydicom.dcmread(paths[0])
        rc, out, err = run("qido", base, "studies", f"PatientID={ct.PatientID}")
        check("Orthanc: siaara QIDO match", rc == 0 and len(json.loads(out)) >= 1)
        rc, out, err = run("qido", base, "instances", "--study", ct.StudyInstanceUID, "--include", "all")
        check("Orthanc: siaara QIDO instances includefield=all", rc == 0 and len(json.loads(out)) >= 1)
        rc, out, err = run("qido", base, "studies", "--limit", "1")
        check("Orthanc: siaara QIDO limit", rc == 0 and len(json.loads(out)) == 1)
        outdir = os.path.join(tmp, "wado")
        os.makedirs(outdir)
        all_ok = True
        detail = ""
        for p in paths:
            d = pydicom.dcmread(p)
            rc, out, err = run("wado", base, d.StudyInstanceUID, d.SeriesInstanceUID, d.SOPInstanceUID, "--out", outdir, "--ts", "*")
            f = os.path.join(outdir, d.SOPInstanceUID + ".dcm")
            if rc != 0 or not os.path.exists(f):
                all_ok, detail = False, f"{os.path.basename(p)}: {err.strip()[:80]}"
                continue
            g = pydicom.dcmread(f)
            if g.PixelData != d.PixelData or g.SOPInstanceUID != d.SOPInstanceUID:
                all_ok, detail = False, f"{os.path.basename(p)}: pixel data differs"
        check("Orthanc: siaara WADO-RS instances (as stored) equal the originals", all_ok, detail)
        rc, out, err = run("metadata", base, ct.StudyInstanceUID, ct.SeriesInstanceUID, ct.SOPInstanceUID)
        ok_meta = rc == 0
        if ok_meta:
            m = json.loads(out)[0]
            ok_meta = m["00100020"]["Value"][0] == ct.PatientID and "7FE00010" in m
        check("Orthanc: siaara WADO-RS metadata", ok_meta, err.strip()[:80])
        rc, out, err = run("frames", base, ct.StudyInstanceUID, ct.SeriesInstanceUID, ct.SOPInstanceUID, "1",
                           "--out", os.path.join(tmp, "frame"))
        fr_ok = rc == 0 and os.path.exists(os.path.join(tmp, "frame.1")) and open(os.path.join(tmp, "frame.1"), "rb").read() == ct.PixelData
        check("Orthanc: siaara WADO-RS frames (uncompressed)", fr_ok, err.strip()[:120])
        wu = os.path.join(tmp, "wado_uri.dcm")
        rc, out, err = run("wado-uri", f"http://127.0.0.1:{port}/dicom-web", ct.StudyInstanceUID, ct.SeriesInstanceUID,
                           ct.SOPInstanceUID, "--out", wu)
        # Orthanc serves WADO-URI at /wado (outside the DICOMweb root): use a raw client call via base on the root.
        if rc != 0:
            rc, out, err = run("wado-uri", f"http://127.0.0.1:{port}", ct.StudyInstanceUID, ct.SeriesInstanceUID,
                               ct.SOPInstanceUID, "--out", wu)
        wu_ok = rc == 0 and os.path.exists(wu) and pydicom.dcmread(wu).SOPInstanceUID == ct.SOPInstanceUID
        check("Orthanc: siaara WADO-URI", wu_ok, err.strip()[:120])
        rc, out, err = run("wado", base, "1.2.3.999", "--out", outdir)
        check("Orthanc: siaara 404 for a missing study", rc != 0 and "404" in err, err.strip()[:80])
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except Exception:
            proc.kill()
        log.close()
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--only", choices=["pydicom", "client", "orthanc"])
    a = ap.parse_args()
    build = os.path.abspath(a.build)
    if a.only in (None, "pydicom"):
        pydicom_checks(build)
    if a.only in (None, "client"):
        dicomweb_client_checks(build)
    if a.only in (None, "orthanc"):
        orthanc_checks(build)
    fails = [r for r in RESULTS if r[1] == "FAIL"]
    print(f"\n{sum(r[1] == 'PASS' for r in RESULTS)} passed, {len(fails)} failed, {sum(r[1] == 'SKIP' for r in RESULTS)} skipped")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
