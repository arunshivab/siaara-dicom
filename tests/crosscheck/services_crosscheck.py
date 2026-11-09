#!/usr/bin/env python3
"""Interoperability checks for transcoding and the DIMSE services (test-only, never shipped).

Third-party software is used purely as an oracle:
  - DCMTK (/usr/bin: storescu, storescp, findscu, wlmscpfs, dcmprscp, dcmdjpeg, dcmdjpls, dcmdrle)
  - OpenJPEG opj_decompress (JPEG 2000 / HTJ2K codestreams)
  - pynetdicom + pydicom (Storage Commitment, MPPS, UPS)
DCMTK binaries are called by absolute path: pynetdicom installs scripts with the same names.

usage: services_crosscheck.py BUILD_DIR [section ...]
  sections: transcode dcmtk-store dcmtk-mwl dcmtk-print commitment mpps ups (default: all)
Prints one line per check (PASS/FAIL/SKIP) and the exact commands it ran.
"""
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else "build"
SECTIONS = sys.argv[2:] or ["transcode", "dcmtk-store", "dcmtk-mwl", "dcmtk-print", "commitment", "mpps", "ups"]
TOOL = os.path.join(BUILD, "tests", "services_tool")
STORESCU = os.path.join(BUILD, "tools", "siaara-storescu")
FINDSCU = os.path.join(BUILD, "tools", "siaara-findscu")
DCMTK = "/usr/bin"
RESULTS = []
VERBOSE = os.environ.get("SIAARA_XCHECK_VERBOSE") == "1"

TS = {
    "explicit": "1.2.840.10008.1.2.1", "implicit": "1.2.840.10008.1.2", "big": "1.2.840.10008.1.2.2",
    "deflate": "1.2.840.10008.1.2.1.99", "rle": "1.2.840.10008.1.2.5", "baseline": "1.2.840.10008.1.2.4.50",
    "extended": "1.2.840.10008.1.2.4.51", "lossless": "1.2.840.10008.1.2.4.57", "sv1": "1.2.840.10008.1.2.4.70",
    "jls": "1.2.840.10008.1.2.4.80", "jls-near": "1.2.840.10008.1.2.4.81", "j2k": "1.2.840.10008.1.2.4.90",
    "j2k-any": "1.2.840.10008.1.2.4.91", "htj2k": "1.2.840.10008.1.2.4.201", "htj2k-rpcl": "1.2.840.10008.1.2.4.202",
    "htj2k-any": "1.2.840.10008.1.2.4.203",
}


def record(name, ok, detail="", skip=False):
    status = "SKIP" if skip else ("PASS" if ok else "FAIL")
    RESULTS.append((status, name, detail))
    print(f"{status} {name}{(' : ' + detail) if detail else ''}", flush=True)


def run(cmd, timeout=60, check=False, **kw):
    if VERBOSE:
        print("  $ " + " ".join(cmd), flush=True)
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, text=True, **kw)
    if check and p.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed ({p.returncode}): {p.stderr.strip()[:400]}")
    return p


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_port_file(path, proc, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path):
            txt = open(path).read().strip()
            if txt:
                return int(txt)
        if proc.poll() is not None:
            raise RuntimeError("server exited: " + proc.stderr.read().decode(errors="replace")[:400])
        time.sleep(0.05)
    raise RuntimeError("server did not start")


def wait_listen(port, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"nothing listening on {port}")


class Server:
    """A background process; stopped with SIGTERM."""

    def __init__(self, cmd, port_file=None, port=None):
        if VERBOSE:
            print("  $ " + " ".join(cmd) + " &", flush=True)
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.port = wait_port_file(port_file, self.proc) if port_file else port
        if port is not None and not port_file:
            wait_listen(port)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        out, err = self.proc.communicate()
        return out.decode(errors="replace"), err.decode(errors="replace")


def frames_of(path, tmp):
    out = os.path.join(tmp, os.path.basename(path) + ".raw")
    p = run([TOOL, "frames", path, out], check=True)
    return open(out, "rb").read(), p.stdout.split()


def psnr(a, b, bytes_per_sample, peak):
    import math
    n = min(len(a), len(b)) // bytes_per_sample
    se = 0
    for i in range(n):
        if bytes_per_sample == 1:
            x, y = a[i], b[i]
        else:
            x = a[2 * i] | (a[2 * i + 1] << 8)
            y = b[2 * i] | (b[2 * i + 1] << 8)
        se += (x - y) * (x - y)
    if se == 0:
        return 99.0
    return 10 * math.log10(peak * peak / (se / n))


def read_pnm(path):
    """PGM/PPM (P5/P6) -> little-endian samples, interleaved."""
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    maxval = int(fields[3])
    body = data[pos:]
    if maxval < 256:
        return body
    out = bytearray(len(body))
    out[0::2] = body[1::2]
    out[1::2] = body[0::2]
    return bytes(out)


# ===================================================================== transcoder vs. decoders

def oracle_decode(path, ts, tmp, kind):
    """Decodes `path` (in transfer syntax ts) with a third-party decoder; returns interleaved frames."""
    import pydicom
    from pydicom.encaps import generate_frames
    if ts in (TS["rle"],):
        tool = "dcmdrle"
    elif ts in (TS["baseline"], TS["extended"], TS["lossless"], TS["sv1"]):
        tool = "dcmdjpeg"
    elif ts in (TS["jls"], TS["jls-near"]):
        tool = "dcmdjpls"
    else:
        tool = None
    if tool:
        out = path + "." + tool + ".dcm"
        run([os.path.join(DCMTK, tool), path, out], check=True)
        return frames_of(out, tmp)[0], tool
    ds = pydicom.dcmread(path)
    nf = int(getattr(ds, "NumberOfFrames", 1) or 1)
    raw = b""
    for i, fr in enumerate(generate_frames(ds.PixelData, number_of_frames=nf)):
        cs = os.path.join(tmp, f"f{i}.j2k")
        open(cs, "wb").write(fr)
        pnm = os.path.join(tmp, f"f{i}." + ("ppm" if kind == "rgb8" else "pgm"))
        run(["opj_decompress", "-i", cs, "-o", pnm], check=True)
        raw += read_pnm(pnm)
    return raw, "opj_decompress"


def section_transcode():
    import pydicom  # noqa: F401
    tmp = tempfile.mkdtemp(prefix="siaara_xc_tc_")
    lossy = {"baseline", "extended"}
    for kind, frames in (("gray8", 2), ("gray12", 1), ("gray16", 2), ("rgb8", 2)):
        src = os.path.join(tmp, f"{kind}.dcm")
        run([TOOL, "make-image", src, kind, "37", "29", str(frames), "2.25.4711"], check=True)
        original, _ = frames_of(src, tmp)
        bps = 1 if kind in ("gray8", "rgb8") else 2
        peak = {"gray8": 255, "rgb8": 255, "gray12": 4095, "gray16": 65535}[kind]
        for name in ("rle", "lossless", "sv1", "baseline", "extended", "jls", "jls-near", "j2k", "j2k-any",
                     "htj2k", "htj2k-rpcl", "htj2k-any"):
            ts = TS[name]
            if name == "baseline" and kind not in ("gray8", "rgb8"):
                continue
            if name == "extended" and kind == "gray16":
                continue
            out = os.path.join(tmp, f"{kind}.{name}.dcm")
            args = [TOOL, "transcode", src, out, ts] + (["--lossy"] if name in ("jls-near", "j2k-any") else [])
            p = run(args)
            label = f"transcode {kind} x{frames} -> {name} ({ts})"
            if p.returncode != 0:
                record(label, False, p.stderr.strip()[:200])
                continue
            try:
                decoded, tool = oracle_decode(out, ts, tmp, kind)
            except Exception as e:  # noqa: BLE001
                record(label, False, f"oracle failed: {e}")
                continue
            if name in lossy or name in ("j2k-any",):
                v = psnr(decoded, original, bps, peak)
                record(label + f" decoded by {tool}", v >= 30.0 and len(decoded) == len(original), f"PSNR {v:.1f} dB")
            elif name == "jls-near":
                n = len(original) // bps
                me = max(abs((decoded[i] if bps == 1 else decoded[2 * i] | decoded[2 * i + 1] << 8) -
                             (original[i] if bps == 1 else original[2 * i] | original[2 * i + 1] << 8))
                         for i in range(n))
                record(label + f" decoded by {tool}", me <= 2 and len(decoded) == len(original), f"max error {me} (NEAR 2)")
            else:
                record(label + f" decoded by {tool}", decoded == original, "bit exact" if decoded == original else
                       f"differs ({len(decoded)} vs {len(original)} bytes)")
    shutil.rmtree(tmp, ignore_errors=True)


# ===================================================================== DCMTK storage

DCMTK_DECODER = {TS["rle"]: "dcmdrle", TS["sv1"]: "dcmdjpeg", TS["lossless"]: "dcmdjpeg", TS["baseline"]: "dcmdjpeg",
                 TS["extended"]: "dcmdjpeg", TS["jls"]: "dcmdjpls", TS["jls-near"]: "dcmdjpls"}


def stored_ts(path):
    import pydicom
    return str(pydicom.dcmread(path, stop_before_pixels=True).file_meta.TransferSyntaxUID)


def section_dcmtk_store():
    tmp = tempfile.mkdtemp(prefix="siaara_xc_store_")
    src = os.path.join(tmp, "gray16.dcm")
    run([TOOL, "make-image", src, "gray16", "64", "48", "2", "2.25.5001"], check=True)
    rgb = os.path.join(tmp, "rgb8.dcm")
    run([TOOL, "make-image", rgb, "rgb8", "40", "30", "1", "2.25.5002"], check=True)
    g12 = os.path.join(tmp, "gray12.dcm")
    run([TOOL, "make-image", g12, "gray12", "40", "30", "1", "2.25.5003"], check=True)
    sv1 = os.path.join(tmp, "gray16.sv1.dcm")
    run([TOOL, "transcode", src, sv1, TS["sv1"]], check=True)
    j2k = os.path.join(tmp, "gray16.j2k.dcm")
    run([TOOL, "transcode", src, j2k, TS["j2k"]], check=True)
    orig16, _ = frames_of(src, tmp)

    # 1. DCMTK storescu (various proposals) -> our SCP storing in a preferred syntax (JPEG-LS lossless).
    for flag, f in (("-xs", sv1), ("-xv", j2k), ("-xw", src), ("-x=", src), ("-xs", src)):
        out = tempfile.mkdtemp(dir=tmp)
        pf = os.path.join(out, "port")
        srv = Server([TOOL, "storescp", pf, out, "--preferred", TS["jls"]], port_file=pf)
        cmd = [os.path.join(DCMTK, "storescu"), flag, "-aec", "SIAARA_SCP", "127.0.0.1", str(srv.port), f]
        p = run(cmd)
        srv.stop()
        files = [x for x in os.listdir(out) if x.endswith(".dcm")]
        ok = p.returncode == 0 and len(files) == 1
        detail = " ".join(cmd)
        if ok:
            got = os.path.join(out, files[0])
            ts = stored_ts(got)
            decoded, _ = frames_of(got, tmp)
            ok = ts == TS["jls"] and decoded == orig16
            detail += f" -> stored {ts}, pixels {'identical' if decoded == orig16 else 'DIFFER'}"
            # the stored JPEG-LS file decodes with DCMTK too
            dj = got + ".dcmdjpls.dcm"
            if run([os.path.join(DCMTK, "dcmdjpls"), got, dj]).returncode == 0:
                ok = ok and frames_of(dj, tmp)[0] == orig16
                detail += ", dcmdjpls decodes it identically" if frames_of(dj, tmp)[0] == orig16 else ", dcmdjpls DIFFERS"
        else:
            detail += f" rc={p.returncode} {p.stderr.strip()[-300:]}"
        record(f"DCMTK storescu {flag} ({os.path.basename(f)}) -> SIAARA SCP preferred JPEG-LS", ok, detail)

    # 2. our storescu --transcode -> DCMTK storescp accepting one syntax.
    cases = [("+xs", "sv1", src, []), ("+xy", "baseline", rgb, ["--allow-lossy"]), ("+xx", "extended", g12, ["--allow-lossy"]),
             ("+xv", "j2k", src, []), ("+xw", "j2k-any", rgb, ["--allow-lossy", "--lossy"]), ("+xt", "jls", src, []),
             ("+xu", "jls-near", src, ["--allow-lossy", "--lossy"]), ("+xr", "rle", rgb, []), ("+xb", "big", src, []),
             ("+xd", "deflate", src, []), ("+xs", "sv1", rgb, [])]
    for flag, name, f, extra in cases:
        out = tempfile.mkdtemp(dir=tmp)
        port = free_port()
        srv = Server([os.path.join(DCMTK, "storescp"), flag, "-od", out, str(port)], port=port)
        cmd = [STORESCU, "--transcode", TS[name]] + extra + ["127.0.0.1", str(port), f]
        p = run(cmd)
        srv.stop()
        files = [x for x in os.listdir(out)]
        ok = p.returncode == 0 and len(files) == 1
        detail = " ".join(cmd) + f" vs storescp {flag}"
        if ok:
            got = os.path.join(out, files[0])
            ts = stored_ts(got)
            ok = ts == TS[name]
            original, _ = frames_of(f, tmp)
            dec = DCMTK_DECODER.get(ts)
            if dec:
                plain = got + ".plain.dcm"
                ok = ok and run([os.path.join(DCMTK, dec), got, plain]).returncode == 0
                decoded = frames_of(plain, tmp)[0] if ok else b""
                via = dec
            elif ts in (TS["j2k"], TS["j2k-any"]):
                decoded, via = oracle_decode(got, ts, tmp, "rgb8" if f == rgb else "gray")
            else:
                decoded, via = frames_of(got, tmp)[0], "siaara (native)"
            bps = 1 if f == rgb else 2
            if name in ("baseline", "extended", "j2k-any"):
                v = psnr(decoded, original, bps, 255 if bps == 1 else (4095 if f == g12 else 65535))
                ok = ok and v >= 30
                detail += f" -> stored {ts}; {via} PSNR {v:.1f} dB"
            elif name == "jls-near":
                n = len(original) // bps
                me = max(abs((decoded[2 * i] | decoded[2 * i + 1] << 8) - (original[2 * i] | original[2 * i + 1] << 8))
                         for i in range(n))
                ok = ok and me <= 2
                detail += f" -> stored {ts}; {via} max error {me}"
            else:
                ok = ok and decoded == original
                detail += f" -> stored {ts}; {via} {'bit exact' if decoded == original else 'DIFFERS'}"
        else:
            detail += f" rc={p.returncode} {p.stderr.strip()[-300:]}"
        record(f"SIAARA storescu --transcode {name} -> DCMTK storescp {flag}", ok, detail)
    shutil.rmtree(tmp, ignore_errors=True)


# ===================================================================== Modality Worklist

def make_wl(path, name, pid, station, modality, date):
    import pydicom
    from pydicom.dataset import Dataset, FileMetaDataset
    from pydicom.sequence import Sequence
    ds = Dataset()
    ds.SpecificCharacterSet = "ISO_IR 100"
    ds.PatientName = name
    ds.PatientID = pid
    ds.AccessionNumber = "A" + pid
    ds.StudyInstanceUID = "2.25.88" + pid[1:]
    ds.RequestedProcedureID = "RP" + pid
    sps = Dataset()
    sps.ScheduledStationAETitle = station
    sps.ScheduledProcedureStepStartDate = date
    sps.ScheduledProcedureStepStartTime = "093000"
    sps.Modality = modality
    sps.ScheduledProcedureStepID = "S" + pid
    sps.ScheduledProcedureStepDescription = "step"
    ds.ScheduledProcedureStepSequence = Sequence([sps])
    ds.file_meta = FileMetaDataset()
    ds.file_meta.TransferSyntaxUID = pydicom.uid.ExplicitVRLittleEndian
    ds.file_meta.MediaStorageSOPClassUID = "1.2.840.10008.5.1.4.31"
    ds.file_meta.MediaStorageSOPInstanceUID = "2.25.99" + pid[1:]
    ds.save_as(path, enforce_file_format=True)


def section_dcmtk_mwl():
    tmp = tempfile.mkdtemp(prefix="siaara_xc_mwl_")
    data = os.path.join(tmp, "WLDATA")
    os.makedirs(os.path.join(data, "WLMSCP"))
    open(os.path.join(data, "WLMSCP", "lockfile"), "w").close()
    make_wl(os.path.join(data, "WLMSCP", "1.wl"), "DOE^JOHN", "P1", "CT1", "CT", "20261109")
    make_wl(os.path.join(data, "WLMSCP", "2.wl"), "ROE^JANE", "P2", "CT1", "CT", "20261110")
    make_wl(os.path.join(data, "WLMSCP", "3.wl"), "POE^ANN", "P3", "MR1", "MR", "20261109")
    port = free_port()
    # -dfr: the synthetic worklist files carry only the attributes the queries use.
    srv = Server([os.path.join(DCMTK, "wlmscpfs"), "-dfp", data, "-dfr", str(port)], port=port)
    for station, date, want in (("CT1", "", ["DOE^JOHN", "ROE^JANE"]), ("", "20261109", ["DOE^JOHN", "POE^ANN"]),
                                ("MR1", "20261109", ["POE^ANN"]), ("XX", "", [])):
        cmd = [TOOL, "mwlscu", "127.0.0.1", str(port), "WLMSCP", station, date]
        p = run(cmd)
        got = p.stdout.strip()
        expect = f"status=0x0000 matches={len(want)}" + "".join(" " + n for n in sorted(want))
        record(f"SIAARA MWL SCU -> DCMTK wlmscpfs (station '{station}', date '{date}')", got == expect,
               " ".join(cmd) + f" -> {got or p.stderr.strip()[:200]}")
    # siaara-findscu -W against wlmscpfs, too
    cmd = [FINDSCU, "-W", "-aec", "WLMSCP", "--modality", "CT", "-q", "127.0.0.1", str(port)]
    p = run(cmd)
    record("siaara-findscu -W -> DCMTK wlmscpfs", p.returncode == 0 and "2 match(es)" in p.stdout,
           " ".join(cmd) + " -> " + p.stdout.strip())
    srv.stop()

    # DCMTK findscu -W -> our MWL SCP
    pf = os.path.join(tmp, "port")
    srv = Server([TOOL, "mwlscp", pf], port_file=pf)
    for keys, want in (([("ScheduledProcedureStepSequence[0].ScheduledStationAETitle", "CT1")], 2),
                       ([("ScheduledProcedureStepSequence[0].ScheduledProcedureStepStartDate", "20261109")], 2),
                       ([("PatientName", "R*")], 1),
                       ([("ScheduledProcedureStepSequence[0].Modality", "MR"),
                         ("ScheduledProcedureStepSequence[0].ScheduledProcedureStepStartDate", "20261101-20261130")], 1)):
        outd = tempfile.mkdtemp(dir=tmp)
        cmd = [os.path.join(DCMTK, "findscu"), "-W", "-aec", "SIAARA_MWL", "-X", "-od", outd,
               "-k", "PatientName", "-k", "PatientID", "-k", "ScheduledProcedureStepSequence[0].Modality",
               "-k", "ScheduledProcedureStepSequence[0].ScheduledStationAETitle",
               "-k", "ScheduledProcedureStepSequence[0].ScheduledProcedureStepStartDate"]
        for k, v in keys:
            cmd += ["-k", f"{k}={v}"]
        cmd += ["127.0.0.1", str(srv.port)]
        p = run(cmd)
        n = len([x for x in os.listdir(outd) if x.startswith("rsp")])
        record(f"DCMTK findscu -W -> SIAARA MWL SCP ({', '.join(k.split('.')[-1] + '=' + v for k, v in keys)})",
               p.returncode == 0 and n == want, " ".join(cmd) + f" -> {n} response file(s), expected {want}")
    srv.stop()
    shutil.rmtree(tmp, ignore_errors=True)


# ===================================================================== print

def section_dcmtk_print():
    tmp = tempfile.mkdtemp(prefix="siaara_xc_print_")
    port = free_port()
    cfg_src = "/etc/dcmtk/dcmpstat.cfg"
    if not os.path.exists(os.path.join(DCMTK, "dcmprscp")) or not os.path.exists(cfg_src):
        record("Print SCU -> DCMTK dcmprscp", False, "dcmprscp or dcmpstat.cfg not installed", skip=True)
        return
    # A minimal configuration: one grayscale printer at `port`, spool in tmp.
    cfg = os.path.join(tmp, "dcmpstat.cfg")
    db = os.path.join(tmp, "db")
    os.makedirs(db)
    open(os.path.join(db, "index.dat"), "wb").close()
    open(cfg, "w").write(f"""[[GENERAL]]
[[DATABASE]]
Directory = {db}
[[NETWORK]]
AETitle = SIAARA_XC
[[PRINT]]
Spooler = PRINTSPOOLER
Printer = PRINT_SCP
Directory = {tmp}
Detailedlog = true
[[COMMUNICATION]]
[PRINT_SCP]
hostname = 127.0.0.1
type = LOCALPRINTER
description = crosscheck printer
port = {port}
aetitle = PRINT_SCP
DisableNewVRs = false
FilmDestination = MAGAZINE\\PROCESSOR
SupportsPresentationLUT = false
MaxPDU = 16384
Supports12Bit = true
SupportsImageSize = false
SupportsDecimateCrop = false
SupportsTrim = false
DisplayFormat=1,1\\2,1\\1,2\\2,2
FilmSizeID = 8INX10IN\\11INX14IN
MediumType = PAPER\\BLUE FILM\\CLEAR FILM
MagnificationType = REPLICATE\\BILINEAR\\CUBIC
SmoothingType = NONE
BorderDensity = BLACK\\WHITE
EmptyImageDensity = BLACK\\WHITE
MaxDensity = 320
MinDensity = 20
Annotation = false
Configuration_1 = default
""")
    cmd_srv = [os.path.join(DCMTK, "dcmprscp"), "-c", cfg, "-p", "PRINT_SCP"]
    srv = Server(cmd_srv, port=None)
    try:
        wait_listen(port, 10)
    except RuntimeError as e:
        out, err = srv.stop()
        record("Print SCU -> DCMTK dcmprscp", False, f"dcmprscp did not start: {err.strip()[-300:]}")
        return
    cmd = [TOOL, "print-scu", "127.0.0.1", str(port), "PRINT_SCP"]
    p = run(cmd)
    out, err = srv.stop()
    ok = p.returncode == 0 and "print=ok" in p.stdout and "printer=NORMAL" in p.stdout
    record("SIAARA Basic Grayscale Print SCU -> DCMTK dcmprscp", ok,
           " ".join(cmd) + " -> " + (p.stdout.strip() or p.stderr.strip()[-300:]))
    shutil.rmtree(tmp, ignore_errors=True)


# ===================================================================== pynetdicom: storage commitment

def pyn_ae(ae_title):
    from pynetdicom import AE
    ae = AE(ae_title=ae_title)
    ae.acse_timeout = 10
    ae.dimse_timeout = 10
    ae.network_timeout = 10
    return ae


def commitment_request_ds(uids):
    from pydicom.dataset import Dataset
    ds = Dataset()
    ds.TransactionUID = "2.25.31415926"
    items = []
    for u in uids:
        i = Dataset()
        i.ReferencedSOPClassUID = "1.2.840.10008.5.1.4.1.1.7"
        i.ReferencedSOPInstanceUID = u
        items.append(i)
    ds.ReferencedSOPSequence = items
    return ds


def section_commitment():
    from pynetdicom import build_role, evt
    from pynetdicom.sop_class import StorageCommitmentPushModel as SC
    import pydicom
    tmp = tempfile.mkdtemp(prefix="siaara_xc_commit_")
    inst = "1.2.840.10008.1.20.1.1"

    # 1. pynetdicom SCU -> our SCP, report on the same association
    pf = os.path.join(tmp, "p1")
    srv = Server([TOOL, "commit-scp", pf, "same"], port_file=pf)
    got = []
    done = threading.Event()

    def on_report(event):
        got.append((event.event_type, event.event_information if hasattr(event, "event_information") else None,
                    event.request.EventTypeID))
        ds = event.event_information
        got.append(ds)
        done.set()
        return 0x0000, None

    ae = pyn_ae("PYN_SCU")
    ae.add_requested_context(SC)
    assoc = ae.associate("127.0.0.1", srv.port, ae_title="SIAARA_COMMIT", evt_handlers=[(evt.EVT_N_EVENT_REPORT, on_report)])
    ok = assoc.is_established
    detail = "pynetdicom send_n_action (StorageCommitmentPushModel) to services_tool commit-scp same"
    if ok:
        status, _ = assoc.send_n_action(commitment_request_ds(["2.25.1.1", "2.25.1.9"]), 1, SC, inst)
        ok = status is not None and status.Status == 0
        done.wait(10)
        assoc.release()
        if got:
            info = got[-1]
            ok = ok and got[0][2] == 2 and len(info.ReferencedSOPSequence) == 1 and len(info.FailedSOPSequence) == 1 \
                and info.TransactionUID == "2.25.31415926"
            detail += f" -> N-ACTION 0x{status.Status:04X}, N-EVENT-REPORT type {got[0][2]}, committed " \
                      f"{len(info.ReferencedSOPSequence)}, failed {len(info.FailedSOPSequence)}"
        else:
            ok = False
            detail += " -> no N-EVENT-REPORT received"
    srv.stop()
    record("pynetdicom Storage Commitment SCU -> SIAARA SCP (same association)", ok, detail)

    # 2. pynetdicom SCU -> our SCP, report on a new association (pynetdicom listens, SCP role)
    lport = free_port()
    got2 = []
    done2 = threading.Event()

    def on_report2(event):
        got2.append((event.request.EventTypeID, event.event_information, event.assoc.requestor.ae_title))
        done2.set()
        return 0x0000, None

    lae = pyn_ae("PYN_SCU")
    lae.add_supported_context(SC, scu_role=False, scp_role=True)
    server = lae.start_server(("127.0.0.1", lport), block=False, evt_handlers=[(evt.EVT_N_EVENT_REPORT, on_report2)])
    pf = os.path.join(tmp, "p2")
    srv = Server([TOOL, "commit-scp", pf, "new", f"PYN_SCU=127.0.0.1:{lport}"], port_file=pf)
    ae = pyn_ae("PYN_SCU")
    ae.add_requested_context(SC)
    assoc = ae.associate("127.0.0.1", srv.port, ae_title="SIAARA_COMMIT")
    ok = assoc.is_established
    detail = f"pynetdicom send_n_action, then services_tool commit-scp new PYN_SCU=127.0.0.1:{lport} opens a new association"
    if ok:
        status, _ = assoc.send_n_action(commitment_request_ds(["2.25.2.1"]), 1, SC, inst)
        assoc.release()
        done2.wait(15)
        ok = status is not None and status.Status == 0 and bool(got2) and got2[0][0] == 1 and \
            len(got2[0][1].ReferencedSOPSequence) == 1
        detail += f" -> report type {got2[0][0] if got2 else '-'} from {got2[0][2] if got2 else '-'}"
    out, err = srv.stop()
    server.shutdown()
    record("pynetdicom Storage Commitment SCU -> SIAARA SCP (new association, role reversal)", ok, detail)

    # 3. our SCU -> pynetdicom SCP sending the report on the same association
    def make_scp(mode):
        def on_action(event):
            req = event.action_information
            def later():
                time.sleep(0.3)
                from pydicom.dataset import Dataset
                ds = Dataset()
                ds.TransactionUID = req.TransactionUID
                ok_items, bad = [], []
                for it in req.ReferencedSOPSequence:
                    (bad if it.ReferencedSOPInstanceUID.endswith(".9") else ok_items).append(it)
                if ok_items:
                    ds.ReferencedSOPSequence = ok_items
                if bad:
                    for b in bad:
                        b.FailureReason = 0x0112
                    ds.FailedSOPSequence = bad
                etype = 2 if bad else 1
                if mode == "same":
                    event.assoc.send_n_event_report(ds, etype, SC, inst)
                else:
                    rae = pyn_ae("PYN_COMMIT")
                    rae.add_requested_context(SC)
                    a2 = rae.associate("127.0.0.1", mode, ae_title="SIAARA_SCU",
                                       ext_neg=[build_role(SC, scp_role=True, scu_role=False)])
                    if a2.is_established:
                        a2.send_n_event_report(ds, etype, SC, inst)
                        a2.release()
            threading.Thread(target=later, daemon=True).start()
            return 0x0000, None
        return on_action

    sport = free_port()
    sae = pyn_ae("PYN_COMMIT")
    sae.add_supported_context(SC)
    server = sae.start_server(("127.0.0.1", sport), block=False, evt_handlers=[(evt.EVT_N_ACTION, make_scp("same"))])
    cmd = [TOOL, "commit-scu", "127.0.0.1", str(sport), "PYN_COMMIT"]
    p = run(cmd)
    server.shutdown()
    record("SIAARA Storage Commitment SCU -> pynetdicom SCP (same association)",
           p.returncode == 0 and "txn_match=1 event=2 committed=1 failed=1" in p.stdout,
           " ".join(cmd) + " -> " + (p.stdout.strip() or p.stderr.strip()[-300:]))

    # 4. our SCU (with listener) -> pynetdicom SCP reporting on a new association with role selection
    lport = free_port()
    sport = free_port()
    server = sae.start_server(("127.0.0.1", sport), block=False, evt_handlers=[(evt.EVT_N_ACTION, make_scp(lport))])
    cmd = [TOOL, "commit-scu", "127.0.0.1", str(sport), "PYN_COMMIT", "--listen", str(lport)]
    p = run(cmd, timeout=60)
    server.shutdown()
    record("SIAARA Storage Commitment SCU -> pynetdicom SCP (new association, role reversal)",
           p.returncode == 0 and "txn_match=1 event=2 committed=1 failed=1" in p.stdout,
           " ".join(cmd) + " -> " + (p.stdout.strip().replace("\n", " | ") or p.stderr.strip()[-300:]))
    shutil.rmtree(tmp, ignore_errors=True)


# ===================================================================== pynetdicom: MPPS

def section_mpps():
    from pynetdicom import evt
    from pynetdicom.sop_class import ModalityPerformedProcedureStep as MPPS
    from pydicom.dataset import Dataset
    tmp = tempfile.mkdtemp(prefix="siaara_xc_mpps_")
    # our SCU -> pynetdicom SCP
    store = {}

    def on_create(event):
        req = event.request
        ds = event.attribute_list
        if ds.get("PerformedProcedureStepStatus", "") != "IN PROGRESS":
            return 0x0106, None
        uid = req.AffectedSOPInstanceUID
        if uid in store:
            return 0x0111, None
        store[uid] = ds
        return 0x0000, ds

    def on_set(event):
        req = event.request
        uid = req.RequestedSOPInstanceUID
        if uid not in store:
            return 0x0112, None
        if store[uid].PerformedProcedureStepStatus != "IN PROGRESS":
            return 0x0110, None
        store[uid].update(event.modification_list)
        return 0x0000, store[uid]

    port = free_port()
    ae = pyn_ae("PYN_MPPS")
    ae.add_supported_context(MPPS)
    server = ae.start_server(("127.0.0.1", port), block=False,
                             evt_handlers=[(evt.EVT_N_CREATE, on_create), (evt.EVT_N_SET, on_set)])
    cmd = [TOOL, "mpps-scu", "127.0.0.1", str(port), "PYN_MPPS"]
    p = run(cmd)
    server.shutdown()
    record("SIAARA MPPS SCU -> pynetdicom MPPS SCP", p.returncode == 0 and
           "create=0x0000 set=0x0000 set_again=0x0110" in p.stdout,
           " ".join(cmd) + " -> " + (p.stdout.strip() or p.stderr.strip()[-300:]))

    # pynetdicom SCU -> our SCP
    pf = os.path.join(tmp, "port")
    srv = Server([TOOL, "mpps-scp", pf], port_file=pf)
    ae = pyn_ae("PYN_SCU")
    ae.add_requested_context(MPPS)
    assoc = ae.associate("127.0.0.1", srv.port, ae_title="SIAARA_MPPS")
    statuses = []
    if assoc.is_established:
        a = Dataset()
        a.PerformedProcedureStepStatus = "IN PROGRESS"
        a.PerformedProcedureStepID = "PPS-1"
        a.Modality = "CT"
        uid = "2.25.27182818"
        st, _ = assoc.send_n_create(a, MPPS, uid)
        statuses.append(st.Status if st else None)
        st, _ = assoc.send_n_create(a, MPPS, uid)
        statuses.append(st.Status if st else None)
        m = Dataset()
        m.PerformedProcedureStepStatus = "COMPLETED"
        st, _ = assoc.send_n_set(m, MPPS, uid)
        statuses.append(st.Status if st else None)
        st, _ = assoc.send_n_set(m, MPPS, uid)
        statuses.append(st.Status if st else None)
        st, _ = assoc.send_n_set(m, MPPS, "2.25.1")
        statuses.append(st.Status if st else None)
        assoc.release()
    srv.stop()
    record("pynetdicom MPPS SCU -> SIAARA MPPS SCP", statuses == [0x0000, 0x0111, 0x0000, 0x0110, 0x0112],
           "pynetdicom send_n_create x2, send_n_set x3 -> " + ", ".join(f"0x{s:04X}" if s is not None else "None"
                                                                       for s in statuses))
    shutil.rmtree(tmp, ignore_errors=True)


# ===================================================================== pynetdicom: UPS

def section_ups():
    from pynetdicom import build_role, evt
    from pynetdicom.sop_class import (UnifiedProcedureStepEvent as UEV, UnifiedProcedureStepPull as UPULL,
                                      UnifiedProcedureStepPush as UPUSH, UnifiedProcedureStepWatch as UWATCH)
    from pydicom.dataset import Dataset
    tmp = tempfile.mkdtemp(prefix="siaara_xc_ups_")

    # our SCU -> pynetdicom UPS SCP
    items = {}

    def on_create(event):
        ds = event.attribute_list
        if ds.get("ProcedureStepState", "") != "SCHEDULED":
            return 0xC309, None
        items[event.request.AffectedSOPInstanceUID] = {"ds": ds, "txn": None}
        return 0x0000, None

    def on_find(event):
        for uid, it in items.items():
            if it["ds"].ProcedureStepState == event.identifier.get("ProcedureStepState", it["ds"].ProcedureStepState):
                r = Dataset()
                r.SOPInstanceUID = uid
                r.ProcedureStepState = it["ds"].ProcedureStepState
                yield 0xFF00, r
        yield 0x0000, None

    def on_action(event):
        uid = event.request.RequestedSOPInstanceUID
        info = event.action_information
        it = items.get(uid)
        if it is None:
            return 0xC307, None
        if event.action_type == 1:
            if info.ProcedureStepState == "IN PROGRESS":
                if it["ds"].ProcedureStepState != "SCHEDULED":
                    return 0xC302, None
                it["txn"] = info.TransactionUID
            elif info.TransactionUID != it["txn"]:
                return 0xC301, None
            it["ds"].ProcedureStepState = info.ProcedureStepState
            return 0x0000, None
        return 0x0123, None

    def on_set(event):
        it = items.get(event.request.RequestedSOPInstanceUID)
        if it is None:
            return 0xC307, None
        mods = event.modification_list
        if mods.get("TransactionUID") != it["txn"]:
            return 0xC301, None
        for elem in mods:
            if elem.keyword not in ("TransactionUID",):
                it["ds"].add(elem)
        return 0x0000, None

    def on_get(event):
        it = items.get(event.request.RequestedSOPInstanceUID)
        if it is None:
            return 0xC307, None
        r = Dataset()
        for tag in event.request.AttributeIdentifierList or []:
            if tag in it["ds"]:
                r.add(it["ds"][tag])
        return 0x0000, r

    port = free_port()
    ae = pyn_ae("PYN_UPS")
    for c in (UPUSH, UPULL, UWATCH):
        ae.add_supported_context(c)
    server = ae.start_server(("127.0.0.1", port), block=False,
                             evt_handlers=[(evt.EVT_N_CREATE, on_create), (evt.EVT_C_FIND, on_find),
                                           (evt.EVT_N_ACTION, on_action), (evt.EVT_N_SET, on_set),
                                           (evt.EVT_N_GET, on_get)])
    cmd = [TOOL, "ups-scu", "127.0.0.1", str(port), "PYN_UPS"]
    p = run(cmd)
    server.shutdown()
    want = "create=0x0000 find=ok/1 claim=0x0000 set=0x0000 get=0x0000 state=IN PROGRESS label=Crosscheck step 2 complete=0x0000"
    record("SIAARA UPS SCU (push/pull) -> pynetdicom UPS SCP", p.returncode == 0 and want in p.stdout,
           " ".join(cmd) + " -> " + (p.stdout.strip() or p.stderr.strip()[-300:]))

    # pynetdicom SCU + pynetdicom event receiver -> our UPS SCP
    lport = free_port()
    events = []
    got_events = threading.Event()

    def on_event(event):
        events.append((event.request.EventTypeID, event.request.AffectedSOPInstanceUID,
                       getattr(event.event_information, "ProcedureStepState", "-"),
                       event.request.AffectedSOPClassUID))
        if len(events) >= 3:
            got_events.set()
        return 0x0000, None

    wae = pyn_ae("PYN_WATCH")
    wae.add_supported_context(UEV, scu_role=False, scp_role=True)
    wae.add_supported_context(UEV)
    wserver = wae.start_server(("127.0.0.1", lport), block=False, evt_handlers=[(evt.EVT_N_EVENT_REPORT, on_event)])
    pf = os.path.join(tmp, "port")
    srv = Server([TOOL, "ups-scp", pf, f"PYN_WATCH=127.0.0.1:{lport}"], port_file=pf)
    ae = pyn_ae("PYN_SCU")
    for c in (UPUSH, UPULL, UWATCH):
        ae.add_requested_context(c)
    assoc = ae.associate("127.0.0.1", srv.port, ae_title="SIAARA_UPS")
    log = []
    ok = assoc.is_established
    if ok:
        sub = Dataset()
        sub.ReceivingAE = "PYN_WATCH"
        sub.DeletionLock = "FALSE"
        st, _ = assoc.send_n_action(sub, 3, UWATCH, "1.2.840.10008.5.1.4.34.5")
        log.append(("subscribe", st.Status if st else None))
        ups = Dataset()
        ups.ProcedureStepState = "SCHEDULED"
        ups.PatientName = "Cross^Check"
        ups.ProcedureStepLabel = "pyn step"
        ups.InputReadinessState = "READY"
        uid = "2.25.161803398"
        st, _ = assoc.send_n_create(ups, UPUSH, uid)
        log.append(("create", st.Status if st else None))
        q = Dataset()
        q.ProcedureStepState = "SCHEDULED"
        q.SOPInstanceUID = ""
        q.ProcedureStepLabel = ""
        found = [r for s, r in assoc.send_c_find(q, UPULL) if s and s.Status in (0xFF00, 0xFF01)]
        log.append(("find", len(found)))
        a = Dataset()
        a.ProcedureStepState = "IN PROGRESS"
        a.TransactionUID = "2.25.112358"
        st, _ = assoc.send_n_action(a, 1, UPULL, uid)
        log.append(("claim", st.Status if st else None))
        st, _ = assoc.send_n_action(a, 1, UPULL, uid)
        log.append(("claim again", st.Status if st else None))
        st, r = assoc.send_n_get([0x00741000, 0x00741204], UPULL, uid)
        log.append(("get", st.Status if st else None, getattr(r, "ProcedureStepState", None)))
        a.ProcedureStepState = "COMPLETED"
        st, _ = assoc.send_n_action(a, 1, UPULL, uid)
        log.append(("complete", st.Status if st else None))
        assoc.release()
    got_events.wait(15)
    out, err = srv.stop()
    wserver.shutdown()
    want_log = [("subscribe", 0), ("create", 0), ("find", 1), ("claim", 0), ("claim again", 0xC302),
                ("get", 0, "IN PROGRESS"), ("complete", 0)]
    states = [e[2] for e in events]
    ok = ok and log == want_log and states[:3] == ["SCHEDULED", "IN PROGRESS", "COMPLETED"]
    record("pynetdicom UPS SCU + UPS Event receiver -> SIAARA UPS SCP", ok,
           f"log={log}; events={[(e[0], e[2]) for e in events]}; event SOP class={events[0][3] if events else '-'}"
           + ("" if ok else f"; scp stderr: {err.strip()[-300:]}"))
    shutil.rmtree(tmp, ignore_errors=True)


def main():
    if not os.path.exists(TOOL):
        print(f"services_tool not found in {BUILD}")
        return 2
    table = {"transcode": section_transcode, "dcmtk-store": section_dcmtk_store, "dcmtk-mwl": section_dcmtk_mwl,
             "dcmtk-print": section_dcmtk_print, "commitment": section_commitment, "mpps": section_mpps,
             "ups": section_ups}
    for s in SECTIONS:
        try:
            table[s]()
        except Exception as e:  # noqa: BLE001
            record(f"section {s}", False, f"exception: {type(e).__name__}: {e}")
    fails = sum(1 for r in RESULTS if r[0] == "FAIL")
    print(f"\n{len(RESULTS)} checks: {sum(1 for r in RESULTS if r[0] == 'PASS')} passed, {fails} failed, "
          f"{sum(1 for r in RESULTS if r[0] == 'SKIP')} skipped")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
