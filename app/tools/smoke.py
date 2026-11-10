#!/usr/bin/env python3
# SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
#
# Scripted smoke test against a running host (no browser needed). Usage:
#   smoke.py LAUNCH_URL STUDY_UID
# LAUNCH_URL is what --print-url printed (single use), or "@DATA_DIR/instance.url" to read the running
# instance's current (fresh) secret from its owner-only instance file.
# The app is served under a random <hex>.localhost name bound to 127.0.0.1. Browsers resolve such
# names themselves; this script connects to 127.0.0.1 and sends the name in the Host header, so it
# does not depend on the system resolver.
# Posts the launch secret for the cookie (as the launch page does), fetches the browser page and the
# scripts, checks the refusals (no cookie, plain 127.0.0.1 Host, foreign Origin, repeated secret) and the
# Content-Security-Policy, opens the study through POST /api/sessions (UID in the body, never in an
# address), connects to /ws/view with a hand-written WebSocket client (standard library only), sends
# a resize and reads the first frame and state message, then signs out and checks the cookie is dead.
import base64, http.client, json, os, socket, struct, sys, urllib.parse

def request(authority, method, path, headers=None, body=None):
    """One request to 127.0.0.1:<port> with Host: <authority> (authority = host:port of the app)."""
    port = int(authority.rsplit(":", 1)[1])
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    h = {"Host": authority}
    h.update(headers or {})
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    # Repeated headers are joined, so a second Content-Security-Policy would show.
    hs = {}
    for k, v in r.getheaders():
        hs[k] = hs[k] + " || " + v if k in hs else v
    return r.status, hs, data

def resolve_launch(arg):
    """A launch URL, or "@file" to read the instance file's current single-use URL."""
    if arg.startswith("@"):
        with open(arg[1:], encoding="utf-8") as f:
            return f.read().strip()
    return arg

def ws_connect(host, port, path, headers, authority=None):
    """WebSocket handshake. With authority, connects to 127.0.0.1:<port> and names the app in Host."""
    s = socket.create_connection(("127.0.0.1" if authority else host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    req = [f"GET {path} HTTP/1.1", f"Host: {authority or f'{host}:{port}'}", "Upgrade: websocket", "Connection: Upgrade",
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
    launch, study = resolve_launch(sys.argv[1]), sys.argv[2]
    u = urllib.parse.urlsplit(launch)
    authority, port, origin = u.netloc, u.port, f"http://{u.netloc}"
    secret = urllib.parse.parse_qs(u.query)["k"][0]
    form = {"Content-Type": "application/x-www-form-urlencoded"}
    print(f"host name is a random *.localhost label: {u.hostname.endswith('.localhost') and len(u.hostname) == 42}")
    st, _, _ = request(authority, "GET", "/")
    print(f"GET / without cookie -> {st}")
    st, h, body = request(authority, "POST", "/launch", form, "k=" + secret)
    cookie = h["Set-Cookie"].split(";")[0]
    print(f"POST /launch -> {st}, cookie name {cookie.split('=')[0]}, attributes: {'; '.join(h['Set-Cookie'].split('; ')[1:])}, "
          f"moves on by meta refresh: {b'http-equiv=\"refresh\"' in body}")
    st2, _, body2 = request(authority, "POST", "/launch", form, "k=" + secret)
    print(f"POST /launch again with the same secret -> {st2} (single use), says so: {b'already used' in body2}")
    st, h, body = request(authority, "GET", "/", {"Cookie": cookie})
    page = body.decode()
    print(f"GET / with cookie -> {st}, {len(body)} bytes, grid header present: {'Patient name' in page}, study UID in the page: {study in page}")
    print(f"Content-Security-Policy: {h.get('Content-Security-Policy')}")
    st, _, _ = request(f"127.0.0.1:{port}", "GET", "/", {"Cookie": cookie})
    print(f"GET / with cookie but Host 127.0.0.1:<port> -> {st}")
    for p in ["/_framework/blazor.web.js", "/_content/Siaara.Viewer.Web/js/siaara-view.js", "/_content/Siaara.Viewer.Web/css/siaara.css"]:
        st, _, body = request(authority, "GET", p, {"Cookie": cookie})
        print(f"GET {p} -> {st}, {len(body)} bytes")
    st, _, body = request(authority, "POST", "/api/sessions?study=" + urllib.parse.quote(study), {"Cookie": cookie, "Origin": origin})
    print(f"POST /api/sessions with the UID in the address -> {st}")
    st, _, body = request(authority, "POST", "/api/sessions", {"Cookie": cookie, "Origin": origin, "Content-Type": "text/plain"}, study)
    opened = json.loads(body)
    sid = opened["id"]
    print(f"POST /api/sessions (UID in the body) -> {st}")
    st, _, body = request(authority, "GET", "/viewer/" + opened["ref"], {"Cookie": cookie})
    print(f"GET /viewer/<per-run id> -> {st}, {len(body)} bytes")
    _, st = ws_connect(None, port, "/ws/view?s=" + sid, {"Origin": origin}, authority)
    print(f"/ws/view without cookie -> {st}")
    _, st = ws_connect(None, port, "/ws/view?s=" + sid, {"Cookie": cookie, "Origin": "http://evil.example"}, authority)
    print(f"/ws/view with a foreign Origin -> {st}")
    s, st = ws_connect(None, port, "/ws/view?s=" + sid, {"Cookie": cookie, "Origin": origin}, authority)
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
    st, _, body = request(authority, "POST", "/signout", {"Cookie": cookie, "Origin": origin})
    st2, _, _ = request(authority, "GET", "/", {"Cookie": cookie})
    print(f"POST /signout -> {st} (to the signed-out page); GET / with the old cookie -> {st2}")

if __name__ == "__main__":
    main()
