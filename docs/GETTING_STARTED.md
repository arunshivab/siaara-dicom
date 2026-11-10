# Getting started with SIAARA

This guide is for a developer who has a SIAARA release and wants to call it from C, C++ or C#.
Every program shown here is in `examples/` and is built and run by the toolkit's test suite
(`ctest -R example`), so it compiles and works with the release it ships in.

## 1. What a release contains

One archive per platform, `siaara-<version>-<platform>.tar.gz` (`.zip` on Windows):

| Path | Content |
|---|---|
| `include/siaara/siaara.h` | The C API: one header, C11 and C++ |
| `lib/libsiaara.so`, `lib/libsiaara.dylib`, `bin/siaara.dll` + `lib/siaara.lib` | The shared library. It exports only the C API |
| `lib/libsiaara_static.a`, `lib/siaara_static.lib` | The static library (define `SIAARA_STATIC` when compiling against it; the CMake target does) |
| `lib/cmake/siaara/` | CMake package: `find_package(siaara)` gives `siaara::siaara` and `siaara::siaara_static` |
| `lib/pkgconfig/siaara.pc` | pkg-config file (Linux, macOS) |
| `bin/siaara-*` | Command-line tools (`--help` on each) |
| `docs/`, `examples/` | This documentation and the example sources |
| `LICENSE`, `README.md`, `CHANGELOG.md`, `SECURITY.md` | |
| `siaara.spdx.json` | SBOM (SPDX 2.3) of the package |

and, for .NET, one package for all platforms: `Siaara.<version>.nupkg`.

The release binaries link the C/C++ runtime statically: on Linux they need only glibc, on
Windows no Visual C++ redistributable. On Windows an application that links the *static*
library must itself use the static runtime (`/MT`). TLS uses the operating system's stack; on
Linux that is the system's OpenSSL 3 (`libssl.so.3`), loaded when TLS is first used - without it
everything works except TLS (`siaara_tls_available()` tells).

Check what you downloaded: `sha256sum -c SHA256SUMS`.

## 2. C and C++ with CMake

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES C CXX)
find_package(siaara REQUIRED CONFIG)          # -DCMAKE_PREFIX_PATH=<the unpacked archive>
add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE siaara::siaara)          # or siaara::siaara_static
```

The examples build the same way against an unpacked release:

    cmake -S examples -B build-examples -DCMAKE_PREFIX_PATH=/path/to/siaara-<version>-<platform>
    cmake --build build-examples
    LD_LIBRARY_PATH=/path/to/siaara-<version>-<platform>/lib build-examples/siaara_example_read_file

With pkg-config (Linux, macOS):

    export PKG_CONFIG_PATH=/path/to/siaara-<version>-<platform>/lib/pkgconfig
    cc -std=c11 examples/c/read_file.c $(pkg-config --cflags --libs siaara) -o read_file

At run time the shared library must be found: next to the program on Windows, on the library
path (or an rpath) on Linux and macOS.

## 3. The rules of the C API in one minute

- Every function returns `siaara_status` (`SIAARA_OK` = 0). `siaara_last_error_message()` returns
  the text of the last error on the calling thread.
- Handles (`siaara_file*`, `siaara_store*`, `siaara_scp*` ...) are created by a function that
  writes them to an out parameter and are freed by the matching `_free` function.
- Structures start with `struct_size`: set it to `sizeof` the structure (after zeroing it). This
  is how the library stays compatible when a structure grows.
- Strings are UTF-8. Text output goes to a caller buffer; when it is too small the function
  returns `SIAARA_ERR_BUFFER_TOO_SMALL` and `*needed` says how much is needed.
- `siaara_abi_version()` must equal `SIAARA_ABI_VERSION` of the header you compiled with.
- Reading may happen from many threads; changing a file or data set needs exclusive access.
  Each function's comment in the header states its thread safety.

More in [capi-notes.md](capi-notes.md).

## 4. Read a file: by keyword, by path, a pixel value

`examples/c/read_file.c` (run it without arguments: it writes a small sample file first).

```c
if (siaara_file_open(path, strlen(path), NULL, &file) != SIAARA_OK) return fail("open");
if (siaara_file_dataset(file, &ds) != SIAARA_OK) return fail("dataset");

/* Patient's Name by keyword: the dictionary gives the tag, the value comes as UTF-8. */
memset(&entry, 0, sizeof entry);
entry.struct_size = sizeof entry;
if (siaara_dict_lookup_keyword("PatientName", strlen("PatientName"), &entry) != SIAARA_OK) return fail("keyword");
if (siaara_dataset_get_utf8(ds, entry.tag, 0, text, sizeof text, &needed) != SIAARA_OK) return fail("PatientName");

/* The same by path; paths also reach into sequences: "BeamSequence[0].BeamName". */
memset(&value, 0, sizeof value);
value.struct_size = sizeof value;
if (siaara_get_by_path(ds, "PatientName", strlen("PatientName"), &value, text, sizeof text, &needed) != SIAARA_OK)
    return fail("path");

/* Stored value and modality value (rescale applied) at column 1, row 0 of frame 0. */
memset(&pixel, 0, sizeof pixel);
pixel.struct_size = sizeof pixel;
if (siaara_file_pixel_value(file, 0, 1, 0, 0, &pixel) != SIAARA_OK) return fail("pixel value");
```

Output:

    SIAARA <version> (ABI 1)
    PatientName (00100010) = Example^Patient
    by path: VR PN, VM 1, text Example^Patient
    pixel (1,0): stored 1074, modality value 50

Things to know:

- **Reading is lenient and tells you so.** A file with problems the reader could work around is
  opened; `siaara_file_warning_count` / `siaara_file_warning` list what was unusual, and
  `siaara_file_read_report` says whether the read was *complete*. A file that was not read
  completely (truncated, bytes skipped, recovered by a heuristic) is refused by
  `siaara_file_save` and by the sending functions unless you explicitly allow it, so damaged
  data is never passed on looking whole.
- One recovery heuristic deserves a mention: files from some GE systems carry element lengths
  that run past the end of their sequence item. For **local files** the reader recovers these (in
  a narrow, well-defined case) and reports it; for data **received from the network** this
  recovery is off.
- Paths, named modules, many pixel values at once, volumes: [DATA_ACCESS.md](DATA_ACCESS.md).
- The toolkit gives stored values, modality values and geometry. Calculations on top of them
  (SUV, ROI statistics) are intentionally left to your application.

The C++ version, `examples/cpp/read_file.cpp`, uses the same calls with a `unique_ptr` owning
the file handle.

## 5. Network: an SCP, C-ECHO and C-STORE, DICOMweb

`examples/c/network.c` (give it an empty work folder). It starts a Storage SCP over a local
store, sends C-ECHO and C-STORE to it, starts the DICOMweb server over the same store and asks
it with QIDO-RS. Everything stays on the loopback interface.

```c
/* A Storage SCP (Verification is always on) that files what it receives in a store. */
if (siaara_store_open(store_root, strlen(store_root), NULL, &store) != SIAARA_OK) return fail("store");
{
    const char* peers[] = {"EXAMPLE_SCU"};  /* the calling AE titles that may connect */
    siaara_scp_options options;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.port = 0;                       /* 0: an ephemeral port; a real archive names its port */
    options.bind_address = "127.0.0.1";
    options.ae_title = "EXAMPLE_SCP";
    options.allowed_calling_aes = peers;
    options.allowed_count = 1;
    if (siaara_scp_start(&options, store, NULL, NULL, &scp) != SIAARA_OK) return fail("scp start");
    if (siaara_scp_port(scp, &dimse_port) != SIAARA_OK) return fail("scp port");
}
```

```c
/* C-ECHO, then C-STORE of a file. */
siaara_dimse_peer peer;
siaara_dimse_result result;
const char* files[] = {sample};
memset(&peer, 0, sizeof peer);
peer.struct_size = sizeof peer;
peer.host = "127.0.0.1";
peer.port = dimse_port;
peer.calling_ae = "EXAMPLE_SCU";
peer.called_ae = "EXAMPLE_SCP";
memset(&result, 0, sizeof result);
result.struct_size = sizeof result;
if (siaara_dimse_echo(&peer, NULL, &result) != SIAARA_OK) return fail("C-ECHO");
if (siaara_dimse_store_files(&peer, files, NULL, 1, NULL, NULL, NULL, &result) != SIAARA_OK) return fail("C-STORE");
```

```c
/* DICOMweb (QIDO-RS, WADO-RS, STOW-RS) over the same store, then a QIDO-RS query. */
siaara_web_server_options options;
memset(&options, 0, sizeof options);
options.struct_size = sizeof options;
options.port = 0;   /* ephemeral; plain HTTP is allowed on the loopback interface only */
if (siaara_web_server_start(&options, store, &web) != SIAARA_OK) return fail("web server");
if (siaara_web_server_port(web, &web_port) != SIAARA_OK) return fail("web port");

snprintf(url, sizeof url, "http://127.0.0.1:%u/dicomweb", (unsigned)web_port);
if (siaara_web_client_create(url, strlen(url), 0, &client) != SIAARA_OK) return fail("web client");
const char* query = "PatientID=EX-0001";
if (siaara_web_qido(client, SIAARA_QIDO_STUDIES, NULL, NULL, query, strlen(query), json, sizeof json, &needed) != SIAARA_OK)
    return fail("QIDO-RS");
```

Things to know:

- **An SCP accepts only the calling AE titles you list.** An empty list refuses to start unless
  you set `allow_any_calling_ae`. This is deliberate.
- **Incomplete data received is quarantined by default**: kept byte for byte outside the
  archive index, and the sender is answered with a warning. You choose the policy.
- **The DICOMweb server serves plain HTTP on loopback only.** On any other interface it needs
  TLS plus your authorization callback or client certificates.
- `siaara_scp_start_ex` adds Query/Retrieve from the store, Modality Worklist, MPPS, Storage
  Commitment and UPS on the same port. MPPS and UPS state lives in memory unless you give a
  `state_folder`.
- All of it, with every option and its default: [NETWORKING.md](NETWORKING.md). What to write in
  your product's Conformance Statement: [CONFORMANCE.md](CONFORMANCE.md).

## 6. C# (.NET 10)

The package `Siaara` holds the managed assemblies and the native library for win-x64, win-x86,
win-arm64, linux-x64, linux-arm64, osx-x64 and osx-arm64. It has no dependencies, so a folder
with the `.nupkg` is all the "feed" you need - nothing is fetched from nuget.org.

1. Put `Siaara.<version>.nupkg` in a folder, for example `packages/` next to your solution.
2. Name that folder as a package source (`examples/csharp/nuget.config`):

   ```xml
   <configuration>
     <packageSources>
       <clear />
       <add key="siaara-local" value="./packages" />
     </packageSources>
   </configuration>
   ```

3. Reference the package:

   ```xml
   <ItemGroup>
     <PackageReference Include="Siaara" Version="x.y.z" />
   </ItemGroup>
   ```

`examples/csharp/QuickStart` does everything the C examples do:

    cd examples/csharp
    dotnet run --project QuickStart -p:SiaaraPackageVersion=<version> -- /tmp/siaara-quickstart

```csharp
using (DicomFile file = DicomFile.Open(sample))
{
    DicomDataset ds = file.Dataset;
    Console.WriteLine($"PatientName = {ds.GetString(DicomTag.Parse("PatientName"))}");
    Console.WriteLine($"PatientID   = {ds.GetString(DicomTags.PatientID)}");
    DicomPathValue? byPath = ds.GetByPath("PatientName");
    Console.WriteLine($"by path: VR {byPath?.VR}, text {byPath?.Text}");
    PixelValue pixel = file.GetPixelValue(frame: 0, column: 1, row: 0);
    Console.WriteLine($"pixel (1,0): stored {pixel.Stored}, modality value {pixel.ModalityValue}");
}
```

```csharp
using DicomStore store = DicomStore.Open(Path.Combine(work, "store"));
using StorageScp scp = StorageScp.Start(
    new StorageScpOptions
    {
        Port = 0,                                   // ephemeral; a real archive names its port
        BindAddress = "127.0.0.1",
        AeTitle = "EXAMPLE_SCP",
        AllowedCallingAeTitles = new[] { "EXAMPLE_SCU" },
    },
    store);

var peer = new DimsePeer
{
    Host = "127.0.0.1",
    Port = scp.Port,
    CallingAeTitle = "EXAMPLE_SCU",
    CalledAeTitle = "EXAMPLE_SCP",
};
DimseResult echo = Dimse.Echo(peer);
DimseResult stored = Dimse.StoreFiles(peer, new[] { sample });

using DicomWebServer web = DicomWebServer.Start(new DicomWebServerOptions { Port = 0 }, store);
using var client = new DicomWebClient($"http://127.0.0.1:{web.Port}/dicomweb");
string json = client.Search(QidoLevel.Studies, "PatientID=EX-0001");
```

Things to know:

- Errors are `SiaaraException` with the error code and the message of the C API.
- Objects that own native resources are `IDisposable`; dispose them (`using`).
- Every blocking network call has an `...Async` form that takes a `CancellationToken`.
- The assemblies are trimmable and NativeAOT compatible. The native library is loaded by
  absolute path from `runtimes/<rid>/native` under the application folder (or the application
  folder itself for a RID-specific publish); `PATH` and the current directory are never searched.

## 7. The command-line tools

    siaara-dump image.dcm                        # elements, with private names from the dictionary
    siaara-echoscu -aec ARCHIVE host 104         # C-ECHO
    siaara-deid in.dcm out-folder                # de-identification (PS3.15 Annex E)
    siaara-repair validate in.dcm                # validation; "report" and "fix" repair with a report

and `siaara-storescu`, `siaara-storescp`, `siaara-findscu`, `siaara-movescu`, `siaara-getscu`,
`siaara-webcli` (QIDO-RS / WADO-RS / STOW-RS from the shell).

Each prints its options with `--help`.
