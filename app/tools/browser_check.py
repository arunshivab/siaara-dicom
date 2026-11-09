#!/usr/bin/env python3
# SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
#
# Drives a headless Chrome / Chromium through the DevTools protocol (standard library only) against
# a running host: opens the launch URL, screenshots the study browser, opens the viewer, does a
# right-drag (W/L), a wheel scroll, a length measurement and the 2x2 layout, and screenshots again.
# Usage: browser_check.py CHROME_BINARY LAUNCH_URL STUDY_UID OUT_DIR
import base64, json, os, socket, struct, subprocess, sys, time, urllib.parse, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke import ws_connect, ws_send, recv_exact  # noqa: E402

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

def main():
    chrome, launch, study, out = sys.argv[1:5]
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
        errors = [e for e in c.events if e.get("method") in ("Log.entryAdded", "Runtime.exceptionThrown")
                  or (e.get("method") == "Runtime.consoleAPICalled" and e["params"].get("type") == "error")]
        print("browser console errors:", len(errors))
        for e in errors[:10]:
            print("  ", json.dumps(e.get("params"))[:300])
    finally:
        proc.terminate()

if __name__ == "__main__":
    main()
