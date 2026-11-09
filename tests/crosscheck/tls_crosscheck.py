#!/usr/bin/env python3
"""TLS interoperability cross-check (PS3.15 B.12/B.13) against independent implementations used
only as test oracles: DCMTK built with OpenSSL (echoscu/storescu/storescp +tls), the OpenSSL
command line (s_client/s_server) and curl.

Throwaway certificates are made at run time with `openssl req`/`openssl x509` in a temporary
folder that is deleted afterwards; no key is ever written to the source tree.

Checks (127.0.0.1, free ports):
  1. DCMTK echoscu/storescu +tls (client certificate)  -> siaara-storescp --tls --tls-require-client-cert
  2. siaara-echoscu --tls (client certificate)         -> DCMTK storescp +tls
  3. siaara-echoscu --tls without a client certificate -> DCMTK storescp +tls -ic? (refused when
                                                           DCMTK requires one)
  4. negatives: untrusted CA, wrong host name, plain client to TLS port, TLS client to plain port
  5. openssl s_server -tls1_2 / -tls1_3 as server, TLS 1.1 and CBC-only servers refused, and the
     clean close_notify seen by s_server
  6. openssl s_client -tls1_1 / CBC-only against siaara-storescp refused, -tls1_2/-tls1_3 accepted
  7. DICOMweb over https: siaara-webcli -> tls_tool https; curl and siaara-webcli STOW/QIDO/WADO

Usage: tls_crosscheck.py [--build BUILD_DIR]     exit status 0 when every check passes.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

RESULTS = []

DUMP = """(0008,0016) UI =CTImageStorage
(0008,0018) UI [{uid}]
(0008,0060) CS [CT]
(0010,0010) PN [Tls^Crosscheck]
(0010,0020) LO [TLS1]
(0020,000d) UI [2.25.61000]
(0020,000e) UI [2.25.61001]
(0028,0002) US 1
(0028,0004) CS [MONOCHROME2]
(0028,0010) US 16
(0028,0011) US 16
(0028,0100) US 16
(0028,0101) US 12
(0028,0102) US 11
(0028,0103) US 0
(7fe0,0010) OW {pixels}
"""


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(cmd, timeout=60, env=None, stdin_data=None):
    p = subprocess.run(cmd, input=stdin_data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout, env=env)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (("  [" + detail.strip()[-300:] + "]") if detail and not ok else ""),
          flush=True)


def wait_port(port, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def start(cmd, port, stdin=None):
    p = subprocess.Popen(cmd, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if not wait_port(port):
        p.kill()
        raise RuntimeError("server did not start: " + " ".join(cmd) + "\n" + p.communicate()[0].decode())
    return p


def stop(p):
    if p.poll() is None:
        p.terminate()
    try:
        out, _ = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
    return out.decode("utf-8", "replace")


def make_pki(d):
    """CA, server (EC P-256, SAN localhost/127.0.0.1), client (RSA 2048), another CA."""
    def sh(cmd):
        rc, o = run(cmd)
        if rc != 0:
            raise RuntimeError(" ".join(cmd) + "\n" + o)
    p = lambda n: os.path.join(d, n)
    ec = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes"]
    ca_ext = ["-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign"]
    sh(["openssl", "req", "-x509"] + ec + ["-keyout", p("ca.key"), "-out", p("ca.pem"), "-days", "2",
                                          "-subj", "/CN=SIAARA Crosscheck CA"] + ca_ext)
    sh(["openssl", "req", "-x509"] + ec + ["-keyout", p("other.key"), "-out", p("other.pem"), "-days", "2",
                                          "-subj", "/CN=Other CA"] + ca_ext)
    with open(p("server.ext"), "w") as f:
        f.write("basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n"
                "subjectAltName=DNS:localhost,IP:127.0.0.1\n")
    with open(p("client.ext"), "w") as f:
        f.write("basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=clientAuth\n")
    sh(["openssl", "req"] + ec + ["-keyout", p("server.key"), "-out", p("server.csr"), "-subj", "/CN=localhost"])
    sh(["openssl", "x509", "-req", "-in", p("server.csr"), "-CA", p("ca.pem"), "-CAkey", p("ca.key"),
        "-set_serial", "2", "-days", "2", "-out", p("server.pem"), "-extfile", p("server.ext")])
    sh(["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", p("client.key"), "-out", p("client.csr"),
        "-subj", "/CN=SIAARA_SCU"])
    sh(["openssl", "x509", "-req", "-in", p("client.csr"), "-CA", p("ca.pem"), "-CAkey", p("ca.key"),
        "-set_serial", "3", "-days", "2", "-out", p("client.pem"), "-extfile", p("client.ext")])
    return {k: p(k + ".pem") for k in ("ca", "other", "server", "client")} | \
           {k + "_key": p(k + ".key") for k in ("server", "client")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    args = ap.parse_args()
    exe = ".exe" if os.name == "nt" else ""
    tools = os.path.join(args.build, "tools")
    echoscu = os.path.join(tools, "siaara-echoscu" + exe)
    storescp = os.path.join(tools, "siaara-storescp" + exe)
    webcli = os.path.join(tools, "siaara-webcli" + exe)
    tls_tool = os.path.join(args.build, "tests", "tls_tool" + exe)
    for t in (echoscu, storescp, webcli, tls_tool):
        if not os.path.exists(t):
            print("missing " + t + "; build first")
            return 2
    if not shutil.which("openssl"):
        print("SKIP: the openssl command line tool is not installed")
        return 0
    rc, info = run([tls_tool, "info"])
    if rc != 0:
        print("SKIP: TLS is not available in this build: " + info.strip())
        return 0
    print("TLS backend: " + info.strip())
    dcmtk = all(shutil.which(t) for t in ("echoscu", "storescp", "storescu", "dump2dcm"))
    if dcmtk:
        rc, o = run(["echoscu", "--help"])
        dcmtk = "+tls" in o
    if not dcmtk:
        print("note: DCMTK with TLS not found; DCMTK checks skipped (apt-get install dcmtk)")

    work = tempfile.mkdtemp(prefix="siaara_tls_xc_")
    try:
        pki = make_pki(work)
        ca, other = pki["ca"], pki["other"]
        srv, srv_key, cli, cli_key = pki["server"], pki["server_key"], pki["client"], pki["client_key"]
        our_client = ["--tls", "--tls-ca", ca, "--tls-cert", cli, "--tls-key", cli_key]
        dicom = None
        if dcmtk:
            txt = os.path.join(work, "ct.txt")
            pixels = "\\".join("%04x" % ((k * 37) % 4096) for k in range(16 * 16))
            with open(txt, "w") as f:
                f.write(DUMP.format(uid="2.25.61002", pixels=pixels))
            dicom = os.path.join(work, "ct.dcm")
            rc, o = run(["dump2dcm", "+te", txt, dicom])
            if rc != 0 or not os.path.exists(dicom):
                raise RuntimeError("dump2dcm failed: " + o)

        # ---- 1. DCMTK clients -> siaara-storescp (mutual TLS)
        port = free_port()
        rx = os.path.join(work, "rx")
        os.makedirs(rx)
        scp = start([storescp, "-v", "-aet", "SIAARA_TLS", "-od", rx, "--tls", "--tls-cert", srv, "--tls-key", srv_key,
                     "--tls-ca", ca, "--tls-require-client-cert", str(port)], port)
        try:
            if dcmtk:
                rc, o = run(["echoscu", "-aec", "SIAARA_TLS", "+tls", cli_key, cli, "+cf", ca, "localhost", str(port)])
                check("DCMTK echoscu +tls (client cert) -> siaara-storescp --tls", rc == 0, o)
                rc, o = run(["storescu", "-aec", "SIAARA_TLS", "+tls", cli_key, cli, "+cf", ca, "127.0.0.1", str(port),
                             dicom])
                got = os.path.join(rx, "2.25.61002.dcm")
                check("DCMTK storescu +tls -> siaara-storescp --tls, file received",
                      rc == 0 and os.path.exists(got) and os.path.getsize(got) > 500, o)
                rc, o = run(["echoscu", "-aec", "SIAARA_TLS", "+tla", "+cf", ca, "localhost", str(port)])
                check("DCMTK echoscu +tla (no client cert) refused by siaara-storescp", rc != 0, o)
                rc, o = run(["echoscu", "-aec", "SIAARA_TLS", "+tls", cli_key, cli, "+cf", other, "localhost", str(port)])
                check("DCMTK echoscu with another CA refuses siaara-storescp", rc != 0, o)
                rc, o = run(["echoscu", "-aec", "SIAARA_TLS", "-to", "5", "localhost", str(port)])
                check("DCMTK echoscu without TLS fails on the TLS port", rc != 0, o)
            rc, o = run([echoscu, "-aec", "SIAARA_TLS", "-v"] + our_client + ["127.0.0.1", str(port)])
            check("siaara-echoscu --tls -> siaara-storescp --tls (mutual)", rc == 0 and "TLSv1.3" in o, o)
            rc, o = run([echoscu, "-aec", "SIAARA_TLS", "--tls", "--tls-ca", ca, "127.0.0.1", str(port)])
            check("siaara-echoscu --tls without client cert refused (mutual TLS required)", rc != 0, o)
            rc, o = run([echoscu, "-aec", "SIAARA_TLS", "--tls-name", "pacs.other.example"] + our_client +
                        ["127.0.0.1", str(port)])
            check("siaara-echoscu refuses a wrong host name", rc != 0 and "hostname mismatch" in o, o)
            rc, o = run([echoscu, "-aec", "SIAARA_TLS", "--tls", "--tls-ca", other, "--tls-cert", cli, "--tls-key",
                         cli_key, "127.0.0.1", str(port)])
            check("siaara-echoscu refuses an untrusted server", rc != 0, o)
            rc, o = run([echoscu, "-aec", "SIAARA_TLS", "--tls", "--tls-insecure", "--tls-ca", other, "--tls-cert", cli,
                         "--tls-key", cli_key, "127.0.0.1", str(port)])
            check("siaara-echoscu --tls-insecure connects and warns", rc == 0 and "WARNING" in o, o)
            # 6. openssl s_client as negative/positive oracle against our server
            base = ["openssl", "s_client", "-connect", "127.0.0.1:%d" % port, "-CAfile", ca, "-cert", cli, "-key",
                    cli_key, "-verify_return_error"]
            rc, o = run(base + ["-tls1_3"], stdin_data=b"")
            check("openssl s_client -tls1_3 -> siaara-storescp accepted", rc == 0 and "TLSv1.3" in o, o)
            rc, o = run(base + ["-tls1_2"], stdin_data=b"")
            check("openssl s_client -tls1_2 -> siaara-storescp accepted (ECDHE+AEAD)",
                  rc == 0 and "ECDHE-ECDSA-AES128-GCM-SHA256" in o, o)
            rc, o = run(base + ["-tls1_1", "-cipher", "DEFAULT@SECLEVEL=0"], stdin_data=b"")
            check("openssl s_client -tls1_1 -> siaara-storescp refused", rc != 0, o)
            rc, o = run(base + ["-tls1_2", "-cipher", "ECDHE-ECDSA-AES128-SHA256:ECDHE-ECDSA-AES256-SHA"],
                        stdin_data=b"")
            check("openssl s_client -tls1_2 CBC-only -> siaara-storescp refused", rc != 0, o)
        finally:
            out = stop(scp)
        check("siaara-storescp logged TLS sessions", "TLS: accepted" in out, out)

        # ---- 2. siaara-echoscu -> DCMTK storescp +tls
        if dcmtk:
            port = free_port()
            dc = start(["storescp", "-aet", "DCMTK_TLS", "+tls", srv_key, srv, "+cf", ca, str(port)], port)
            try:
                rc, o = run([echoscu, "-aec", "DCMTK_TLS", "-v"] + our_client + ["127.0.0.1", str(port)])
                check("siaara-echoscu --tls -> DCMTK storescp +tls", rc == 0 and "C-ECHO-RSP status 0x0000" in o, o)
                rc, o = run([echoscu, "-aec", "DCMTK_TLS", "--tls", "--tls-ca", ca, "127.0.0.1", str(port)])
                check("siaara-echoscu without client cert refused by DCMTK (require-peer-cert)", rc != 0, o)
                rc, o = run([echoscu, "-aec", "DCMTK_TLS", "--tls-name", "localhost", "-r", "3"] + our_client +
                            ["127.0.0.1", str(port)])
                check("siaara-echoscu --tls-name localhost x3 -> DCMTK storescp", rc == 0, o)
            finally:
                stop(dc)
            port = free_port()
            dc = start(["storescp", "-aet", "DCMTK_PLAIN", str(port)], port)
            try:
                rc, o = run([echoscu, "-aec", "DCMTK_PLAIN", "-t", "5000"] + our_client + ["127.0.0.1", str(port)])
                check("siaara-echoscu --tls to a plain DCMTK port fails (no fallback)", rc != 0, o)
            finally:
                stop(dc)

        # ---- 5. openssl s_server as the server
        def s_server(extra, client_args, name, expect_ok, needle=None):
            port = free_port()
            srv_p = subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-cert", srv, "-key", srv_key,
                                      "-naccept", "1"] + extra, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
            head = b""
            end = time.time() + 10
            while b"ACCEPT" not in head and time.time() < end and srv_p.poll() is None:
                head += srv_p.stdout.readline()
            rc, o = run([echoscu, "-v", "-t", "1500"] + client_args + ["127.0.0.1", str(port)])
            out = head.decode("utf-8", "replace") + stop(srv_p)
            connected = "TLS: connected" in o
            check(name, connected == expect_ok and (needle is None or needle in o + out), o + out)
            return o, out

        _, out = s_server(["-tls1_2"], our_client, "siaara-echoscu -> openssl s_server -tls1_2 (handshake)", True,
                          "TLSv1.2")
        s_server(["-tls1_3"], our_client, "siaara-echoscu -> openssl s_server -tls1_3 (handshake)", True, "TLSv1.3")
        # s_server prints DONE when SSL_read reports the peer's close_notify, ERROR / "unexpected eof"
        # when the TCP connection ends without one.
        _, out = s_server([], our_client, "siaara-echoscu sends close_notify (s_server prints DONE)", True, "DONE\nshut")
        check("no unexpected EOF at s_server", "unexpected eof" not in out and "ERROR" not in out, out)
        s_server(["-tls1_1", "-cipher", "DEFAULT@SECLEVEL=0"], our_client, "siaara-echoscu refuses a TLS 1.1 server",
                 False)
        s_server(["-tls1_2", "-cipher", "ECDHE-ECDSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA"], our_client,
                 "siaara-echoscu refuses a CBC-only TLS 1.2 server", False)

        # ---- 7. DICOMweb over https
        port = free_port()
        web = start([tls_tool, "https", str(port), srv, srv_key, "-"], port, stdin=subprocess.PIPE)
        try:
            url = "https://127.0.0.1:%d/dicomweb" % port
            if dicom:
                rc, o = run([webcli, "stow", url, "--tls-ca", ca, dicom])
                check("siaara-webcli stow over https", rc == 0 and "1 stored" in o, o)
            rc, o = run([webcli, "qido", url, "studies", "--tls-ca", ca, "--tls-verbose"])
            check("siaara-webcli qido over https", rc == 0 and o.strip().endswith("]"), o)
            rc, o = run([webcli, "qido", "https://localhost:%d/dicomweb" % port, "studies", "--tls-ca", other])
            check("siaara-webcli refuses an untrusted https server", rc != 0, o)
            rc, o = run([webcli, "qido", url.replace("https", "http"), "studies", "--timeout", "3000"])
            check("plain http to the https port fails", rc != 0, o)
            if shutil.which("curl"):
                rc, o = run(["curl", "-sS", "--fail", "--cacert", ca, "--tlsv1.2", url + "/studies"])
                check("curl --cacert -> tls_tool https (QIDO)", rc == 0 and (o.startswith("[") or o == ""), o)
                rc, o = run(["curl", "-sS", "--fail", "--cacert", ca, "--tlsv1.2", "--tls-max", "1.2",
                             "--ciphers", "ECDHE-ECDSA-AES256-GCM-SHA384", url + "/studies"])
                check("curl TLS 1.2 ECDHE-ECDSA-AES256-GCM-SHA384 -> https server", rc == 0, o)
                rc, o = run(["curl", "-sS", "--fail", "--cacert", ca, "--tlsv1.2", "--tls-max", "1.2",
                             "--ciphers", "ECDHE-ECDSA-AES128-SHA256", url + "/studies"])
                check("curl TLS 1.2 CBC suite refused by the https server", rc != 0, o)
                if dicom:
                    rc, o = run(["curl", "-sS", "--fail", "--cacert", ca, "-H", "Accept: application/dicom+json",
                                 url + "/studies/2.25.61000/metadata"])
                    check("curl WADO-RS metadata over https", rc == 0 and "2.25.61002" in o, o)
        finally:
            web.stdin.close()
            out = stop(web)
        # mutual TLS on the web server
        port = free_port()
        web = start([tls_tool, "https", str(port), srv, srv_key, ca], port, stdin=subprocess.PIPE)
        try:
            url = "https://127.0.0.1:%d/dicomweb" % port
            rc, o = run([webcli, "qido", url, "studies", "--tls-ca", ca])
            check("https with mutual TLS: siaara-webcli without a certificate refused", rc != 0, o)
            rc, o = run([webcli, "qido", url, "studies", "--tls-ca", ca, "--tls-cert", cli, "--tls-key", cli_key])
            check("https with mutual TLS: siaara-webcli with a certificate accepted", rc == 0, o)
            if shutil.which("curl"):
                rc, o = run(["curl", "-sS", "--fail", "--cacert", ca, "--cert", cli, "--key", cli_key, url + "/studies"])
                check("https with mutual TLS: curl with a certificate accepted", rc == 0, o)
        finally:
            web.stdin.close()
            stop(web)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    failed = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed" % (len(RESULTS), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
