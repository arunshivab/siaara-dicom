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
                              Measure, Text, Settings, Audit, Logging, Samples
  src/Siaara.Viewer.Web/      Razor class library: Pages (Browser, Viewer), Components, Streaming (/ws/view),
                              Security (launch secret, cookie, Origin), wwwroot/js/siaara-view.js, wwwroot/css/siaara.css
  src/Siaara.Viewer.Host/     siaara-viewer: ABI check, single instance, store, Kestrel 127.0.0.1:0, BrowserLauncher (A1 a)
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
                       $XDG_DATA_HOME/siaara): settings.json, store/, logs/, audit/, annotations/
  --store DIR          image store (default: settings, then DIR/store)
  --import DIR         import a folder or a DICOMDIR at start (same path as the Import sheet)
  --no-browser         do not open a window
  --print-url          print the launch URL (it holds the launch secret)
  --no-idle-exit       keep running when no window is connected (default: exit after 60 s)
  --verbose            echo the (redacted) app log to stderr
siaara-viewer --generate-samples DIR [--full]
                       synthetic CT (60 slices, or 500 x 512x512 with --full), US clip, PET, CR, and a DICOMDIR set
```

Start-up: ABI check ("toolkit version mismatch" and exit), single-instance lock (a second start
opens the running instance's window and exits), store open (create, durable, rebuild if corrupt),
Kestrel on 127.0.0.1 at a random port, a random 256-bit launch secret in the opening URL that the
browser swaps for an HttpOnly SameSite=Strict cookie, then the window: Edge / Chrome / Chromium with
`--app=URL` when found, else the default browser (`BrowserLauncher`, the only class that knows).

## Test

```
dotnet run --project app/tests/Siaara.Viewer.Tests -c Release                 # all cases (perf uses 500 x 512x512)
dotnet run --project app/tests/Siaara.Viewer.Tests -c Release -- --quick       # perf on 100 x 256x256
dotnet run --project app/tests/Siaara.Viewer.Tests -c Release -- store import  # cases whose name starts so
```

37 cases: interop (ABI, UTF-8 names), display formatting, settings, audit chain, PHI redactor,
layouts, cine timing, scheduler, no-PackageReference check, store (paging, NO_TOTAL, date ranges,
wildcards, live updates, delete by patient key), import (mixed folder, DICOMDIR with a missing file,
cancel < 200 ms, duplicates, index in place), series (uneven spacing, stacks, multi-frame, lossy),
rendering (file / explicit / full-range windows and invert byte-equal to the toolkit, overlays),
measurements (mm, px, calibration, angle, undo / redo, ROI vs direct call, SUV with and without
weight), transform vs `siaara_frame_map_point`, protocol (codec, flow control, back-pressure with a
slow client, cine at its own rate with skipping), security against the real Kestrel host (loopback,
launch / cookie, Host header, Origin on POST and WebSocket, unknown session), and an end-to-end
`ClientWebSocket` session on `/ws/view` (frame size and format, state, no frame without an ack,
W/L drag, command) with the audit trail and a no-PHI check of the logs. Performance is reported, not
failing. Tests use only files the toolkit generates.

Against a running host:

```
dotnet .../siaara-viewer.dll --data-dir /tmp/sv --import SAMPLES/folder --no-browser --print-url --no-idle-exit &
python3 app/tools/smoke.py LAUNCH_URL STUDY_UID
python3 app/tools/browser_check.py /usr/bin/google-chrome LAUNCH_URL STUDY_UID /tmp/shots
```

## How it works (short)

- Protocol `/ws/view` (binary, little endian, one socket per viewer session, separate from the Blazor
  circuit): frame `u8 1, u8 fmt, u16 vp, u32 seq, u16 w, u16 h, pixels`; state `u8 2, u16 vp, u32 seq,
  JSON`; ack `u8 3, u16 vp, u32 seq`; input `u8 4, ...` (22 bytes); command `u8 5, u16 vp, JSON`.
  At most one frame in flight per viewport (`FrameFlow`); input meanwhile only marks the viewport
  dirty and the newest state is rendered after the ack. Messages over 64 MB close the socket.
- Threads: Kestrel and Blazor threads never call the toolkit for slow work. Every toolkit call runs
  on `ToolkitScheduler` (Interactive cores/2 above normal, Decode cores/2 below normal with a
  background queue for prefetch, Import 2 = one scan producer + one store writer, StoreRead cores/4).
- Rendering: `DicomFrame` decoded once (`RenderCache`, LRU by bytes), rendered per viewport state.
  Gray images go as gray8 (one byte per pixel); the browser unpacks gray8 to RGBA for
  `putImageData` (a format unpack, not image processing).
- Measurements: geometry in image pixels, numbers on the server (spacing, `roi_stats_compute` with
  partial weighting, `suv_factor`), drawn by the browser from the vectors in the state message.
  Stored as JSON per user / SOP / frame, written atomically.
- Data integrity: stored values are never changed. Display formatting ('^' in names, dates, ages)
  makes new strings for the screen only.
- Security: loopback only; Host header must be the loopback listener; every request needs the
  session cookie; WebSocket upgrades and non-GET requests need the app's exact Origin. App logs pass
  a PHI redactor and never contain dataset values; toolkit messages flagged as PHI are dropped. Audit
  log: append-only JSON lines, each with the SHA-256 of the previous line, patient key from the
  store. Blazor's data-protection keys are kept in memory only. The app refuses to start when crash
  dumps are enabled through `DOTNET_DbgEnableMiniDump`.

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
