// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// The browser side of the viewer. It shows the frames the server renders and sends input; it does
// no image processing (decision D2). Two canvases per viewport: "img" shows the frame as sent
// (RGBA copied, gray8 unpacked to RGBA for putImageData), "ann" draws annotations, overlay text,
// orientation labels and warnings from the vector data in the state message.
//
// Protocol (binary, little endian), see Streaming/FrameProtocol.cs:
//   frame  u8 1, u8 fmt, u16 vp, u32 seq, u16 w, u16 h, pixels   (server -> browser)
//   state  u8 2, u16 vp, u32 seq, JSON                            (server -> browser)
//   ack    u8 3, u16 vp, u32 seq                                  (browser -> server)
//   input  u8 4, u16 vp, u32 seq, u8 kind, i16 x, i16 y, u8 buttons, i16 wheel, u16 key, u8 mods, u16 w, u16 h
//   command u8 5, u16 vp, JSON
(function () {
    'use strict';

    const T_FRAME = 1, T_STATE = 2, T_ACK = 3, T_INPUT = 4, T_CMD = 5;
    const K_DOWN = 0, K_MOVE = 1, K_UP = 2, K_WHEEL = 3, K_KEY = 4, K_RESIZE = 5;
    const FMT_RGBA = 1, FMT_GRAY = 2;
    const HANDLED_KEYS = new Set([9, 13, 27, 32, 33, 34, 35, 36, 37, 38, 39, 40, 49, 50, 51, 52, 53, 54, 55, 56, 57,
        65, 69, 70, 72, 73, 76, 80, 82, 86, 87, 89, 90, 119]);
    const SPACING_ICON = { pixel: 'PS', imager: 'IS', magnified: 'IM', nominal: 'NS', us: 'US', manual: 'CAL', none: 'px' };

    let client = null;

    class ViewportView {
        constructor(index, host) {
            this.index = index;
            this.attach(host);
            this.shownSeq = 0;
            this.state = null;
            this.pending = new Map();
            this.prompted = new Set();
        }

        attach(host) {
            this.host = host;
            this.img = host.querySelector('canvas.img');
            this.ann = host.querySelector('canvas.ann');
            this.ictx = this.img.getContext('2d', { alpha: false });
            this.actx = this.ann.getContext('2d');
            this.imageData = null;
        }
    }

    class ViewClient {
        constructor(sessionId) {
            this.sessionId = sessionId;
            this.views = new Map();
            this.seq = 1;
            this.active = 0;
            this.pendingMove = null;
            this.moveScheduled = false;
            this.resizeObserver = new ResizeObserver(entries => {
                for (const e of entries) {
                    const idx = Number(e.target.dataset.vp);
                    this.sendResize(idx, e.target);
                }
            });
            this.onKey = this.onKey.bind(this);
            document.addEventListener('keydown', this.onKey, true);
            this.open();
        }

        open() {
            const url = 'ws://' + location.host + '/ws/view?s=' + encodeURIComponent(this.sessionId);
            const ws = new WebSocket(url);
            ws.binaryType = 'arraybuffer';
            ws.onopen = () => {
                this.command(0, { dpr: window.devicePixelRatio || 1 });
                for (const v of this.views.values()) {
                    this.sendResize(v.index, v.host);
                }
            };
            ws.onmessage = ev => this.onMessage(ev.data);
            ws.onclose = () => {
                if (!this.closed) {
                    setTimeout(() => this.open(), 1000);
                }
            };
            this.ws = ws;
        }

        close() {
            this.closed = true;
            document.removeEventListener('keydown', this.onKey, true);
            this.resizeObserver.disconnect();
            if (this.ws) {
                this.ws.close();
            }
        }

        // ---------------------------------------------------------------- DOM binding

        bind() {
            const hosts = document.querySelectorAll('[data-vp]');
            const seen = new Set();
            hosts.forEach(host => {
                const idx = Number(host.dataset.vp);
                seen.add(idx);
                let v = this.views.get(idx);
                if (!v) {
                    v = new ViewportView(idx, host);
                    this.views.set(idx, v);
                } else if (v.host !== host) {
                    this.resizeObserver.unobserve(v.host);
                    v.attach(host);
                    v.shownSeq = 0;
                }
                if (!host.dataset.bound) {
                    host.dataset.bound = '1';
                    this.wire(host, idx);
                    this.resizeObserver.observe(host);
                }
                if (host.classList.contains('active')) {
                    this.active = idx;
                }
            });
            for (const idx of [...this.views.keys()]) {
                if (!seen.has(idx)) {
                    this.views.delete(idx);
                }
            }
        }

        wire(host, idx) {
            host.addEventListener('contextmenu', e => e.preventDefault());
            host.addEventListener('pointerdown', e => {
                host.setPointerCapture(e.pointerId);
                this.active = idx;
                e.preventDefault();
                this.input(idx, K_DOWN, e, this.buttonsDown(e));
            });
            host.addEventListener('pointermove', e => {
                // Coalesce moves to one per animation frame; the server coalesces further.
                this.pendingMove = { idx, e, buttons: e.buttons };
                if (!this.moveScheduled) {
                    this.moveScheduled = true;
                    requestAnimationFrame(() => {
                        this.moveScheduled = false;
                        const m = this.pendingMove;
                        this.pendingMove = null;
                        if (m) {
                            this.input(m.idx, K_MOVE, m.e, m.buttons);
                        }
                    });
                }
            });
            host.addEventListener('pointerup', e => {
                if (this.pendingMove && this.pendingMove.idx === idx) {
                    this.input(idx, K_MOVE, this.pendingMove.e, this.pendingMove.buttons);
                    this.pendingMove = null;
                }
                this.input(idx, K_UP, e, 0);
            });
            host.addEventListener('wheel', e => {
                e.preventDefault();
                const delta = e.deltaY === 0 ? 0 : (e.deltaY > 0 ? 1 : -1);
                this.input(idx, K_WHEEL, e, 0, delta);
            }, { passive: false });
        }

        buttonsDown(e) {
            // pointerdown's e.buttons already includes the pressed button.
            return e.buttons || (e.button === 2 ? 2 : e.button === 1 ? 4 : 1);
        }

        dpr() {
            return window.devicePixelRatio || 1;
        }

        sendResize(idx, host) {
            const v = this.views.get(idx);
            if (!v) {
                return;
            }
            const r = host.getBoundingClientRect();
            const d = this.dpr();
            const w = Math.max(1, Math.round(r.width * d)), h = Math.max(1, Math.round(r.height * d));
            if (v.img.width !== w || v.img.height !== h) {
                v.img.width = w; v.img.height = h;
                v.ann.width = w; v.ann.height = h;
                v.imageData = null;
            }
            this.send(this.inputMessage(idx, K_RESIZE, 0, 0, 0, 0, 0, 0, w, h));
        }

        mods(e) {
            return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
        }

        input(idx, kind, e, buttons, wheel) {
            const v = this.views.get(idx);
            if (!v) {
                return;
            }
            const r = v.host.getBoundingClientRect();
            const d = this.dpr();
            const x = Math.round((e.clientX - r.left) * d), y = Math.round((e.clientY - r.top) * d);
            this.send(this.inputMessage(idx, kind, x, y, buttons, wheel || 0, 0, this.mods(e), 0, 0));
        }

        onKey(e) {
            const t = e.target;
            if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) {
                return;
            }
            const code = e.keyCode;
            if (!HANDLED_KEYS.has(code) || this.views.size === 0) {
                return;
            }
            e.preventDefault();
            this.send(this.inputMessage(this.active, K_KEY, 0, 0, 0, 0, code, this.mods(e), 0, 0));
        }

        inputMessage(idx, kind, x, y, buttons, wheel, key, mods, w, h) {
            const b = new ArrayBuffer(22);
            const dv = new DataView(b);
            dv.setUint8(0, T_INPUT);
            dv.setUint16(1, idx, true);
            dv.setUint32(3, this.seq++ >>> 0, true);
            dv.setUint8(7, kind);
            dv.setInt16(8, x, true);
            dv.setInt16(10, y, true);
            dv.setUint8(12, buttons & 255);
            dv.setInt16(13, wheel, true);
            dv.setUint16(15, key, true);
            dv.setUint8(17, mods);
            dv.setUint16(18, w, true);
            dv.setUint16(20, h, true);
            return b;
        }

        command(idx, obj) {
            const json = new TextEncoder().encode(JSON.stringify(obj));
            const b = new Uint8Array(3 + json.length);
            b[0] = T_CMD;
            b[1] = idx & 255;
            b[2] = (idx >> 8) & 255;
            b.set(json, 3);
            this.send(b.buffer);
        }

        ack(idx, seq) {
            const b = new ArrayBuffer(7);
            const dv = new DataView(b);
            dv.setUint8(0, T_ACK);
            dv.setUint16(1, idx, true);
            dv.setUint32(3, seq, true);
            this.send(b);
        }

        send(buf) {
            if (this.ws && this.ws.readyState === WebSocket.OPEN) {
                this.ws.send(buf);
            }
        }

        // ---------------------------------------------------------------- messages

        onMessage(data) {
            const dv = new DataView(data);
            const type = dv.getUint8(0);
            if (type === T_FRAME) {
                this.onFrame(dv, data);
            } else if (type === T_STATE) {
                const idx = dv.getUint16(1, true), seq = dv.getUint32(3, true);
                const json = new TextDecoder().decode(new Uint8Array(data, 7));
                this.onState(idx, seq, JSON.parse(json));
            }
        }

        onFrame(dv, data) {
            const fmt = dv.getUint8(1), idx = dv.getUint16(2, true), seq = dv.getUint32(4, true);
            const w = dv.getUint16(8, true), h = dv.getUint16(10, true);
            const v = this.views.get(idx);
            if (v && seq > v.shownSeq && w > 0 && h > 0) {
                if (v.img.width !== w || v.img.height !== h) {
                    // A frame for the previous size: show it scaled until the new one arrives.
                    v.img.width = w; v.img.height = h;
                    v.imageData = null;
                }
                if (!v.imageData || v.imageData.width !== w || v.imageData.height !== h) {
                    v.imageData = v.ictx.createImageData(w, h);
                }
                const out = v.imageData.data;
                const px = new Uint8Array(data, 12);
                if (fmt === FMT_RGBA) {
                    out.set(px.subarray(0, w * h * 4));
                } else if (fmt === FMT_GRAY) {
                    for (let i = 0, j = 0, n = w * h; i < n; i++, j += 4) {
                        const g = px[i];
                        out[j] = g; out[j + 1] = g; out[j + 2] = g; out[j + 3] = 255;
                    }
                }
                v.ictx.putImageData(v.imageData, 0, 0);
                v.shownSeq = seq;
                const st = v.pending.get(seq);
                if (st) {
                    v.state = st;
                }
                for (const k of [...v.pending.keys()]) {
                    if (k <= seq) {
                        v.pending.delete(k);
                    }
                }
                this.drawOverlay(v);
            }
            // Always acknowledge, also a stale frame, so the server never waits for it.
            this.ack(idx, seq);
        }

        onState(idx, seq, st) {
            const v = this.views.get(idx);
            if (!v) {
                return;
            }
            if (seq > v.shownSeq) {
                v.pending.set(seq, st);
                if (st.message) {
                    v.state = st;
                    v.ictx.fillStyle = '#000';
                    v.ictx.fillRect(0, 0, v.img.width, v.img.height);
                    this.drawOverlay(v);
                }
                return;
            }
            if (seq === v.shownSeq || st.message) {
                v.state = st;
                this.drawOverlay(v);
            }
        }

        // ---------------------------------------------------------------- drawing

        drawOverlay(v) {
            const c = v.actx, st = v.state, W = v.ann.width, H = v.ann.height;
            c.clearRect(0, 0, W, H);
            if (!st) {
                return;
            }
            const d = this.dpr();
            const font = Math.round(12 * d);
            c.font = font + 'px system-ui, sans-serif';
            c.textBaseline = 'top';
            v.host.classList.toggle('active', !!st.active);
            if (st.active) {
                this.active = v.index;
            }
            if (st.message) {
                c.fillStyle = '#9aa4b2';
                c.textAlign = 'center';
                c.fillText(st.message, W / 2, H / 2);
                c.textAlign = 'left';
            }
            for (const a of st.annotations || []) {
                this.drawAnnotation(c, a, d, font);
            }
            if (st.overlayText) {
                const t = st.overlayText, pad = 6 * d, lh = font + 3 * d;
                this.corner(c, t.topLeft, pad, pad, 'left', lh, false);
                this.corner(c, t.topRight, W - pad, pad, 'right', lh, false);
                this.corner(c, t.bottomLeft, pad, H - pad, 'left', lh, true);
                this.corner(c, t.bottomRight, W - pad, H - pad, 'right', lh, true);
            }
            if (st.orientation) {
                const o = st.orientation;
                c.fillStyle = '#ffd34d';
                c.textAlign = 'center';
                c.fillText(o.top, W / 2, 6 * d + font * 6);
                c.fillText(o.bottom, W / 2, H - font * 7);
                c.textAlign = 'left';
                c.fillText(o.left, 6 * d, H / 2);
                c.textAlign = 'right';
                c.fillText(o.right, W - 6 * d, H / 2);
                c.textAlign = 'left';
            }
            const warnings = st.warnings || [];
            if (warnings.length) {
                c.textAlign = 'center';
                let y = H / 2 - warnings.length * (font + 4 * d) - 40 * d;
                y = Math.max(y, font * 6);
                for (const w of warnings) {
                    const tw = c.measureText('⚠ ' + w).width;
                    c.fillStyle = 'rgba(120, 60, 0, 0.85)';
                    c.fillRect(W / 2 - tw / 2 - 6 * d, y - 2 * d, tw + 12 * d, font + 4 * d);
                    c.fillStyle = '#ffcf7a';
                    c.fillText('⚠ ' + w, W / 2, y);
                    y += font + 6 * d;
                }
                c.textAlign = 'left';
            }
            if (st.calibrationPrompt && !v.prompted.has(st.calibrationPrompt)) {
                v.prompted.add(st.calibrationPrompt);
                const id = st.calibrationPrompt;
                setTimeout(() => {
                    const text = window.prompt('True length of the calibration line in mm:', '');
                    const mm = text === null ? 0 : parseFloat(text.replace(',', '.'));
                    this.command(v.index, { calibrate: { id: id, mm: isFinite(mm) ? mm : 0 } });
                }, 0);
            }
        }

        corner(c, lines, x, y, align, lh, bottom) {
            if (!lines || !lines.length) {
                return;
            }
            c.textAlign = align;
            c.fillStyle = '#e6edf3';
            c.shadowColor = '#000';
            c.shadowBlur = 3;
            for (let i = 0; i < lines.length; i++) {
                const yy = bottom ? y - (lines.length - i) * lh : y + i * lh;
                c.fillText(lines[i], x, yy);
            }
            c.shadowBlur = 0;
            c.textAlign = 'left';
        }

        drawAnnotation(c, a, d, font) {
            const p = a.points;
            if (!p || p.length < 2) {
                return;
            }
            c.lineWidth = 1.5 * d;
            c.strokeStyle = a.drawing ? '#7dd3fc' : '#facc15';
            c.setLineDash(a.drawing ? [6 * d, 4 * d] : []);
            c.beginPath();
            c.moveTo(p[0], p[1]);
            for (let i = 2; i < p.length; i += 2) {
                c.lineTo(p[i], p[i + 1]);
            }
            if (a.closed) {
                c.closePath();
            }
            c.stroke();
            c.setLineDash([]);
            if (a.kind === 'length' || a.kind === 'calibration' || a.kind === 'angle' || a.kind === 'cobb' || a.kind === 'polyline') {
                c.fillStyle = c.strokeStyle;
                for (let i = 0; i < p.length; i += 2) {
                    c.fillRect(p[i] - 2 * d, p[i + 1] - 2 * d, 4 * d, 4 * d);
                }
            }
            let lx = p[p.length - 2] + 8 * d, ly = p[p.length - 1] + 4 * d;
            const label = (a.label || '') + (a.warnings && a.warnings.length ? '  ⚠ ' + a.warnings.join('; ') : '');
            if (label) {
                const icon = SPACING_ICON[a.spacing] || '';
                const text = icon && a.kind !== 'probe' ? label + '  [' + icon + ']' : label;
                const tw = c.measureText(text).width;
                lx = Math.min(lx, c.canvas.width - tw - 4 * d);
                ly = Math.min(ly, c.canvas.height - font - 4 * d);
                c.fillStyle = 'rgba(0, 0, 0, 0.6)';
                c.fillRect(lx - 3 * d, ly - 2 * d, tw + 6 * d, font + 4 * d);
                c.fillStyle = a.warnings && a.warnings.length ? '#ffcf7a' : '#facc15';
                c.fillText(text, lx, ly);
            }
        }
    }

    window.siaaraView = {
        connect(sessionId) {
            if (client) {
                client.close();
            }
            client = new ViewClient(sessionId);
            client.bind();
        },
        bind() {
            if (client) {
                client.bind();
                for (const v of client.views.values()) {
                    client.sendResize(v.index, v.host);
                }
            }
        },
        command(viewport, obj) {
            if (client) {
                client.command(viewport, obj);
            }
        },
        disconnect() {
            if (client) {
                client.close();
                client = null;
            }
        }
    };
})();
