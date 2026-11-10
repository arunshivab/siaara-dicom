#!/usr/bin/env python3
"""Interoperability checks for the C API service set (test-only, never shipped).

The SIAARA SCP (siaara_scp_start_ex, through capi_net_tool) is driven by third-party SCUs, and
the SIAARA C API SCU calls are run against third-party SCPs. Third-party software is used purely
as an oracle:
  - DCMTK (/usr/bin: echoscu, storescu, findscu, movescu, getscu, storescp). Called by absolute
    path: pynetdicom installs scripts with the same names in /usr/local/bin.
  - pynetdicom + pydicom (Modality Worklist, MPPS, Storage Commitment, UPS)

usage: capi_net_crosscheck.py BUILD_DIR [section ...]
  sections: dcmtk pynetdicom-scu siaara-scu (default: all)
Prints one line per check (PASS/FAIL/SKIP) and the exact commands it ran; exit 1 on a FAIL.
"""
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else "build"
SECTIONS = sys.argv[2:] or ["dcmtk", "pynetdicom-scu", "siaara-scu"]
TOOL = os.path.join(BUILD, "tests", "capi_net_tool")
DCMTK = "/usr/bin"
RESULTS = []
CT = "1.2.840.10008.5.1.4.1.1.2"


def record(name, ok, detail="", skip=False):
    status = "SKIP" if skip else ("PASS" if ok else "FAIL")
    RESULTS.append(status)
    print(f"{status} {name}{(' : ' + detail) if detail else ''}", flush=True)


def run(cmd, timeout=60):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, text=True)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Process:
    def __init__(self, cmd, port_file=None, port=None):
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.port = port
        end = time.time() + 15
        while time.time() < end:
            if port_file and os.path.exists(port_file) and open(port_file).read().strip():
                self.port = int(open(port_file).read())
                return
            if port and not port_file:
                try:
                    with socket.create_connection(("127.0.0.1", port), 0.2):
                        return
                except OSError:
                    pass
            if self.proc.poll() is not None:
                raise RuntimeError("process exited: " + self.proc.stderr.read().decode(errors="replace")[:400])
            time.sleep(0.05)
        raise RuntimeError("process did not start: " + " ".join(cmd))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        out, err = self.proc.communicate()
        return out.decode(errors="replace"), err.decode(errors="replace")


def make_files(tmp):
    import pydicom
    from pydicom.dataset import FileDataset, FileMetaDataset
    from pydicom.uid import ExplicitVRLittleEndian
    paths = []
    for i, (study, series, pid) in enumerate([("2.25.71001", "2.25.71011", "XC1"), ("2.25.71001", "2.25.71011", "XC1"),
                                              ("2.25.71002", "2.25.71021", "XC2")]):
        meta = FileMetaDataset()
        meta.MediaStorageSOPClassUID = CT
        meta.MediaStorageSOPInstanceUID = f"2.25.7100{i}9"
        meta.TransferSyntaxUID = ExplicitVRLittleEndian
        ds = FileDataset(None, {}, file_meta=meta, preamble=b"\0" * 128)
        ds.SOPClassUID = CT
        ds.SOPInstanceUID = meta.MediaStorageSOPInstanceUID
        ds.StudyInstanceUID, ds.SeriesInstanceUID, ds.PatientID = study, series, pid
        ds.PatientName = "Cross^Check"
        ds.Modality = "CT"
        ds.StudyDate = "20261110"
        ds.SamplesPerPixel, ds.PhotometricInterpretation = 1, "MONOCHROME2"
        ds.Rows = ds.Columns = 4
        ds.BitsAllocated, ds.BitsStored, ds.HighBit, ds.PixelRepresentation = 16, 12, 11, 0
        ds.PixelData = bytes(range(32))
        p = os.path.join(tmp, f"ct{i}.dcm")
        ds.save_as(p, enforce_file_format=True)
        paths.append(p)
    return paths


WORKLIST = [
    {"00100010": {"vr": "PN", "Value": [{"Alphabetic": "Work^One"}]}, "00100020": {"vr": "LO", "Value": ["W1"]},
     "0020000D": {"vr": "UI", "Value": ["2.25.72001"]}, "00080050": {"vr": "SH", "Value": ["A1"]},
     "00400100": {"vr": "SQ", "Value": [{"00400001": {"vr": "AE", "Value": ["CT01"]},
                                         "00080060": {"vr": "CS", "Value": ["CT"]},
                                         "00400002": {"vr": "DA", "Value": ["20261110"]}}]}},
    {"00100010": {"vr": "PN", "Value": [{"Alphabetic": "Work^Two"}]}, "00100020": {"vr": "LO", "Value": ["W2"]},
     "00400100": {"vr": "SQ", "Value": [{"00400001": {"vr": "AE", "Value": ["MR01"]},
                                         "00080060": {"vr": "CS", "Value": ["MR"]}}]}},
]


def start_scp(tmp, extra=()):
    pf = os.path.join(tmp, "port" + str(time.time()))
    wl = os.path.join(tmp, "worklist.json")
    with open(wl, "w") as f:
        json.dump(WORKLIST, f)
    cmd = [TOOL, "scp", pf, os.path.join(tmp, "store"), "SIAARA_SCP", "--worklist", wl] + list(extra)
    return Process(cmd, port_file=pf), cmd


def section_dcmtk():
    tmp = tempfile.mkdtemp(prefix="siaara_xc_capinet_")
    files = make_files(tmp)
    dport = free_port()
    ddir = os.path.join(tmp, "dcmtk_dest")
    os.makedirs(ddir)
    dest = Process([os.path.join(DCMTK, "storescp"), "-aet", "DCMDEST", "-od", ddir, str(dport)], port=dport)
    scp, cmd = start_scp(tmp, ["--dest", f"DCMDEST:127.0.0.1:{dport}"])
    base = ["-aet", "DCMTK_SCU", "-aec", "SIAARA_SCP"]
    try:
        p = run([os.path.join(DCMTK, "echoscu")] + base + ["127.0.0.1", str(scp.port)])
        record("DCMTK echoscu -> SIAARA SCP", p.returncode == 0, "echoscu -> rc " + str(p.returncode))
        p = run([os.path.join(DCMTK, "storescu")] + base + ["127.0.0.1", str(scp.port)] + files)
        record("DCMTK storescu -> SIAARA SCP (3 files)", p.returncode == 0, "storescu -> rc " + str(p.returncode) +
               " " + p.stderr.strip()[-200:])

        def find(model, keys):
            c = [os.path.join(DCMTK, "findscu"), "-v", model] + base
            for k in keys:
                c += ["-k", k]
            c += ["127.0.0.1", str(scp.port)]
            r = run(c)
            return r, len(re.findall(r"Find Response: \d+ \(Pending\)", r.stdout + r.stderr))

        for model, keys, want, label in [
            ("-P", ["QueryRetrieveLevel=PATIENT", "PatientID", "PatientName", "NumberOfPatientRelatedStudies"], 2, "Patient Root PATIENT"),
            ("-S", ["QueryRetrieveLevel=STUDY", "PatientID=XC1", "StudyInstanceUID", "NumberOfStudyRelatedInstances"], 1, "Study Root STUDY by PatientID"),
            ("-S", ["QueryRetrieveLevel=STUDY", "PatientName=Cross*", "StudyDate=20260101-20261231", "StudyInstanceUID"], 2, "Study Root STUDY wildcard + date range"),
            ("-S", ["QueryRetrieveLevel=SERIES", "StudyInstanceUID=2.25.71001", "SeriesInstanceUID", "Modality"], 1, "Study Root SERIES"),
            ("-S", ["QueryRetrieveLevel=IMAGE", "StudyInstanceUID=2.25.71001", "SeriesInstanceUID=2.25.71011", "SOPInstanceUID"], 2, "Study Root IMAGE"),
            ("-P", ["QueryRetrieveLevel=STUDY", "PatientID=XC2", "StudyInstanceUID"], 1, "Patient Root STUDY"),
        ]:
            r, n = find(model, keys)
            record(f"DCMTK findscu {label} -> SIAARA SCP", r.returncode == 0 and n == want, f"{n} matches (want {want})")
        p = run([os.path.join(DCMTK, "movescu"), "-v", "-S"] + base + ["-aem", "DCMDEST", "-k", "QueryRetrieveLevel=STUDY",
                                                                       "-k", "StudyInstanceUID=2.25.71001", "127.0.0.1", str(scp.port)])
        time.sleep(0.5)
        got = len(os.listdir(ddir))
        record("DCMTK movescu -> SIAARA SCP -> DCMTK storescp", p.returncode == 0 and got == 2,
               f"rc {p.returncode}, {got} files at the destination")
        gdir = os.path.join(tmp, "getscu")
        os.makedirs(gdir)
        p = run([os.path.join(DCMTK, "getscu"), "-v", "-S"] + base + ["-od", gdir, "-k", "QueryRetrieveLevel=STUDY",
                                                                      "-k", "StudyInstanceUID=2.25.71002", "127.0.0.1", str(scp.port)])
        got = len(os.listdir(gdir))
        record("DCMTK getscu -> SIAARA SCP", p.returncode == 0 and got == 1, f"rc {p.returncode}, {got} files received")
        p = run([os.path.join(DCMTK, "findscu"), "-v", "-W"] + base + ["-k", "ScheduledProcedureStepSequence[0].Modality=CT",
                                                                       "-k", "PatientName", "-k", "PatientID",
                                                                       "127.0.0.1", str(scp.port)])
        n = len(re.findall(r"Find Response: \d+ \(Pending\)", p.stdout + p.stderr))
        record("DCMTK findscu -W (worklist) -> SIAARA SCP", p.returncode == 0 and n == 1, f"{n} items (want 1)")
    finally:
        out, _ = scp.stop()
        dest.stop()
    record("SIAARA SCP statistics after the DCMTK run", "stored=3" in out, out.strip())


def section_pynetdicom_scu():
    try:
        from pynetdicom import AE, evt
        from pynetdicom.sop_class import (ModalityPerformedProcedureStep as MPPS, ModalityWorklistInformationFind as MWL,
                                          StorageCommitmentPushModel as SC, UnifiedProcedureStepPush as UPS_PUSH,
                                          CTImageStorage)
        from pydicom.dataset import Dataset
    except ImportError:
        record("pynetdicom SCUs", False, "pynetdicom not installed", skip=True)
        return
    tmp = tempfile.mkdtemp(prefix="siaara_xc_capinet_pyn_")
    files = make_files(tmp)
    scp, _ = start_scp(tmp)
    try:
        ae = AE(ae_title="PYN_SCU")
        for c in (MPPS, MWL, SC, UPS_PUSH, CTImageStorage):
            ae.add_requested_context(c)
        got = []
        done = threading.Event()

        def on_report(event):
            got.append((event.request.EventTypeID, event.event_information))
            done.set()
            return 0x0000, None

        assoc = ae.associate("127.0.0.1", scp.port, ae_title="SIAARA_SCP", evt_handlers=[(evt.EVT_N_EVENT_REPORT, on_report)])
        if not assoc.is_established:
            record("pynetdicom association to SIAARA SCP", False, "not established")
            return
        import pydicom
        st = assoc.send_c_store(pydicom.dcmread(files[0]))
        record("pynetdicom C-STORE -> SIAARA SCP", st and st.Status == 0, f"status 0x{st.Status:04X}" if st else "none")
        q = Dataset()
        q.PatientName = ""
        q.PatientID = ""
        sps = Dataset()
        sps.Modality = "CT"
        sps.ScheduledStationAETitle = ""
        q.ScheduledProcedureStepSequence = [sps]
        items = [ident for status, ident in assoc.send_c_find(q, MWL) if status and status.Status in (0xFF00, 0xFF01)]
        record("pynetdicom worklist C-FIND -> SIAARA SCP", len(items) == 1 and items[0].PatientID == "W1",
               f"{len(items)} items")
        a = Dataset()
        a.PerformedProcedureStepStatus = "IN PROGRESS"
        a.PerformedProcedureStepID = "PPS1"
        st1, _ = assoc.send_n_create(a, MPPS, "2.25.73001")
        m = Dataset()
        m.PerformedProcedureStepStatus = "COMPLETED"
        st2, _ = assoc.send_n_set(m, MPPS, "2.25.73001")
        st3, _ = assoc.send_n_set(m, MPPS, "2.25.73001")
        record("pynetdicom MPPS N-CREATE / N-SET -> SIAARA SCP",
               st1.Status == 0 and st2.Status == 0 and st3.Status != 0,
               f"create 0x{st1.Status:04X}, set 0x{st2.Status:04X}, set after COMPLETED 0x{st3.Status:04X}")
        rq = Dataset()
        rq.TransactionUID = "2.25.73100"
        r1, r2 = Dataset(), Dataset()
        r1.ReferencedSOPClassUID, r1.ReferencedSOPInstanceUID = CT, "2.25.7100" + "09"
        r2.ReferencedSOPClassUID, r2.ReferencedSOPInstanceUID = CT, "2.25.99999"
        rq.ReferencedSOPSequence = [r1, r2]
        st, _ = assoc.send_n_action(rq, 1, SC, "1.2.840.10008.1.20.1.1")
        done.wait(10)
        ok = st is not None and st.Status == 0 and bool(got)
        detail = f"N-ACTION 0x{st.Status:04X}" if st else "no N-ACTION response"
        if got:
            info = got[0][1]
            ok = ok and got[0][0] == 2 and len(info.ReferencedSOPSequence) == 1 and len(info.FailedSOPSequence) == 1 \
                and info.TransactionUID == "2.25.73100"
            detail += f", N-EVENT-REPORT type {got[0][0]}, committed {len(info.ReferencedSOPSequence)}, " \
                      f"failed {len(info.FailedSOPSequence)}"
        record("pynetdicom Storage Commitment -> SIAARA SCP (report on the same association)", ok, detail)
        u = Dataset()
        u.ProcedureStepState = "SCHEDULED"
        u.PatientName = "Ups^Patient"
        u.ScheduledProcedureStepPriority = "MEDIUM"
        st, _ = assoc.send_n_create(u, UPS_PUSH, "2.25.73200")
        record("pynetdicom UPS Push N-CREATE -> SIAARA SCP", st is not None and st.Status == 0,
               f"status 0x{st.Status:04X}" if st else "none")
        assoc.release()
    finally:
        out, _ = scp.stop()
    record("SIAARA SCP statistics after the pynetdicom run", "mpps=3" in out and "commitments=1" in out, out.strip())


def section_siaara_scu():
    try:
        from pynetdicom import AE, build_role, evt
        from pynetdicom.sop_class import (ModalityPerformedProcedureStep as MPPS, ModalityWorklistInformationFind as MWL,
                                          StorageCommitmentPushModel as SC, StudyRootQueryRetrieveInformationModelFind as SRF)
        from pydicom.dataset import Dataset
    except ImportError:
        record("SIAARA SCUs against pynetdicom", False, "pynetdicom not installed", skip=True)
        return
    # MPPS
    store = {}

    def on_create(event):
        ds = event.attribute_list
        if ds.get("PerformedProcedureStepStatus", "") != "IN PROGRESS":
            return 0x0106, None
        store[event.request.AffectedSOPInstanceUID] = ds
        return 0x0000, ds

    def on_set(event):
        uid = event.request.RequestedSOPInstanceUID
        if uid not in store:
            return 0x0112, None
        if store[uid].PerformedProcedureStepStatus != "IN PROGRESS":
            return 0x0110, None
        store[uid].update(event.modification_list)
        return 0x0000, store[uid]

    def on_find_mwl(event):
        for name, pid, mod in (("Work^One", "W1", "CT"), ("Work^Two", "W2", "MR")):
            want = event.identifier.ScheduledProcedureStepSequence[0].Modality
            if want and want != mod:
                continue
            d = Dataset()
            d.PatientName, d.PatientID = name, pid
            s = Dataset()
            s.Modality = mod
            d.ScheduledProcedureStepSequence = [s]
            yield 0xFF00, d

    def on_find_qr(event):
        for uid in ("2.25.74001", "2.25.74002"):
            d = Dataset()
            d.QueryRetrieveLevel = "STUDY"
            d.StudyInstanceUID = uid
            d.PatientID = event.identifier.PatientID
            yield 0xFF00, d

    def on_find(event):
        handler = on_find_mwl if event.request.AffectedSOPClassUID == MWL else on_find_qr
        yield from handler(event)

    report_port = free_port()

    def on_action(event):
        info = event.action_information
        txn = info.TransactionUID
        refs = list(info.ReferencedSOPSequence)

        def report():
            time.sleep(0.3)
            ae2 = AE(ae_title="PYN_COMMIT")
            ae2.add_requested_context(SC)
            role = build_role(SC, scp_role=True, scu_role=False)
            a2 = ae2.associate("127.0.0.1", report_port, ae_title="SIAARA_RECV", ext_neg=[role])
            if a2.is_established:
                ev = Dataset()
                ev.TransactionUID = txn
                ev.ReferencedSOPSequence = refs
                a2.send_n_event_report(ev, 1, SC, "1.2.840.10008.1.20.1.1")
                a2.release()

        threading.Thread(target=report, daemon=True).start()
        return 0x0000, None

    port = free_port()
    ae = AE(ae_title="PYN_SCP")
    for c in (MPPS, MWL, SC, SRF):
        ae.add_supported_context(c)
    server = ae.start_server(("127.0.0.1", port), block=False, evt_handlers=[
        (evt.EVT_N_CREATE, on_create), (evt.EVT_N_SET, on_set), (evt.EVT_C_FIND, on_find), (evt.EVT_N_ACTION, on_action)])
    try:
        p = run([TOOL, "mpps-scu", "127.0.0.1", str(port), "PYN_SCP"])
        record("SIAARA MPPS SCU (C API) -> pynetdicom", "create=0x0000 set=0x0000 set_again=0x0110" in p.stdout,
               p.stdout.strip() or p.stderr.strip()[-200:])
        p = run([TOOL, "worklist-scu", "127.0.0.1", str(port), "PYN_SCP", "CT"])
        record("SIAARA worklist SCU (C API) -> pynetdicom", "matches=1" in p.stdout, p.stdout.strip() or p.stderr.strip()[-200:])
        p = run([TOOL, "find-scu", "127.0.0.1", str(port), "PYN_SCP", "STUDY", "P9"])
        record("SIAARA Q/R C-FIND SCU (C API, relational proposed) -> pynetdicom", "matches=2" in p.stdout,
               p.stdout.strip() or p.stderr.strip()[-200:])
        p = run([TOOL, "commit-scu", "127.0.0.1", str(port), "PYN_SCP", CT, "2.25.74100", "SIAARA_RECV", str(report_port)])
        record("SIAARA Storage Commitment SCU (C API) <- report on a new association from pynetdicom",
               "committed=1 failed=0" in p.stdout, p.stdout.strip() or p.stderr.strip()[-200:])
    finally:
        server.shutdown()


def main():
    if not os.path.exists(TOOL):
        print(f"missing {TOOL}: build the capi_net_tool target", file=sys.stderr)
        return 2
    for s in SECTIONS:
        try:
            {"dcmtk": section_dcmtk, "pynetdicom-scu": section_pynetdicom_scu, "siaara-scu": section_siaara_scu}[s]()
        except Exception as e:  # noqa: BLE001 - report and continue with the next section
            record(f"section {s}", False, f"{type(e).__name__}: {e}")
    fails = RESULTS.count("FAIL")
    print(f"{len(RESULTS)} checks, {fails} failed, {RESULTS.count('SKIP')} skipped")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
