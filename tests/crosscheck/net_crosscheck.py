#!/usr/bin/env python3
"""Interoperability cross-check of the SIAARA network layer against DCMTK (used only as a test oracle).

Runs, on 127.0.0.1 with free ports:
  1. DCMTK echoscu            -> siaara-storescp
  2. DCMTK storescu           -> siaara-storescp, one run per transfer syntax (native, deflate, RLE,
                                 JPEG lossless, JPEG-LS); received files compared with dcmdump
  3. siaara-echoscu           -> DCMTK storescp
  4. SIAARA SCU (oracle test in siaara_tests) C-ECHO + C-STORE -> DCMTK storescp
  5. DCMTK findscu/getscu/movescu -> siaara-storescp --qr-dir (C-MOVE destination: DCMTK storescp)
  6. SIAARA SCU (oracle test) C-FIND/C-GET/C-MOVE -> DCMTK dcmqrscp (move destination: siaara-storescp)

Usage: net_crosscheck.py [--build BUILD_DIR]
Requires DCMTK command-line tools (apt-get install dcmtk). Exit status 0 when every check passes.
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

RESULTS = []


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(cmd, timeout=60, env=None):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, env=env)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (("  [" + detail + "]") if detail else ""), flush=True)


def wait_port(port, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def start(cmd, port):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if not wait_port(port):
        p.kill()
        raise RuntimeError("server did not start: " + " ".join(cmd))
    return p


def stop(p):
    p.terminate()
    try:
        out, _ = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
    return out.decode("utf-8", "replace")


def dataset_dump(path):
    """dcmdump of the data set only (group 0002 and file-level comments removed)."""
    rc, out = run(["dcmdump", "-q", "-Un", "+L", path])
    lines = []
    in_meta = False
    for line in out.splitlines():
        if line.startswith("# Dicom-Meta-Information-Header"):
            in_meta = True
            continue
        if line.startswith("# Dicom-Data-Set"):
            in_meta = False
            continue
        if in_meta or line.startswith("#") or not line.strip():
            continue
        lines.append(line)
    return rc, lines


def meta_ts(path):
    rc, out = run(["dcmdump", "-q", "-Un", "+P", "0002,0010", path])
    m = re.search(r"\[([0-9.]+)\]", out)
    return m.group(1) if m else ""


DUMP = """(0008,0016) UI =CTImageStorage
(0008,0018) UI [{uid}]
(0008,0060) CS [CT]
(0010,0010) PN [Cross^Check]
(0010,0020) LO [XC1]
(0020,000d) UI [2.25.8000]
(0020,000e) UI [2.25.7000]
(0028,0002) US 1
(0028,0004) CS [MONOCHROME2]
(0028,0010) US 16
(0028,0011) US 16
(0028,0100) US 16
(0028,0101) US 16
(0028,0102) US 15
(0028,0103) US 0
(7fe0,0010) OW {pixels}
"""


def make_files(work):
    """Creates one CT instance per transfer syntax. Returns list of (label, ts_uid, storescu_flag, path)."""
    src = os.path.join(work, "src")
    os.makedirs(src)
    specs = [
        ("explicit-le", "1.2.840.10008.1.2.1", ["dcmconv", "+te"], "-xe"),
        ("implicit-le", "1.2.840.10008.1.2", ["dcmconv", "+ti"], "-xi"),
        ("explicit-be", "1.2.840.10008.1.2.2", ["dcmconv", "+tb"], "-xb"),
        ("deflated", "1.2.840.10008.1.2.1.99", ["dcmconv", "+td"], "-xd"),
        ("rle", "1.2.840.10008.1.2.5", ["dcmcrle"], "-xr"),
        ("jpeg-lossless-sv1", "1.2.840.10008.1.2.4.70", ["dcmcjpeg", "+e1"], "-xs"),
        ("jpeg-ls-lossless", "1.2.840.10008.1.2.4.80", ["dcmcjpls", "+el"], "-xt"),
    ]
    out = []
    for i, (label, ts, conv, flag) in enumerate(specs):
        pixels = "\\".join("%04x" % ((k * 37 + i * 101) % 4096) for k in range(256))
        txt = os.path.join(src, label + ".txt")
        with open(txt, "w") as f:
            f.write(DUMP.format(uid="2.25.5000%d" % i, pixels=pixels))
        base = os.path.join(src, label + "_base.dcm")
        rc, o = run(["dump2dcm", "+te", txt, base])
        if rc != 0:
            raise RuntimeError("dump2dcm failed: " + o)
        path = os.path.join(src, label + ".dcm")
        rc, o = run(conv + [base, path])
        if rc != 0 or meta_ts(path) != ts:
            raise RuntimeError("cannot make %s file: %s (ts %s)" % (label, o, meta_ts(path)))
        os.remove(base)
        out.append((label, ts, flag, path))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(os.path.dirname(__file__), "..", "..", "build"))
    args = ap.parse_args()
    build = os.path.abspath(args.build)
    storescp_bin = os.path.join(build, "tools", "siaara-storescp")
    echoscu_bin = os.path.join(build, "tools", "siaara-echoscu")
    tests_bin = os.path.join(build, "tests", "siaara_tests")
    for tool in ["echoscu", "storescu", "storescp", "findscu", "getscu", "movescu", "dcmqrscp", "dcmqridx",
                 "dump2dcm", "dcmconv", "dcmdump", "dcmcrle", "dcmcjpeg", "dcmcjpls"]:
        if not shutil.which(tool):
            print("DCMTK tool %s not found; install dcmtk" % tool)
            return 2
    work = tempfile.mkdtemp(prefix="siaara_netx_")
    files = make_files(work)
    native = [f for f in files if f[0] in ("explicit-le", "implicit-le", "explicit-be", "deflated")]

    # ---------------------------------------------------------------- 1 + 2: DCMTK SCU -> SIAARA SCP
    rx = os.path.join(work, "rx")
    os.makedirs(rx)
    port = free_port()
    scp = start([storescp_bin, "--allow-any", "-aet", "SIAARA_SCP", "-od", rx, "--all-ts", "-v", str(port)], port)
    rc, out = run(["echoscu", "-v", "-aec", "SIAARA_SCP", "127.0.0.1", str(port)])
    check("DCMTK echoscu -> siaara-storescp", rc == 0 and "Received Echo Response (Success)" in out)
    rc, out = run(["echoscu", "-aec", "SIAARA_SCP", "--max-pdu", "4096", "127.0.0.1", str(port)])
    check("DCMTK echoscu (max PDU 4096) -> siaara-storescp", rc == 0)
    for label, ts, flag, path in files:
        rc, out = run(["storescu", "-v", flag, "-aec", "SIAARA_SCP", "127.0.0.1", str(port), path])
        uid = "2.25.5000%d" % [f[0] for f in files].index(label)
        got = os.path.join(rx, uid + ".dcm")
        ok = rc == 0 and "Received Store Response (Success)" in out and os.path.exists(got)
        detail = "storescu rc %d" % rc
        if ok:
            rts = meta_ts(got)
            _, a = dataset_dump(path)
            _, b = dataset_dump(got)
            ok = rts == ts and a == b
            detail = "received ts %s, data set dump %s" % (rts, "identical" if a == b else "DIFFERS")
        check("DCMTK storescu %s -> siaara-storescp (%s)" % (flag, label), ok, detail)
    # several files on one association, small PDUs
    rc, out = run(["storescu", "-aec", "SIAARA_SCP", "--max-pdu", "4096", "127.0.0.1", str(port)]
                  + [f[3] for f in native])
    check("DCMTK storescu 4 native files, one association, max PDU 4096", rc == 0, "rc %d" % rc)
    rc, out = run(["echoscu", "-aec", "WRONG_AE", "127.0.0.1", str(port)])
    check("DCMTK echoscu with wrong called AE is rejected", rc != 0 and "Called AE Title Not Recognized" in out)
    stop(scp)

    # ---------------------------------------------------------------- 3 + 4: SIAARA SCU -> DCMTK storescp
    drx = os.path.join(work, "dcmtk_rx")
    os.makedirs(drx)
    dport = free_port()
    dscp = start(["storescp", "-aet", "STORESCP", "-od", drx, "+B", str(dport)], dport)
    rc, out = run([echoscu_bin, "-aec", "STORESCP", "-r", "3", "127.0.0.1", str(dport)])
    check("siaara-echoscu -> DCMTK storescp", rc == 0 and out.count("status 0x0000") == 3, out.strip().replace("\n", "; "))
    rc, out = run([echoscu_bin, "-aec", "STORESCP", "--max-pdu", "1024", "127.0.0.1", str(dport)])
    check("siaara-echoscu (max PDU 1024) -> DCMTK storescp", rc == 0)
    env = dict(os.environ, SIAARA_NET_ORACLE_STORE_PORT=str(dport), SIAARA_NET_ORACLE_STORE_AE="STORESCP")
    rc, out = run([tests_bin, "net/oracle: C-STORE"], env=env, timeout=120)
    time.sleep(0.5)
    received = sorted(os.listdir(drx))
    ok = rc == 0 and "1 test cases, 0 failed checks" in out and len(received) == 3
    if ok:
        for name in received:
            r2, dump = run(["dcmdump", "-q", os.path.join(drx, name)])
            ok = ok and r2 == 0 and "Loop^Back" in dump
    check("SIAARA SCU C-ECHO + C-STORE (explicit LE, implicit LE, explicit BE) -> DCMTK storescp", ok,
          "%d files received, test rc %d" % (len(received), rc))
    stop(dscp)

    # ---------------------------------------------------------------- 5: DCMTK Q/R SCUs -> SIAARA SCP
    qr = os.path.join(work, "qr")
    os.makedirs(qr)
    for f in native:
        shutil.copy(f[3], qr)
    mrx = os.path.join(work, "move_rx")
    os.makedirs(mrx)
    mport = free_port()
    mscp = start(["storescp", "-aet", "DCMDEST", "-od", mrx, str(mport)], mport)
    qport = free_port()
    qscp = start([storescp_bin, "--allow-any", "-aet", "SIAARA_QR", "--qr-dir", qr, "--move-dest", "DCMDEST=127.0.0.1:%d" % mport,
                  "-v", str(qport)], qport)
    rc, out = run(["findscu", "-v", "-S", "-aec", "SIAARA_QR", "-k", "QueryRetrieveLevel=IMAGE",
                   "-k", "StudyInstanceUID=2.25.8000", "-k", "SeriesInstanceUID=2.25.7000", "-k", "SOPInstanceUID",
                   "-k", "PatientName", "127.0.0.1", str(qport)])
    pend = len(re.findall(r"Find Response: \d+ \(Pending\)", out))
    check("DCMTK findscu (study root, IMAGE level) -> SIAARA SCP", rc == 0 and pend == len(native) and
          "Final Find Response (Success)" in out, "%d pending responses" % pend)
    rc, out = run(["findscu", "-v", "-P", "-aec", "SIAARA_QR", "-k", "QueryRetrieveLevel=PATIENT", "-k", "PatientID",
                   "-k", "PatientName=Cross*", "127.0.0.1", str(qport)])
    pend = len(re.findall(r"\(Pending\)", out))
    check("DCMTK findscu (patient root, wildcard) -> SIAARA SCP", rc == 0 and pend == 1, "%d pending" % pend)
    rc, out = run(["findscu", "-v", "-S", "--cancel", "1", "-aec", "SIAARA_QR", "-k", "QueryRetrieveLevel=IMAGE",
                   "-k", "StudyInstanceUID=2.25.8000", "-k", "SeriesInstanceUID=2.25.7000", "-k", "SOPInstanceUID",
                   "127.0.0.1", str(qport)])
    final = re.findall(r"Final Find Response \((\w+)\)", out)
    check("DCMTK findscu --cancel 1 -> SIAARA SCP (association survives, final status reported)",
          rc == 0 and bool(final) and final[-1] in ("Cancel", "Success"), "final status " + (final[-1] if final else "?"))
    grx = os.path.join(work, "get_rx")
    os.makedirs(grx)
    rc, out = run(["getscu", "-v", "-S", "-aec", "SIAARA_QR", "-od", grx, "-k", "QueryRetrieveLevel=STUDY",
                   "-k", "StudyInstanceUID=2.25.8000", "127.0.0.1", str(qport)], timeout=120)
    got = os.listdir(grx)
    check("DCMTK getscu -> SIAARA SCP (C-STORE sub-operations on the same association)",
          rc == 0 and len(got) == len(native), "%d instances received, rc %d" % (len(got), rc))
    rc, out = run(["movescu", "-v", "-S", "-aec", "SIAARA_QR", "-aem", "DCMDEST", "-k", "QueryRetrieveLevel=STUDY",
                   "-k", "StudyInstanceUID=2.25.8000", "127.0.0.1", str(qport)], timeout=120)
    time.sleep(0.5)
    moved = os.listdir(mrx)
    check("DCMTK movescu -> SIAARA SCP -> DCMTK storescp", rc == 0 and len(moved) == len(native),
          "%d instances at destination, rc %d" % (len(moved), rc))
    rc, out = run(["movescu", "-S", "-aec", "SIAARA_QR", "-aem", "NOWHERE", "-k", "QueryRetrieveLevel=STUDY",
                   "-k", "StudyInstanceUID=2.25.8000", "127.0.0.1", str(qport)], timeout=60)
    check("DCMTK movescu to unknown destination -> status A801", "MoveDestinationUnknown" in out or "a801" in out.lower(),
          "rc %d" % rc)
    stop(qscp)
    stop(mscp)

    # ---------------------------------------------------------------- 6: SIAARA SCU -> DCMTK dcmqrscp
    db = os.path.join(work, "qrdb")
    os.makedirs(db)
    srx = os.path.join(work, "siaara_move_rx")
    os.makedirs(srx)
    sport = free_port()
    sdest = start([storescp_bin, "--allow-any", "-aet", "SIAARA_DEST", "-od", srx, str(sport)], sport)
    qrport = free_port()
    cfg = os.path.join(work, "dcmqrscp.cfg")
    with open(cfg, "w") as f:
        f.write("NetworkTCPPort = %d\nMaxPDUSize = 16384\nMaxAssociations = 16\n" % qrport)
        f.write("HostTable BEGIN\nsiaaradest = (SIAARA_DEST, 127.0.0.1, %d)\nHostTable END\n" % sport)
        f.write("VendorTable BEGIN\nVendorTable END\n")
        f.write("AETable BEGIN\nQRSCP %s RW (200, 1024mb) ANY\nAETable END\n" % db)
    rc, out = run(["dcmqridx", db] + [f[3] for f in native])
    if rc != 0:
        check("dcmqridx", False, out)
    qrp = start(["dcmqrscp", "-c", cfg, str(qrport)], qrport)
    rc, out = run([echoscu_bin, "-aec", "QRSCP", "127.0.0.1", str(qrport)])
    check("siaara-echoscu -> DCMTK dcmqrscp", rc == 0)
    env = dict(os.environ, SIAARA_NET_ORACLE_QR_PORT=str(qrport), SIAARA_NET_ORACLE_QR_AE="QRSCP",
               SIAARA_NET_ORACLE_MOVE_DEST="SIAARA_DEST", SIAARA_NET_ORACLE_QR_COUNT=str(len(native)))
    rc, out = run([tests_bin, "net/oracle: C-FIND"], env=env, timeout=180)
    time.sleep(0.5)
    moved = os.listdir(srx)
    lines = [l.strip() for l in out.splitlines() if "oracle" in l]
    check("SIAARA SCU C-FIND/C-GET/C-MOVE -> DCMTK dcmqrscp (move to siaara-storescp)",
          rc == 0 and "1 test cases, 0 failed checks" in out and len(moved) == len(native),
          "; ".join(lines) + "; %d instances at SIAARA destination" % len(moved))
    if rc != 0:
        print(out)
    stop(qrp)
    stop(sdest)

    shutil.rmtree(work, ignore_errors=True)
    failed = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed" % (len(RESULTS), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
