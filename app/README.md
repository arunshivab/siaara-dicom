# SIAARA Viewer (M1: local 2D viewer)

Copyright (c) Sthraas Intelligence Pvt Ltd. Proprietary. Written for this project; no code is taken
from any other viewer.

A PACS viewer in C# on .NET 10 with Blazor on ASP.NET Core, on top of the SIAARA C++ DICOM toolkit
through the C# bindings in `bindings/csharp`. The toolkit renders; frames stream to the browser as raw
pixels; the browser shows them and sends input (decision D2).

## Layout

```
app/
  Siaara.Viewer.slnx          solution (app projects + the two binding projects)
  Directory.Build.props       net10.0, nullable, warnings as errors, AnalysisLevel latest-all, code style in build
  Directory.Build.targets     build check: fails if any project has a PackageReference
  src/Siaara.Viewer.Core/     no UI: Toolkit (Tk.cs facade, ToolkitScheduler), Store, Import, Imaging, Viewing,
                              Measure, Text, Settings, Audit (signed log, key store), Logging, Samples,
                              Platform (owner-only files, peer-user check, *.localhost probe; all P/Invoke is here)
  src/Siaara.Viewer.Web/      Razor class library: Pages (Browser, Viewer), Components, Streaming (/ws/view),
                              Security (launch secret, cookie, Host, Origin, peer), wwwroot/js/siaara-view.js, wwwroot/css/siaara.css
  src/Siaara.Viewer.Host/     siaara-viewer: ABI check, single instance, store, Kestrel 127.0.0.1:0, LaunchPages,
                              BrowserLauncher (A1 a)
  tests/Siaara.Viewer.Tests/  console test runner (no test framework packages)
  tools/smoke.py              scripted check of a running host (HTTP + a hand-written WebSocket client)
  tools/browser_check.py      drives headless Chrome over the DevTools protocol against a running host
```

`Toolkit/Tk.cs` is the only file that names binding types; the rest of the app uses the app-owned
types declared there (`TkStore`, `TkFile`, `TkFrame`, `RenderSpec`, ...).

## Build

Needs the .NET 10 SDK and a C++ toolchain with CMake for the toolkit.

```
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j2 --target siaara
dotnet build app/Siaara.Viewer.slnx -c Release
```

The native library is copied into every output folder as `runtimes/<rid>/native/libsiaara.so`
(`siaara.dll`, `libsiaara.dylib`) by the `Siaara.Interop` project, as for the bindings
(`-p:SiaaraNativeBuildDir=...` or `-p:SiaaraNativeLibrary=...` to point elsewhere).

Dependencies: none beyond the .NET SDK and the ASP.NET Core shared framework (the build fails on any
`PackageReference`).

Zero downloads (owner decision b): the build never touches the network.

- In .NET 10, Blazor's script (`_framework/blazor.web.js`) ships as the SDK pack
  `Microsoft.AspNetCore.App.Internal.Assets`, which the Web SDK would fetch from nuget.org (it does so
  only when the app project itself has `.razor` files; ours are in the class library, so the host
  sets `RequiresAspNetWebAssets=true`). A copy of exactly that pack (10.0.14, the version SDK 10.0.403
  resolves; SHA-256 in `vendor/packs/SHA256SUMS`) is kept in `vendor/packs/`. `NuGet.config` clears
  every other source and `Directory.Build.props` points `RestoreSources` / `RestoreConfigFile` at
  that folder only; `Directory.Build.targets` pins the pack version to the vendored file, so a newer
  10.0 SDK still restores offline. To move to another pack version, copy the new `.nupkg` from
  `~/.nuget/packages/microsoft.aspnetcore.app.internal.assets/<v>/`, update `SHA256SUMS` and
  `SiaaraAspNetAssetsPackVersion`.
- The bindings projects set `IsAotCompatible` / `IsTrimmable`, which would make the SDK fetch the
  trimming analyzer pack `Microsoft.NET.ILLink.Tasks`. `Directory.Build.rsp` (read by MSBuild for any
  build started under `app/`) turns those two off for the app build only; the bindings' own solution
  keeps its analyzers. NuGet audit is off for the same reason (it would query the feed).
- Proof: a fresh `NUGET_PACKAGES` folder, a dead proxy and no network interface (`unshare -rn`):
  `dotnet build app/Siaara.Viewer.slnx -c Release` succeeds and the folder then holds only
  `microsoft.aspnetcore.app.internal.assets` (copied from `vendor/packs`); no HTTP cache is created.
- Not covered: a self-contained or RID-specific publish (`-r <rid>`) needs the runtime packs
  `Microsoft.NETCore.App.Runtime.<rid>` and `Microsoft.AspNetCore.App.Runtime.<rid>`, which are not
  vendored (owner decision pending). Builds from an IDE that ignores `Directory.Build.rsp` would ask
  for the ILLink pack.

## Run

```
dotnet app/src/Siaara.Viewer.Host/bin/Release/net10.0/siaara-viewer.dll [options]
  --data-dir DIR       per-user folder (default %LOCALAPPDATA%\Siaara, ~/Library/Application Support/Siaara,
                       $XDG_DATA_HOME/siaara): settings.json, store/, logs/, audit/, annotations/, launch/
  --store DIR          image store (default: settings, then DIR/store)
  --import DIR         import a folder or a DICOMDIR at start (same path as the Import sheet)
  --no-browser         do not open a window
  --print-launch-file  print the PATH of a one-shot, owner-only launch page (no secret on standard output)
  --print-url          print the launch URL; it holds the launch secret (see "Launch and session")
  --host auto|name|ip  host name the app is served under (default auto; see "Launch and session")
  --no-idle-exit       keep running when no window is connected (default: exit after 60 s)
  --verbose            echo the (redacted) app log to stderr
siaara-viewer --generate-samples DIR [--full]
                       synthetic CT (60 slices, or 500 x 512x512 with --full), US clip, PET, CR, and a DICOMDIR set
```

Start-up: ABI check ("toolkit version mismatch" and exit), single-instance lock (a second start
opens the running instance's window and exits), store open (create, durable, rebuild if corrupt),
Kestrel on 127.0.0.1 at a random port, then the window: Edge / Chrome / Chromium with `--app=FILE`
when found, else the default browser (`BrowserLauncher`, the only class that knows). FILE is the
one-shot launch page described below, never an address with a secret.

### Launch and session (security review round 4)

- **Host name (R-124).** The app is served as `http://<32 random hex digits>.localhost:<port>`.
  Kestrel still binds 127.0.0.1 only; Edge, Chrome, Chromium and Firefox resolve every `*.localhost`
  name to the loopback interface themselves. Requests must carry exactly that `Host`; plain
  `127.0.0.1:<port>`, `localhost:<port>` and any other name get 400. The session cookie
  (`siaara_session`) is host-only (no Domain), HttpOnly, SameSite=Strict, Path=/, so the browser
  never sends it to another program listening on a 127.0.0.1 port. (`__Host-` needs `Secure`, which
  plain http does not have.) Fallback, `--host ip`: the host is `127.0.0.1` and the cookie is named
  `siaara_session_<port>`; browsers then send it to every 127.0.0.1 port, as before this change.
  `--host auto` (default) takes the fallback only when the window has to be opened by an unknown
  default browser (no Edge / Chrome / Chromium found) on a system whose own resolver does not know
  `*.localhost` names (asked through `getaddrinfo`; Safari and GNOME Web ask the system). It is
  logged as a warning and recorded in `app.start` (`host: ip`). `--host name` never falls back.
- **Launch secret (R-113).** A launch secret is 256 random bits and works once. It is never put on
  a command line: the host writes `launch/launch-<hash>.html` in the per-user data folder, readable
  by the current user only from the create call on (0600 / owner-only ACL), and opens the browser
  on that file. The page posts the secret to `/launch` (a form submitted by one line of script, with
  a button when scripts are off), so it is not in an address or the browser history either.
  `/launch` answers with the cookie and a page of the app that moves on to `/` (not a redirect: a
  SameSite=Strict cookie is not sent along a redirect that started on another site). The file is
  deleted when the secret is used, after 5 minutes (the secret is then withdrawn), when the app
  stops, and at the next start. A second start reads `instance.url` (owner-only, created so in one
  step, refused when others can read it), which always holds one unused secret, writes the same
  kind of page and opens it.
- **Wrong, expired or repeated secret.** The browser shows "This link was already used" (or
  expired / does not work) and what to do; the attempt is a `security.alert` in the audit log.
- **`--print-url` and the no-browser fallback (R-158).** `--print-url` writes
  `http://<name>.localhost:<port>/launch?k=<secret>` to standard output. Whoever can read that
  output (a terminal log, a CI log, a wrapper script) within 5 minutes can open the viewer once,
  and the secret lands in that browser's history (useless after its one use). The same line is
  printed when no browser could be started. Use `--print-launch-file` instead where possible: it
  prints only the path of the owner-only page.
- **Peer user (R-113).** Every connection must come from a process of the same OS user; anything
  else gets 403 with no content, for every path, and is a `security.alert` (`peer-other-user` or
  `peer-unknown`). If the owner cannot be determined the connection is refused (`PeerPolicy.Allows`
  is the one rule). Linux: `/proc/net/tcp` and `tcp6`, the uid of the socket whose local port is
  the peer's port and whose remote port is ours, both loopback. Windows: `GetExtendedTcpTable`
  (owner PID), then the user SID of that process's token compared with ours. macOS: `libproc` over
  the current user's own processes (`proc_listpids` PROC_UID_ONLY, `PROC_PIDLISTFDS`,
  `PROC_PIDFDSOCKETINFO`): the peer is accepted only when one of the user's own processes owns the
  socket with that port pair; another user's (or root's) process cannot be inspected and is refused.
  The Windows and macOS native calls are compiled on every OS but have run only where CI runs them;
  see "Still missing / not verified".
- **Session lifetime.** The cookie has no expiry date (a browser session cookie) and its value
  lives in the host's memory only: it stops working when the app stops (60 s after the last window
  closes, unless `--no-idle-exit`) or when the user chooses **Sign out** in the study browser.
  Sign out invalidates the cookie at once for every window, closes the viewer sessions and their
  sockets, and shows "Signed out"; starting SIAARA Viewer again opens a new window (`POST /signout`
  does the same for scripts).
- **Other checks.** WebSocket upgrades (the Blazor circuit and `/ws/view`) and non-GET requests need
  the app's exact Origin. Every response is `Cache-Control: no-store` (pages, thumbnails and API
  answers can hold patient data), except the static framework files. Content-Security-Policy
  (R-125): `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self'
  data: blob:; connect-src 'self' ws://<host>:<port>; object-src 'none'; base-uri 'self';
  form-action 'self'; frame-ancestors 'none'` (one header; Blazor's own is switched off).
  `base-uri` is `'self'`, not `'none'`: the root document has `<base href="/">`, which Blazor needs.
- **No UIDs in addresses (R-145).** `/viewer/<id>` and `/api/thumb?series=<id>` use random per-run
  ids (`OpaqueIds`); `POST /api/sessions` takes the Study Instance UID as its body. At most 8 viewer
  sessions are open (a ninth replaces the one longest without a window; with 8 windows connected,
  opening fails with a message); Kestrel accepts at most 64 connections and 32 WebSockets.

### Files, permissions and the audit log

- **Owner-only files (R-143).** The data folder and its `logs`, `audit`, `annotations` and `launch`
  folders are created 0700 (owner-only ACL on Windows); audit files, the anchor, the key, app logs,
  `settings.json`, annotation files, `instance.url`, `instance.lock` and launch pages are created
  0600 in the create call itself. A log file left with a wider mode by an earlier version is
  narrowed when it is opened. An existing data folder is not changed: when its group or everybody
  can reach it, the study browser shows a banner, the app log has a warning and the audit log a
  `security.alert` (`data-folder-not-private`). The image store's files are written by the toolkit
  with the default mode and are protected by the 0700 folder above them (a store moved elsewhere
  with `--store` is not).
- **At rest (review decision 3, option A).** The app does not encrypt what it stores. Protection of
  images, annotations, logs and the audit trail on a lost or stolen disk is full-disk encryption:
  BitLocker (Windows), FileVault (macOS), LUKS (Linux). Turn it on. App-level encryption is an
  open owner decision and is not implemented.
- **Audit log (R-126).** `audit/audit.jsonl`: one JSON object per line with `seq`, `ts`, `event`,
  `user`, `outcome`, optional `reason`, `data`, `prev` (the `mac` of the line before) and `mac` =
  HMAC-SHA256 of the line without `mac`, with a 256-bit key that is not in the log. Key: Windows,
  `audit/audit.key.dpapi` wrapped with CryptProtectData for the user; macOS, a generic password in
  the login keychain (service `in.sthraas.siaara.viewer.audit`; if the keychain cannot be used, the
  key file below, with a warning); Linux, `audit/audit.key` (0600). The Linux form is the weaker
  one: it stops other users and unaware tools, but a program running as the same user can read the
  key and re-sign the log. `audit/audit.anchor` (0600, signed) holds the newest `seq` and `mac`:
  lines cut off the end, an emptied file or a missing anchor are detected by `AuditLog.Verify` and,
  at the next start, recorded as `security.alert` (`audit-truncated`, `audit-anchor-missing`,
  `audit-anchor-invalid`, `audit-damaged`); sequence numbers are never reused, so the gap stays
  visible. Not covered: someone who replaces log AND anchor with an older matching pair (that
  needs an anchor outside this computer), and on Linux someone with the key. A crash in the middle
  of a write leaves a torn fragment: the next start ends it with a line break and writes
  `audit.recovered` with the fragment's length and SHA-256, which `Verify` accepts (and only
  that). The file is rotated at 16 MB to `audit.<last seq, 12 digits>.jsonl`; the chain and the
  numbering continue across files and `Verify` reads them all. Opening reads the anchor and the
  last 64 KB only. A log in the earlier format (no `mac`) is kept as `audit.legacy-*.jsonl.old`
  and its SHA-256 is the first entry (`audit.migrated`) of the new trail; a log whose key is gone
  is kept as `audit.unverifiable-*.jsonl.old` with an alert.
- **What is audited (R-127).** `app.start` / `app.stop`; `session.launch`, `session.signout`;
  `security.alert` with `kind` = `launch-reused`, `launch-expired`, `launch-revoked`,
  `launch-invalid`, `peer-other-user`, `peer-unknown`, `host-refused`, `cookie-refused`,
  `origin-refused`, `data-folder-not-private`, the `audit-*` kinds above, `alerts-suppressed`
  (refusals are limited to 10 entries of a kind per minute; the rest are counted); `import`;
  `study.open`; `study.delete` and `patient.delete` (first with outcome `intent`, then `success` or
  `failure`); `image.export`; `annotation.change`; `calibration.change`; `settings.change` (names
  of the changed settings). A delete is refused, with nothing deleted, when its intent cannot be
  written to the audit log. Other entries are best-effort: a write failure is logged in the app log
  and does not undo an action that has already happened.
- **No paths in the audit log (R-142).** Folders (import root, store folder) are written as
  `p:<32 hex>/d<depth>`: the first 128 bits of HMAC-SHA256(audit key, "path:" + full path) and the
  number of path components. Whoever holds the key can test a suspected folder against an entry;
  nobody else can guess names from it. (Chosen over "last component hashed": no component of the
  path is readable at all.)
- **Annotations (R-144).** `annotations/<user>/<SHA-256 of the SOP Instance UID>/<frame>.json`.
  Files of the earlier layout (`<user>/<UID>/...`) are still read, never for dot-only names, and
  the next save writes the new place.

## Test

```
dotnet run --project app/tests/Siaara.Viewer.Tests -c Release                 # all cases (perf uses 500 x 512x512)
dotnet run --project app/tests/Siaara.Viewer.Tests -c Release -- --quick       # perf on 100 x 256x256
dotnet run --project app/tests/Siaara.Viewer.Tests -c Release -- store import  # cases whose name starts so
```

70 cases: interop (ABI, UTF-8 names), display formatting, settings, audit chain, PHI redactor,
layouts, cine timing, scheduler, no-PackageReference check, store (paging, NO_TOTAL, date ranges,
wildcards, live updates, delete by patient key), import (mixed folder, DICOMDIR with a missing file,
cancel < 200 ms, duplicates, index in place), series (uneven spacing, stacks, multi-frame, lossy),
rendering (file / explicit / full-range windows and invert byte-equal to the toolkit, overlays),
measurements (mm, px, calibration, angle, undo / redo, ROI labels, SUV with and without weight),
the SUV / ROI golden file (64 PET data sets and 235 ROI cases computed by the toolkit's former
`siaara_suv_factor` / `siaara_roi_stats_compute`; the app's code matches them exactly), M1.1 viewer
features and the m1-review robustness fixes, transform vs `siaara_frame_map_point`, protocol (codec, flow control, back-pressure with a
slow client, cine at its own rate with skipping), security against the real Kestrel host (loopback,
launch / cookie, Host header, Origin on POST and WebSocket, unknown session), and an end-to-end
`ClientWebSocket` session on `/ws/view` (frame size and format, state, no frame without an ack,
W/L drag, command) with the audit trail and a no-PHI check of the logs. Security review round 4
(`AuditTests.cs`, `SecurityTests.cs`): signed audit lines and a forged chain, cut-off end and
anchor, torn write and `audit.recovered`, rotation, the earlier format, intent before delete and a
refused delete, settings changes; launch page (owner-only, no secret on the command line, deleted
after use / at expiry / at exit), peer-user parsing for the three OSes and the real check of the
running OS, a host that refuses another user's connection, alerts and their rate limit, host name
and cookie, CSP, file modes and the folder warning, annotation folder names, per-run ids and
session / connection limits, socket limits and malformed commands, sign out. Performance is
reported, not failing. Tests use only files the toolkit generates. The test client names the
app's `*.localhost` host and connects to 127.0.0.1 itself, as a browser's resolver would.

Against a running host:

```
dotnet .../siaara-viewer.dll --data-dir /tmp/sv --import SAMPLES/folder --no-browser --print-url --print-launch-file --no-idle-exit &
python3 app/tools/smoke.py LAUNCH_URL STUDY_UID                      # the printed URL (single use); signs out at the end
python3 app/tools/browser_check.py /usr/bin/google-chrome LAUNCH_FILE STUDY_UID /tmp/shots   # the printed launch page path
```

## How it works (short)

- Protocol `/ws/view` (binary, little endian, one socket per viewer session, separate from the Blazor
  circuit): frame `u8 1, u8 fmt, u16 vp, u32 seq, u16 w, u16 h, pixels`; state `u8 2, u16 vp, u32 seq,
  JSON`; ack `u8 3, u16 vp, u32 seq`; input `u8 4, ...` (22 bytes); command `u8 5, u16 vp, JSON`.
  At most one frame in flight per viewport (`FrameFlow`); input meanwhile only marks the viewport
  dirty and the newest state is rendered after the ack. Inbound messages over 64 KB close the socket
  (the browser sends only acks, input and small commands); JSON members of the wrong kind are
  ignored. The send queue holds at most 128 messages: a browser that stops reading is disconnected
  and gets every viewport afresh when it reconnects (R-146).
- Threads: Kestrel and Blazor threads never call the toolkit for slow work. Every toolkit call runs
  on `ToolkitScheduler` (Interactive cores/2 above normal, Decode cores/2 below normal with a
  background queue for prefetch, Import 2 = one scan producer + one store writer, StoreRead cores/4).
- Rendering: `DicomFrame` decoded once (`RenderCache`, LRU by bytes), rendered per viewport state.
  Gray images go as gray8 (one byte per pixel); the browser unpacks gray8 to RGBA for
  `putImageData` (a format unpack, not image processing).
- Measurements: geometry in image pixels, numbers on the server, drawn by the browser from the
  vectors in the state message. ROI statistics and SUV are the app's own code (M1.2):
  `Measure/Roi` (shapes, centre test and exact covered area of edge pixels, weighted statistics over
  the frame's float32 modality values from `siaara_frame_decode`) and `Measure/Suv` (inputs read with
  the generic getters and their source attributes recorded; QIBA scan reference time rules for
  START / ADMIN / NONE decay correction, Corrected Image DECY / ATTN checks, series vs acquisition
  time, injection date-time; LBM James and Janmahasatian, BSA Du Bois, IBW Devine; units BQML, CNTS
  with the Philips private Activity Concentration Scale Factor (7053,xx09) "Philips PET Private Group"
  (flagged), GML; the same 23 warnings as the toolkit had). `tests/.../Golden/suv-roi-golden.json`
  holds the toolkit's results on the synthetic cases of `SuvRoiCases.cs`; the C# results equal them.
  Stored as JSON per user / SOP / frame, written atomically.
- Data integrity: stored values are never changed. Display formatting ('^' in names, dates, ages)
  makes new strings for the screen only.
- Security: see "Launch and session" and "Files, permissions and the audit log" above. App logs pass
  a PHI redactor and never contain dataset values; toolkit messages flagged as PHI are dropped.
  Blazor's data-protection keys are kept in memory only. The app refuses to start when crash
  dumps are enabled through `DOTNET_DbgEnableMiniDump`.

## Robustness (m1-review)

- Render cache: frames and full file handles are handed out as ref-counted leases
  (`FrameLease`, `FileLease`); eviction drops the cache's reference, and the object is disposed when
  the last lease ends. A finished decode takes one reference per waiting caller in the same locked
  step as its insert, so nothing can release a frame between its decode and its use.
- Background work is observed (`ViewerSession.Observe`): failures are logged (type and toolkit
  status only) and shown (status bar, or the viewport when its picture depends on it). Loading always
  ends in images or a message; one unreadable series does not stop the others. A process-wide handler
  logs any task failure nobody observed.
- Header handles are released when a session closes; deleting a study or patient first closes its
  viewer sessions and releases cached handles and thumbnails, so no file stays open.
- Audit entries carry `outcome` (success / failure / cancelled) and, for failures, a plain-word
  `reason`; failed deletes are audited.

## M1.1 (review P0)

- Intended-use label "Not for primary diagnosis" (`Core/Text/Disclaimer.cs`): patient bar, About box
  (study browser and viewer), and a band on every exported or copied image (with "images and
  calculated values" when measurements are on it). Exports and copies are audited (`image.export`).
- Magnifier (M or the Magnify tool, left button held): a 200 x 200 CSS px glass rendered by the
  toolkit (`siaara_frame_render`, small viewport, 3x the viewport's zoom, same window / colour map /
  rotation / flips), sent as frames for viewport index + 0x4000 with its own flow control.
- Colour maps for grayscale images (`siaara_view2d_options.colormap`: Gray, Inverse gray, Hot Iron,
  PET, Hot Metal Blue, PET 20 Step) from the toolbar or the viewport menu; output switches to RGBA.
- Right-click (without a drag; a drag stays W/L) opens the viewport menu: window presets, colour map,
  overlays and shutters, sync group, linked scrolling, reference lines, copy image, export image (PNG),
  tag browser (placeholder for M2).
- Reference lines, cross-reference (X) and linked scrolling by Frame of Reference: planes from
  `siaara_frame_geometry`, the maths in `Core/Geometry/ImagePlane.cs` (plane intersection clipped to
  both images; nearest slice along each plane's normal). Linked scrolling moves roughly parallel stacks
  (|cos| of the normals >= 0.9); cross-reference moves every stack that passes within 10 mm.
- Sync group: chosen viewports (toolbar "Sync VP", menu, "Sync all") share W/L, zoom and pan; each of
  the three can be switched off.
- Arrow and Text annotation tools (Text: T; the label text is stored exactly as typed; an empty
  answer removes it).

## Still missing / not verified

- Security round 4, not run on the platform (compiled everywhere, pure parts unit-tested here on
  Linux): the Windows peer-user check (`GetExtendedTcpTable`, token SID), the macOS peer-user check
  (`libproc`; the `socket_fdinfo` offsets are taken from the SDK headers, not from a run), the
  Windows DPAPI key wrap, the macOS keychain item, the owner-only ACLs on Windows. The test runner
  exercises each of them on its own OS (the peer check on a real loopback connection, the key
  store round trip, the file checks), so the app test job on Windows and macOS is their first
  real run; if the macOS or Windows peer check is wrong the app refuses every connection (fail
  closed) and `peer-unknown` alerts fill the audit log. Not checked in a browser: Firefox, Safari,
  Edge with the `*.localhost` name; a default browser opening the local launch page (only headless
  Chrome was driven).

- Checked here with headless Chrome for Testing on Linux (study browser, opening a study by
  double-click, image drawn, right-drag W/L, wheel, length measurement, 2x2 layout; see
  `tools/browser_check.py`). Not checked: a headed browser window on a real display (look and feel,
  HiDPI, `--app` window behaviour), Edge, Firefox, Safari, Windows and macOS (CI should run the test
  runner there).
- Not exercised in a browser: drag and drop of a series onto a viewport, the calibration prompt,
  polygon / freehand drawing, keyboard shortcuts other than L, F8 in the patient bar, the folder picker.
- M1 gaps: no study / series level progress bar while headers load (the strip says "Reading
  headers"); theme change applies at the next page load; the patient bar hides in privacy mode but
  the Blazor measurement panel shows only labels (hidden in privacy mode); `True size` assumes 96
  CSS px per inch times the user calibration.
- Later milestones by design: GPU backend (M4), LAN mode with JPEG / PNG frames (M3), presentation
  states, native web-view shells (decision A1 option b).
