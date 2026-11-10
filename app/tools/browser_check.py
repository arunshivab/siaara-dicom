#!/usr/bin/env python3
# SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
#
# Drives a headless Chrome / Chromium through the DevTools protocol (standard library only) against
# a running host: opens the launch URL, screenshots the study browser, opens the viewer, does a
# right-drag (W/L), a wheel scroll, a length measurement and the 2x2 layout, and screenshots again.
# Then the M1.1 pieces: the intended-use label, the right-click menu and a colour map, the magnifier,
# reference lines and cross-reference on the CT study's axial / sagittal / coronal series, arrow and
# text annotations, the About box, and an exported PNG (with the label band).
# Then the round-4 security pieces: the page was reached through the one-shot launch page FILE (as the
# host opens it), under the random *.localhost name; the viewer address holds no Study Instance UID; the
# console shows no Content-Security-Policy violation; Sign out locks the window.
# Then the M2 networking UI, with the app talking to itself over the loopback interface: the Network sheet (the
# receiver switched on for one sender, a DICOM node pointing at that receiver, its Test button), the source dropdown,
# a remote search with the Local column, series on expand, Retrieve and Send with their activity rows, and a
# validation message.
# Usage: browser_check.py CHROME_BINARY LAUNCH STUDY_UID OUT_DIR
# LAUNCH is the path the host printed with --print-launch-file (preferred: the real launch page), a
# launch URL, or @INSTANCE_FILE.
import base64, json, os, socket, struct, subprocess, sys, time, urllib.parse, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke import ws_connect, ws_send, recv_exact, resolve_launch  # noqa: E402

def ws_recv_full(s):
    data = b""
    while True:
        b0, b1 = recv_exact(s, 2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", recv_exact(s, 2))[0]
        elif n == 127:
            n = struct.unpack(">Q", recv_exact(s, 8))[0]
        data += recv_exact(s, n)
        if b0 & 0x80:
            return data

class Cdp:
    def __init__(self, port):
        for _ in range(50):
            try:
                pages = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
                break
            except OSError:
                time.sleep(0.2)
        page = next(p for p in pages if p["type"] == "page")
        u = urllib.parse.urlsplit(page["webSocketDebuggerUrl"])
        self.s, st = ws_connect(u.hostname, u.port, u.path, {})
        self.s.settimeout(30)
        self.n = 0
        self.events = []

    def call(self, method, **params):
        self.n += 1
        ws_send(self.s, json.dumps({"id": self.n, "method": method, "params": params}).encode(), text=True)
        while True:
            m = json.loads(ws_recv_full(self.s))
            if m.get("id") == self.n:
                return m.get("result", m.get("error"))
            self.events.append(m)

    def eval(self, expr):
        r = self.call("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
        return r.get("result", {}).get("value")

    def shot(self, path):
        r = self.call("Page.captureScreenshot", format="png")
        with open(path, "wb") as f:
            f.write(base64.b64decode(r["data"]))

    def mouse(self, kind, x, y, button="none", buttons=0):
        self.call("Input.dispatchMouseEvent", type=kind, x=x, y=y, button=button, buttons=buttons, clickCount=1 if kind in ("mousePressed", "mouseReleased") else 0)

def key(c, code, ch):
    c.call("Input.dispatchKeyEvent", type="rawKeyDown", windowsVirtualKeyCode=code, key=ch, code="Key" + ch.upper())
    c.call("Input.dispatchKeyEvent", type="keyUp", windowsVirtualKeyCode=code, key=ch, code="Key" + ch.upper())
    time.sleep(0.2)

def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24]) if head[:8] == b"\x89PNG\r\n\x1a\n" else None

def m11(c, out):
    print("patient bar label:", c.eval("(document.querySelector('.patientbar .disclaimer') || {}).textContent"))
    rects = c.eval("[...document.querySelectorAll('.vp')].map(v => { const b = v.getBoundingClientRect(); return [b.x, b.y, b.width, b.height]; })")
    print("viewport rects:", rects)
    v0 = rects[0]
    cx, cy = v0[0] + v0[2] / 2, v0[1] + v0[3] / 2
    # Make viewport 0 (axial) active so the others show its reference line.
    c.mouse("mousePressed", cx, cy, "left", 1)
    c.mouse("mouseReleased", cx, cy, "left", 0)
    time.sleep(0.6)
    c.shot(f"{out}/5-reference-lines.png")
    # Right click without a drag: the viewport menu.
    c.mouse("mouseMoved", cx, cy)
    c.mouse("mousePressed", cx, cy, "right", 2)
    c.mouse("mouseReleased", cx, cy, "right", 0)
    time.sleep(0.4)
    print("menu items:", c.eval("[...document.querySelectorAll('.vpmenu > .item')].map(i => i.textContent)"))
    # Open the colour map submenu.
    pos = c.eval("(() => { const i = [...document.querySelectorAll('.vpmenu > .item')].find(x => x.textContent.startsWith('Colour map')); if (!i) return null; const b = i.getBoundingClientRect(); return [b.x + 30, b.y + b.height / 2]; })()")
    if pos:
        c.mouse("mouseMoved", pos[0], pos[1])
        time.sleep(0.3)
    c.shot(f"{out}/6-context-menu.png")
    hot = c.eval("(() => { const i = [...document.querySelectorAll('.vpmenu .sub .item')].find(x => x.textContent.includes('Hot Iron')); if (!i) return null; const b = i.getBoundingClientRect(); return [b.x + 20, b.y + b.height / 2]; })()")
    print("Hot Iron item at:", hot)
    if hot:
        c.mouse("mouseMoved", hot[0], hot[1])
        c.mouse("mousePressed", hot[0], hot[1], "left", 1)
        c.mouse("mouseReleased", hot[0], hot[1], "left", 0)
        time.sleep(0.8)
    colour = "(() => { const c = document.querySelector('.vp canvas.img'); const g = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 0; i < g.length; i += 4) { if (Math.abs(g[i] - g[i + 1]) > 20) n++; } return n; })()"
    print("coloured pixels in viewport 0 after Hot Iron:", c.eval(colour))
    c.shot(f"{out}/7-colormap.png")
    # Magnifier (M) held at the centre.
    key(c, 77, "m")
    c.mouse("mousePressed", cx - 30, cy - 20, "left", 1)
    for i in range(6):
        c.mouse("mouseMoved", cx - 30 + i * 5, cy - 20 + i * 3, "left", 1)
        time.sleep(0.05)
    time.sleep(0.6)
    print("magnifier shown:", c.eval("(() => { const m = document.querySelector('.vp canvas.magnifier'); return m ? [getComputedStyle(m).display, m.width, m.height] : null; })()"))
    c.shot(f"{out}/8-magnifier.png")
    c.mouse("mouseReleased", cx, cy - 2, "left", 0)
    time.sleep(0.3)
    print("magnifier after release:", c.eval("getComputedStyle(document.querySelector('.vp canvas.magnifier')).display"))
    # Cross-reference (X): a click in the axial image moves the sagittal and coronal viewports.
    key(c, 88, "x")
    c.mouse("mousePressed", cx + v0[2] * 0.2, cy - v0[3] * 0.15, "left", 1)
    c.mouse("mouseReleased", cx + v0[2] * 0.2, cy - v0[3] * 0.15, "left", 0)
    time.sleep(1.0)
    c.shot(f"{out}/9-cross-reference.png")
    # Arrow from the toolbar, then a text label (T) with the prompt answered.
    c.eval("[...document.querySelectorAll('.toolbar button')].find(b => b.textContent.trim() === 'Arrow').click()")
    time.sleep(0.3)
    c.mouse("mousePressed", cx - 40, cy + 30, "left", 1)
    c.mouse("mouseMoved", cx - 80, cy + 70, "left", 1)
    time.sleep(0.05)
    c.mouse("mouseMoved", cx - 110, cy + 90, "left", 1)
    c.mouse("mouseReleased", cx - 110, cy + 90, "left", 0)
    time.sleep(0.5)
    key(c, 84, "t")
    c.mouse("mousePressed", cx - 110, cy + 100, "left", 1)
    c.mouse("mouseReleased", cx - 110, cy + 100, "left", 0)
    time.sleep(0.8)
    print("text prompt answered:", c.call("Page.handleJavaScriptDialog", accept=True, promptText="cyst?"))
    time.sleep(0.8)
    print("measurement panel:", c.eval("[...document.querySelectorAll('.measurements li')].map(l => l.textContent.trim())"))
    c.shot(f"{out}/10-arrow-text.png")
    # Export through the menu into a download folder; the PNG is the viewport plus the label band.
    dl = os.path.join(out, "downloads")
    os.makedirs(dl, exist_ok=True)
    c.call("Browser.setDownloadBehavior", behavior="allow", downloadPath=dl)
    c.mouse("mousePressed", cx, cy, "right", 2)
    c.mouse("mouseReleased", cx, cy, "right", 0)
    time.sleep(0.4)
    c.eval("[...document.querySelectorAll('.vpmenu > .item')].find(x => x.textContent.startsWith('Export image')).click()")
    time.sleep(1.5)
    files = sorted(os.listdir(dl))
    print("exported files:", files)
    if files:
        canvas = c.eval("[document.querySelector('.vp canvas.img').width, document.querySelector('.vp canvas.img').height]")
        print("export size:", png_size(os.path.join(dl, files[-1])), "viewport canvas:", canvas)
    # About box.
    c.eval("[...document.querySelectorAll('.patientbar button')].find(b => b.textContent.trim() === 'About').click()")
    time.sleep(0.6)
    print("about box:", c.eval("(document.querySelector('.modal.about') || {}).innerText"))
    c.shot(f"{out}/11-about.png")

def click(c, selector, wait=0.5):
    ok = c.eval("(() => { const e = document.querySelector(" + json.dumps(selector) + "); if (!e) return false; e.click(); return true; })()")
    time.sleep(wait)
    return ok

def fill(c, selector, value, wait=0.25):
    """Sets a bound input or select the way a user's edit ends: value, then a change event."""
    ok = c.eval("(() => { const e = document.querySelector(" + json.dumps(selector) + "); if (!e) return false; e.value = " + json.dumps(str(value))
                + "; e.dispatchEvent(new Event('change', { bubbles: true })); return true; })()")
    time.sleep(wait)
    return ok

def wait_for(c, expr, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        v = c.eval(expr)
        if v:
            return v
        time.sleep(0.25)
    return c.eval(expr)

def m2(c, out):
    """The networking UI against the app's own receiver on 127.0.0.1 (no other program involved)."""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    print("M2: network button:", click(c, "button.network"))
    print("M2: receiver before:", c.eval("(document.querySelector('.scp-status') || {}).innerText"))
    click(c, "button.scp-edit")
    # Saving "on" without any allowed sender is refused in plain words.
    click(c, "input.scp-enabled", 0.2)
    fill(c, "input.scp-port", port)
    fill(c, "input.scp-bind", "127.0.0.1")
    click(c, "button.save", 1.0)
    print("M2: receiver without a sender:", c.eval("(document.querySelector('.scp-status') || {}).innerText"), "|", c.eval("(document.querySelector('.network-sheet .err') || {}).innerText"))
    click(c, "button.scp-edit")
    fill(c, "input.scp-allowed", "SELF")
    c.eval("[...document.querySelectorAll('.network-sheet label')].find(l => l.textContent.includes('search and fetch')).querySelector('input').click()")
    time.sleep(0.2)
    print("M2: no 'skip certificate checks' control anywhere:", not c.eval("/skip|insecure|do not check|don't check/i.test(document.querySelector('.network-sheet').innerText)"))
    click(c, "input.scp-any", 0.3)
    print("M2: accept-any warning:", c.eval("(document.querySelector('.network-sheet .banner.danger') || {}).innerText"))
    click(c, "input.scp-any", 0.3)
    click(c, "button.save", 1.5)
    print("M2: receiver after:", c.eval("(document.querySelector('.scp-status') || {}).innerText"))
    # A node with a bad AE title: the form says what to correct and nothing is saved.
    click(c, "button.add-dimse")
    fill(c, "input.node-name", "Loop")
    fill(c, "input.node-host", "127.0.0.1")
    fill(c, "input.node-port", port)
    fill(c, "input.node-called", "THIS_AE_TITLE_IS_TOO_LONG")
    fill(c, "input.node-calling", "SELF")
    click(c, "button.save", 0.8)
    print("M2: validation:", c.eval("[...document.querySelectorAll('.problems li')].map(l => l.textContent)"))
    fill(c, "input.node-called", "SIAARA")
    click(c, "button.save", 1.0)
    print("M2: nodes:", c.eval("[...document.querySelectorAll('table.nodes tbody tr')].map(r => r.innerText.replace(/\\s+/g, ' ').trim())"))
    click(c, "button.node-test", 0.2)
    print("M2: test result:", wait_for(c, "(document.querySelector('.node-test-result') || {}).innerText"))
    c.shot(f"{out}/13-network.png")
    click(c, "button.close")
    # Remote search through the source dropdown.
    node = c.eval("(() => { const o = [...document.querySelectorAll('select.source option')].find(o => o.textContent === 'Loop'); return o ? o.value : null; })()")
    print("M2: node in the source dropdown:", bool(node))
    fill(c, "select.source", node, 0.8)
    print("M2: hint before a search:", c.eval("(document.querySelector('.remote tbody td') || {}).innerText"))
    c.eval("(() => { const e = document.querySelector('input.search'); e.value = 'SAMPLE'; e.dispatchEvent(new Event('input', { bubbles: true })); })()")
    rows = wait_for(c, "[...document.querySelectorAll('.remote tbody tr')].filter(r => r.querySelector('.local')).map(r => r.innerText.replace(/\\s+/g, ' ').trim())")
    print("M2: remote rows:", rows)
    print("M2: status bar:", c.eval("(document.querySelector('.statusbar') || {}).innerText"))
    click(c, ".remote button.expand", 0.2)
    print("M2: series on expand:", wait_for(c, "[...document.querySelectorAll('.remote tr.sub')].map(r => r.innerText.replace(/\\s+/g, ' ').trim()).filter(t => t.startsWith('Series'))"))
    c.shot(f"{out}/14-remote-search.png")
    click(c, ".remote button.retrieve", 0.5)
    print("M2: activity after Retrieve:", wait_for(c, "(() => { const j = [...document.querySelectorAll('.activity .job')].map(j => j.innerText.replace(/\\s+/g, ' ').trim()); return j.some(t => t.includes('Done')) ? j : null; })()"))
    c.shot(f"{out}/15-activity.png")
    click(c, ".activity .btn")
    # Send a local study to the same node.
    fill(c, "select.source", "", 1.0)
    row = c.eval("(() => { const r = [...document.querySelectorAll('table.grid tbody tr')].find(t => t.textContent.includes('CT PHANTOM')); if (!r) return null; const b = r.getBoundingClientRect(); return [b.x + 20, b.y + b.height / 2]; })()")
    if row:
        c.mouse("mousePressed", row[0], row[1], "left", 1)
        c.mouse("mouseReleased", row[0], row[1], "left", 0)
        time.sleep(0.5)
    print("M2: send button:", click(c, "button.send"))
    click(c, "input.allow-incomplete", 0.3)
    print("M2: incomplete-files warning:", c.eval("(document.querySelector('.modal .banner.danger') || {}).innerText"))
    click(c, "input.allow-incomplete", 0.3)
    click(c, "button.send-go", 0.5)
    print("M2: activity after Send:", wait_for(c, "(() => { const j = [...document.querySelectorAll('.activity .job')].map(j => j.innerText.replace(/\\s+/g, ' ').trim()); return j.filter(t => t.startsWith('Send') && t.includes('Done')).length ? j : null; })()", 60))
    c.shot(f"{out}/16-send.png")
    click(c, ".activity .btn")
    # Switch the receiver off again; the node stays.
    click(c, "button.network")
    click(c, "button.scp-edit")
    click(c, "input.scp-enabled", 0.2)
    click(c, "button.save", 1.5)
    print("M2: receiver switched off:", c.eval("(document.querySelector('.scp-status') || {}).innerText"))
    click(c, "button.close")

def main():
    chrome, launch, study, out = sys.argv[1:5]
    if os.path.isfile(launch):
        # The one-shot launch page: its content must hold the secret, its address and Chrome's command line must not.
        with open(launch, encoding="utf-8") as f:
            page = f.read()
        print("launch page: mode", oct(os.stat(launch).st_mode & 0o777), "posts a form:", 'method="post"' in page)
        launch_file = launch
        launch = "file://" + urllib.parse.quote(os.path.abspath(launch))
    else:
        launch_file = None
        launch = resolve_launch(launch)
    os.makedirs(out, exist_ok=True)
    port = 9333
    proc = subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={port}",
                             f"--user-data-dir={out}/profile", "--window-size=1400,900", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        c = Cdp(port)
        c.call("Page.enable")
        c.call("Runtime.enable")
        c.call("Log.enable")
        c.call("Page.navigate", url=launch)
        time.sleep(4)
        print("browser page title:", c.eval("document.title"))
        host = c.eval("location.host")
        print("served under a random *.localhost name:", bool(host) and host.split(":")[0].endswith(".localhost") and len(host.split(":")[0]) == 42)
        print("cookie visible to scripts (HttpOnly):", repr(c.eval("document.cookie")))
        if launch_file:
            print("launch page deleted after use:", not os.path.exists(launch_file))
        print("rows in the study grid:", c.eval("document.querySelectorAll('table.grid tbody tr').length"))
        print("blazor connected:", c.eval("!!(window.Blazor)"))
        c.shot(f"{out}/1-browser.png")
        # Select the CT study and open it by double-click (the real UI path).
        row = c.eval("(() => { const r = [...document.querySelectorAll('table.grid tbody tr')].find(t => t.textContent.includes('CT PHANTOM')); if (!r) return null; const b = r.getBoundingClientRect(); return [b.x + 20, b.y + b.height / 2]; })()")
        print("CT row at:", row)
        if row:
            for k in ("mousePressed", "mouseReleased"):
                c.mouse(k, row[0], row[1], "left", 1)
            c.call("Input.dispatchMouseEvent", type="mousePressed", x=row[0], y=row[1], button="left", buttons=1, clickCount=2)
            c.call("Input.dispatchMouseEvent", type="mouseReleased", x=row[0], y=row[1], button="left", buttons=0, clickCount=2)
        time.sleep(4)
        print("viewer url:", c.eval("location.pathname"))
        print("study UID in the viewer address:", study in (c.eval("location.href") or ""))
        print("viewports:", c.eval("document.querySelectorAll('.vp').length"))
        vp = c.eval("(() => { const b = document.querySelector('.vp').getBoundingClientRect(); return [b.x, b.y, b.width, b.height]; })()")
        print("viewport rect:", vp)
        stats = "(() => { const c = document.querySelector('.vp canvas.img'); const g = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let s = 0, nz = 0; for (let i = 0; i < g.length; i += 4) { s += g[i]; if (g[i] > 0) nz++; } return [c.width, c.height, Math.round(s / (g.length / 4)), nz]; })()"
        print("image canvas (w, h, mean, non-zero px):", c.eval(stats))
        c.shot(f"{out}/2-viewer.png")
        cx, cy = vp[0] + vp[2] / 2, vp[1] + vp[3] / 2
        # Right-drag: window / level.
        c.mouse("mouseMoved", cx, cy)
        c.mouse("mousePressed", cx, cy, "right", 2)
        for i in range(1, 11):
            c.mouse("mouseMoved", cx + i * 12, cy + i * 6, "right", 2)
            time.sleep(0.03)
        c.mouse("mouseReleased", cx + 120, cy + 60, "right", 0)
        time.sleep(0.5)
        print("after W/L drag (w, h, mean, non-zero px):", c.eval(stats))
        # Wheel: next slices.
        for _ in range(5):
            c.call("Input.dispatchMouseEvent", type="mouseWheel", x=cx, y=cy, deltaX=0, deltaY=100)
            time.sleep(0.05)
        # Length tool (L) and a drag.
        c.call("Input.dispatchKeyEvent", type="rawKeyDown", windowsVirtualKeyCode=76, key="l", code="KeyL")
        c.call("Input.dispatchKeyEvent", type="keyUp", windowsVirtualKeyCode=76, key="l", code="KeyL")
        time.sleep(0.2)
        c.mouse("mousePressed", cx - 100, cy, "left", 1)
        c.mouse("mouseMoved", cx - 20, cy + 40, "left", 1)
        time.sleep(0.05)
        c.mouse("mouseMoved", cx + 60, cy + 80, "left", 1)
        c.mouse("mouseReleased", cx + 60, cy + 80, "left", 0)
        time.sleep(0.8)
        print("measurement panel:", c.eval("[...document.querySelectorAll('.measurements li')].map(l => l.textContent.trim())"))
        c.shot(f"{out}/3-viewer-wl-length.png")
        # 2x2 layout through the layout picker.
        c.eval("document.querySelector('.layoutpicker > button').click()")
        time.sleep(0.3)
        c.eval("document.querySelectorAll('.layoutpicker .cell')[5].click()")
        time.sleep(2)
        print("viewports after 2x2:", c.eval("document.querySelectorAll('.vp').length"))
        c.shot(f"{out}/4-viewer-2x2.png")
        m11(c, out)
        errors = [e for e in c.events if e.get("method") in ("Log.entryAdded", "Runtime.exceptionThrown")
                  or (e.get("method") == "Runtime.consoleAPICalled" and e["params"].get("type") == "error")]
        print("browser console errors:", len(errors))
        for e in errors[:10]:
            print("  ", json.dumps(e.get("params"))[:300])
        csp = [e for e in c.events if "Content Security Policy" in json.dumps(e.get("params", {}))]
        print("Content-Security-Policy violations in the console:", len(csp))
        # Sign out from the study browser: the window is locked and a reload is refused.
        c.call("Page.navigate", url=c.eval("location.origin") + "/")
        time.sleep(3)
        before = len(c.events)
        m2(c, out)
        errors = [e for e in c.events[before:] if e.get("method") == "Runtime.exceptionThrown"
                  or (e.get("method") == "Runtime.consoleAPICalled" and e["params"].get("type") == "error")]
        print("M2: browser console errors:", len(errors))
        for e in errors[:10]:
            print("  ", json.dumps(e.get("params"))[:300])
        c.eval("document.querySelector('button.signout').click()")
        time.sleep(2)
        print("after Sign out:", c.eval("location.pathname + ' | ' + (document.querySelector('h1') || {}).textContent"))
        c.call("Page.navigate", url=c.eval("location.origin") + "/")
        time.sleep(2)
        print("study list after Sign out:", c.eval("(document.querySelector('h1') || {}).textContent"), "| grid rows:", c.eval("document.querySelectorAll('table.grid tbody tr').length"))
        c.shot(f"{out}/12-signed-out.png")
    finally:
        proc.terminate()

if __name__ == "__main__":
    main()
