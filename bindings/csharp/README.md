# SIAARA C# bindings

.NET 10 bindings over the stable C API (`include/siaara/siaara.h`). No NuGet dependencies: only the
.NET SDK. AOT-compatible and trimmable.

## Layout

| Path | What |
|---|---|
| `src/Siaara.Interop` | Internal. `Generated/Native.g.cs` (constants, blittable structs, `[LibraryImport]` declarations in `Raw`, safe span/ref overloads in `Native`), SafeHandles, the native library resolver, `[UnmanagedCallersOnly]` callback trampolines. The only project with `AllowUnsafeBlocks`. Packed into the Siaara package. |
| `src/Siaara` | Public API: `DicomFile`, `DicomDataset`, `DicomTag`/`DicomTags`, `DicomDictionary`, frames and rendering, `Transcoder`, `DicomStore`, `FolderScanner`, `Dicomdir`, `Deidentifier`, `DicomIndex`, `Siaara.Web.DicomWebClient` + `TlsOptions`, `Siaara.Clinical` (geometry, series sort, SUV, ROI, SR, SEG, RTSTRUCT), `Siaara.Volumes` (`Volume`, `VolumeView`, `GpuDevices`), `DicomFrame` + `ViewportOptions` (2D viewing), `PresentationState`, `ImageEncoder` (JPEG / PNG), `Siaara.Network` (`Dimse`, `StorageScp`, `SiaaraLog`). |
| `tests/Siaara.Tests` | Console test runner (no test framework package), same CHECK/REQUIRE style as `tests/test.hpp`. |
| `tests/native/struct_layout.c` | Generated; built by the CMake target `siaara_csharp_struct_layout`. |
| `samples/Siaara.AotSample` | NativeAOT / trimming proof. |

## Regenerating the interop

```
python3 tools/gen_csharp_interop.py          # after any change to siaara.h or src/dict
python3 tools/gen_csharp_interop.py --check  # CI, ctest (csharp_interop_up_to_date) and the C# runner
```

The generator parses `siaara.h` (functions, structs, enums, macros, callback typedefs). A new
function whose parameter pattern the safe-overload rules do not know still gets its exact `Raw`
declaration; the safe overload is left out with a warning on stderr.

## Build and test

```
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j1 --target siaara siaara_csharp_struct_layout
dotnet run -c Release --project bindings/csharp/tests/Siaara.Tests -- --layout-exe build-rel/tests/siaara_csharp_struct_layout
dotnet publish bindings/csharp/samples/Siaara.AotSample -c Release -r linux-x64 -p:PublishAot=true
dotnet pack bindings/csharp/src/Siaara -c Release      # -> bindings/csharp/artifacts/packages
```

MSBuild properties: `SiaaraNativeBuildDir` (default `<repo>/build-rel`), `SiaaraNativeLibrary`
(explicit file), `SiaaraNativeArtifactsDir` (pack: `<dir>/<rid>/<library>` for linux-x64,
linux-arm64, win-x64, win-x86, win-arm64, osx-x64, osx-arm64). linux-x86 is not a .NET runtime
identifier (.NET does not support 32-bit x86 Linux), so it has no package folder.

## Native library loading

`NativeLibrary.SetDllImportResolver` loads, by absolute path, `AppContext.BaseDirectory/runtimes/<rid>/native/<lib>`
and then `AppContext.BaseDirectory/<lib>` (a RID-specific publish flattens the runtimes folder). The
current directory and PATH are never searched; anything else fails with `DllNotFoundException`
listing the paths tried. musl (Alpine) is not detected: it would look in `linux-<arch>`.

## Threads, async and callbacks

* A `DicomFile` may be read from many threads; changes need exclusive access (as in the C API).
  Borrowed data sets keep the file alive per call and throw `ObjectDisposedException` after
  `Dispose`. Sequence items are located again on every call, so they never dangle.
* `Task.Run` is used only where the C call blocks for long: `DicomWebClient.*Async` (network),
  `Siaara.Network.Dimse.*Async` (DIMSE), `DicomStore.QueryAsync/VerifyAsync/RebuildAsync`,
  `FolderScanner.ScanAsync`, `Dicomdir.WriteAsync`, `Volume.BuildAsync`, `VolumeView.RenderRgba8Async`.
  The C API has no asynchronous I/O, so these occupy a pool thread. The token is passed to the C
  call as a `siaara_cancel` token: cancelling aborts a running DIMSE association or HTTP request
  and stops volume builds, store verify / rebuild and DICOMDIR writes at the next file
  (`OperationCanceledException`). Only a TCP connect in progress waits for its timeout.
  Long calls take an `IProgress<OperationProgress>` / `IProgress<DimseResult>`; reports arrive on
  the native calling thread.
* `DicomFrame` decodes a frame once; rendering (window, colour map, zoom, pan, rotation) is
  immutable and thread safe. `StorageScp` raises `Received` on its association threads, one at a
  time. `SiaaraLog.SetHandler` receives the library's diagnostics with a PHI flag per message.
* `DicomWebClient` and `VolumeView` serialise their calls with a lock (the C objects are single-threaded).
* Callbacks (store events, folder scan, DICOMDIR read) go through `[UnmanagedCallersOnly]`
  trampolines with a `GCHandle` as user data. Exceptions never unwind into native code: they are
  caught, the scan stops, and the exception is rethrown from the .NET method that made the call.
  `DicomStore.Changed` is raised on the thread that made the change; in Blazor use `InvokeAsync`.
