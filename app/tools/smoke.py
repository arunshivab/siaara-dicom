#!/usr/bin/env python3
# SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
#
# Scripted smoke test against a running host (no browser needed). Usage:
#   smoke.py LAUNCH_URL STUDY_UID
# Exchanges the launch secret for the cookie, fetches the browser page and the scripts, checks the
# refusals (no cookie, foreign Origin), opens the study through POST /api/sessions, connects to
# /ws/view with a hand-written WebSocket client (standard library only), sends a resize and reads
# the first frame and state message.
import base64, http.client, json, os, socket, struct, sys, urllib.parse

def request(host, port, method, path, headers=None):
    c = http.client.HTTPConnection(host, port, timeout=10)
    c.request(method, path, headers=headers or {})
    r = c.getresponse()
    body = r.read()
    return r.status, dict(r.getheaders()), body

def ws_connect(host, port, path, headers):
    s = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    req = [f"GET {path} HTTP/1.1", f"Host: {host}:{port}", "Upgrade: websocket", "Connection: Upgrade",
           f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"] + [f"{k}: {v}" for k, v in headers.items()]
    s.sendall(("\r\n".join(req) + "\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(4096)
        if not chunk:
            break
        resp += chunk
    status = int(resp.split(b" ", 2)[1])
    return s, status

def ws_send(s, payload, text=False):
    mask = os.urandom(4)
    n = len(payload)
    head = bytes([0x81 if text else 0x82])
    head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n)
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf

def ws_recv(s):
    b0, b1 = recv_exact(s, 2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack(">H", recv_exact(s, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", recv_exact(s, 8))[0]
    return b0 & 0x0F, recv_exact(s, n)

def main():
    launch, study = sys.argv[1], sys.argv[2]
    u = urllib.parse.urlsplit(launch)
    host, port, origin = u.hostname, u.port, f"http://{u.hostname}:{u.port}"
    st, _, _ = request(host, port, "GET", "/")
    print(f"GET / without cookie -> {st}")
    st, h, _ = request(host, port, "GET", u.path + "?" + u.query)
    cookie = h["Set-Cookie"].split(";")[0]
    print(f"GET /launch -> {st}, cookie attributes: {'; '.join(h['Set-Cookie'].split('; ')[1:])}")
    st, _, body = request(host, port, "GET", "/", {"Cookie": cookie})
    page = body.decode()
    print(f"GET / with cookie -> {st}, {len(body)} bytes, grid header present: {'Patient name' in page}")
    for p in ["/_framework/blazor.web.js", "/_content/Siaara.Viewer.Web/js/siaara-view.js", "/_content/Siaara.Viewer.Web/css/siaara.css"]:
        st, _, body = request(host, port, "GET", p, {"Cookie": cookie})
        print(f"GET {p} -> {st}, {len(body)} bytes")
    st, _, body = request(host, port, "GET", "/viewer/" + urllib.parse.quote(study), {"Cookie": cookie})
    print(f"GET /viewer/<study> -> {st}, {len(body)} bytes")
    st, _, body = request(host, port, "POST", "/api/sessions?study=" + urllib.parse.quote(study), {"Cookie": cookie, "Origin": origin})
    sid = json.loads(body)["id"]
    print(f"POST /api/sessions -> {st}")
    _, st = ws_connect(host, port, "/ws/view?s=" + sid, {"Origin": origin})
    print(f"/ws/view without cookie -> {st}")
    _, st = ws_connect(host, port, "/ws/view?s=" + sid, {"Cookie": cookie, "Origin": "http://evil.example"})
    print(f"/ws/view with a foreign Origin -> {st}")
    s, st = ws_connect(host, port, "/ws/view?s=" + sid, {"Cookie": cookie, "Origin": origin})
    print(f"/ws/view with cookie and Origin -> {st}")
    resize = struct.pack("<BHIBhhBhHBHH", 4, 0, 1, 5, 0, 0, 0, 0, 0, 0, 400, 300)
    ws_send(s, resize)
    got_frame = False
    while True:
        op, msg = ws_recv(s)
        if msg[0] == 1:
            t, fmt, vp, seq, w, h = struct.unpack("<BBHIHH", msg[:12])
            print(f"frame: format {fmt} ({'gray8' if fmt == 2 else 'rgba'}), viewport {vp}, seq {seq}, {w}x{h}, {len(msg) - 12} pixel bytes (expected {w * h * (1 if fmt == 2 else 4)})")
            ws_send(s, struct.pack("<BHI", 3, vp, seq))
            got_frame = True
        elif msg[0] == 2:
            vp, seq = struct.unpack("<HI", msg[1:7])
            state = json.loads(msg[7:])
            if state.get("message"):
                print(f"state: viewport {vp}, seq {seq}, message: {state['message']}")
            else:
                print(f"state: viewport {vp}, seq {seq}, image {state.get('imageIndex', 0) + 1}/{state.get('imageCount')}, corner lines: {sum(len(v) for v in (state.get('overlayText') or {}).values())}, warnings: {state.get('warnings')}")
            if got_frame and seq > 0:
                break
    s.close()

if __name__ == "__main__":
    main()
