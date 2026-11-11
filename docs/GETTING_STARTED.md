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
- **How a file is brought into memory** is an option (`siaara_read_options.open_mode`, C#
  `DicomReadOptions.OpenMode`). The default, `SIAARA_OPEN_AUTO`, reads files up to 256 MiB
  (`open_read_threshold`) into the library's own memory and memory-maps larger ones.
  `SIAARA_OPEN_READ` always reads (and refuses a file above `open_read_max_bytes`, default
  4 GiB); `SIAARA_OPEN_MAP` always maps, which costs no memory but is only safe for files nobody
  rewrites in place: if another process shortens a mapped file - think of a shared or network
  folder - touching the missing part ends the process. The library re-checks a mapped file's
  size after mapping and before it decodes pixel data and returns `SIAARA_ERR_TRUNCATED` when it
  changed, but that cannot close the gap completely. Values, the read report and unchanged saves
  are identical in all three modes.
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
win-arm64, linux-x64, linux-arm64, osx-x64 and osx-arm64. It has no dependencies. There are two
ways to get it; the package is the same file either way.

**From the GitHub Packages feed** (how the owner's products get it). Every published release
is on the feed of the repository's owner. `nuget.config` next to your solution:

```xml
<configuration>
  <packageSources>
    <add key="siaara" value="https://nuget.pkg.github.com/arunshivab/index.json" />
  </packageSources>
  <packageSourceMapping>
    <packageSource key="siaara"><package pattern="Siaara" /></packageSource>
    <packageSource key="nuget.org"><package pattern="*" /></packageSource>
  </packageSourceMapping>
</configuration>
```

The feed asks for a GitHub account that may read the package and a personal access token with
the `read:packages` scope. Keep the token out of the repository: give it once per machine

    dotnet nuget update source siaara --username <github user> --password <token> --store-password-in-clear-text --configfile <your user-level NuGet.Config>

or, in a build pipeline, through the environment (`NuGetPackageSourceCredentials_siaara` =
`Username=<github user>;Password=<token>`). The source mapping makes sure `Siaara` can only
come from this feed, and nothing else can come from it. Then reference the package (step 3).

**From a folder** (no account, no network): a folder with the `.nupkg` is all the "feed" you
need - nothing is fetched from nuget.org.

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

## 7. Showing a DICOM attachment safely (untrusted input)

A DICOM file that arrives from outside - a mail attachment, an upload - is **untrusted bytes**.
To show it (a caption and a small picture) without letting it take the server's memory,
open it from memory with every limit set explicitly, decode a fixed number of frames, and render
at a fixed size.

> **Run the preview in a separate process, with a kill timer and a memory limit.** The limits
> below bound the memory and the amount of work. They do **not** bound the time: decoding a frame
> has no deadline and cannot be cancelled (a progressive JPEG of 8456 x 7936 with 883 legal
> scans took 12.9 s for one frame in the security review), and a thread that is still decoding
> cannot be abandoned - it keeps its memory and its core until it returns. So the worker that
> calls the preview must be a process of its own that the caller kills when it has not answered
> in time (a few seconds), and that runs under an operating-system memory limit (a job object on
> Windows, a cgroup or `ulimit -v` on Linux) a little above the limits you configure. One
> attachment per call; a killed worker costs nothing but that one preview.

**In C# this is one call of the package** - there is nothing to copy into your product:

```csharp
using Siaara;

DicomPreview p = DicomAttachmentPreview.Create(attachmentBytes);          // recommended limits
// or: DicomAttachmentPreview.Create(attachmentBytes, DicomPreviewOptions.Recommended with { MaxBox = 256 });

switch (p.Outcome)
{
    case DicomPreviewOutcome.Ok:                       // p.Image: frame 0; p.Strip: a few frames of a multi-frame file
        byte[] png = p.Image!.ToPng();                 // or ToJpeg()
        break;
    case DicomPreviewOutcome.NotDicom:    break;       // a plain attachment
    default:                              break;       // NoPixelData, Unsupported, TooLarge, Damaged: p.Reason says why
}
// Caption: p.Modality, p.StudyDate, p.SeriesDescription, p.Rows, p.Columns, p.Frames, p.TransferSyntaxName.
// p.Incomplete: the file was cut off or damaged - say so next to the picture.
// Patient's Name and ID are NOT in the preview. On request only:
(string Name, string Id)? patient = DicomAttachmentPreview.ReadPatientFields(attachmentBytes);
```

`Create` never throws for bad input. `DicomPreviewOptions` carries every limit of the table
below plus how the picture is drawn (`MaxBox`, `MaxStripFrames`, `StripBox`,
`MaxCaptionLength`, `StripControlAndFormatCharacters`, `Interpolation`, `VoiMode`,
`Background`); its defaults are the recommended values.
`DicomPreviewOptions.Recommended.ToReadOptions()` is the toolkit's **preset for untrusted
input**: use it for your own `DicomFile.Open` of bytes you do not trust, too.
`examples/csharp/AttachmentPreview` shows the preview on five kinds of attachment. In C the same
is `examples/c/attachment_preview.h` (`preview_attachment`, `preview_default_limits`), a header
to compile into your program. Both are run by the test suite.

The caption fields are **text from the file**. The preview takes control characters and Unicode
format characters out of them (bidirectional overrides such as U+202E, which make
`gpj.exe` read as `exe.jpg`, zero-width characters, the byte order mark) and cuts them to
`MaxCaptionLength`; it does not escape them. Before a caption goes into a page:

```csharp
string html = "<figcaption>" + DicomAttachmentPreview.HtmlEscape(p.SeriesDescription) + "</figcaption>";
```

(`preview_html_escape` in the C example; `DicomAttachmentPreview.CaptionText` cleans any other
text from a file the same way.)

What the call does, step by step, with the public API (the C calls are in brackets):

```csharp
// 1. Not DICOM at all? A Part 10 file has "DICM" at offset 128. Nothing else is parsed.
if (data.Length < 132 || !data.Slice(128, 4).SequenceEqual("DICM"u8)) return NotDicom;

// 2. Open from memory with the limits  [siaara_file_open_memory + siaara_read_options]
using DicomFile file = DicomFile.Open(data, new DicomReadOptions
{
    AllowRawDataset = false,          // Part 10 only
    RecoverOverlongLengths = false,   // no guessing at damaged lengths
    MaxDepth = 16,
    MaxElements = 200_000,
    MaxInflatedSize = 128L << 20,
    MaxFrameBytes = 64L << 20,
    FrameMemoryBudget = 256L << 20,
    MaxScans = 64,
    MaxCropFactor = 1,
});                                   // = DicomPreviewOptions.Recommended.ToReadOptions()

// 3. Was the read complete? If not, show what there is and say so  [siaara_file_read_report]
bool incomplete = !file.ReadReport.IsComplete;

// 4. Caption: safe header fields only, and only values of a sane stored length - a longer one is
//    not decoded at all  [siaara_dataset_get_raw for the length, siaara_dataset_get_utf8]
string modality = file.Dataset.TryGetValueLength(DicomTags.Modality, out long stored) && stored <= 65_536
                  && file.Dataset.TryGetString(DicomTags.Modality, out string? m)
                      ? DicomAttachmentPreview.CaptionText(m, 256) : "";

// 5. Is there an image the toolkit can decode? Nothing is decoded yet  [siaara_file_pixel_info]
DicomPixelInfo pi = file.GetPixelInfo();   // NotFound: no image; Unsupported: video, JPEG XL ...
if ((long)pi.Rows * pi.Columns > 64_000_000) return TooLarge;   // pixels, not bytes: see the table

// 6. Frame 0, with the file's own window / palette / presentation defaults, fitted into 512 x 512
//    [siaara_frame_open + siaara_frame_render with viewport_width / viewport_height, zoom 0]
using DicomFrame frame = file.OpenFrame(0);
byte[] rgba = frame.Render(new ViewportOptions { Width = w, Height = h, Interpolation = Interpolation.Linear });

// 7. PNG and JPEG  [siaara_encode_png / siaara_encode_jpeg]
byte[] png = ImageEncoder.EncodePng(rgba, w, h);
byte[] jpeg = ImageEncoder.EncodeJpeg(rgba, w, h);
```

The recommended settings and what each protects against:

| Setting | Recommended | Protects against |
|---|---|---|
| attachment size (your own check, before anything else) | 64 MiB | everything below is proportional to the input at worst |
| the "DICM" check at offset 128, `AllowRawDataset = false` (`allow_raw_dataset = 0`) | on | parsing arbitrary bytes as a data set without a header |
| `RecoverOverlongLengths = false` (`SIAARA_RECOVER_OFF`) | off | a recovery heuristic meant for trusted local files reinterpreting hostile bytes |
| `MaxDepth` (`max_depth`) | 16 | deeply nested sequences |
| `MaxElements` (`max_elements`) | 200,000 | millions of tiny elements or empty sequence items: 8 bytes of input each become an object in memory |
| `MaxInflatedSize` (`max_inflated_size`) | 128 MiB | a deflate bomb: a few kB in a deflated transfer syntax that inflate to gigabytes |
| `MaxFrameBytes` (`max_frame_bytes`) | 64 MiB | huge Rows x Columns: a frame larger than this is refused before anything is decoded |
| `FrameMemoryBudget` (`frame_memory_budget`) | 256 MiB | a JPEG 2000 / JPEG-LS / JPEG stream that claims huge dimensions or many components; a render, which works at image resolution with 4 bytes per pixel before it is scaled down, is held to it too |
| `MaxPixels` (`max_pixels`, the preview's own check of Rows x Columns) | 64,000,000 | a small file that states a huge 1-bit or 8-bit image: a 1-bit frame of 64 MiB is 537 million pixels. Refused before anything is decoded |
| `MaxTotalPixels` (`max_total_pixels`, the preview's own count) | 128,000,000 | many large frames: the strip ends when the next frame would pass it |
| `MaxScans` (`max_scans`) | 64 | a progressive JPEG with hundreds of scans, each a pass over the whole frame (the library default is 1000) |
| `MaxCropFactor` (`max_crop_factor`) | 1 | a codestream larger than Rows x Columns that is decoded only to be cropped (the library default allows 2 x per axis, for files whose codestream is padded) |
| `MaxValueBytes` (`max_value_bytes`, the preview's own check) | 65,536 | a caption attribute with a value of many megabytes: its stored length is looked at first and it is not decoded |
| `StripControlAndFormatCharacters` (`strip_control_and_format`) | on | captions that reorder or hide text on screen (bidirectional overrides, zero-width characters), terminal escapes |
| frames decoded | frame 0, plus at most 8 for a strip | a file that claims a million frames: frames are decoded one at a time, only the ones you ask for |
| output size | 512 x 512 (strip tiles 128 x 128) | the size of what you produce and encode, whatever the file claims |
| patient identifiers | not read | personal data in a preview nobody asked for; `ReadPatientFields` / `preview_patient_fields` read them on request |

What you get back, and what to show:

| Outcome | When | Show |
|---|---|---|
| picture | the first frame could be decoded | the picture and the caption; "incomplete file" when the read was not complete |
| not DICOM | no "DICM" header | a plain attachment |
| no image | SR, RT Structure Set, encapsulated PDF, DICOMDIR and other objects without pixel data | the caption |
| cannot preview | pixel data the toolkit does not decode: MPEG-2 / MPEG-4 / HEVC video, JPEG XL - the reason names the transfer syntax | the caption and the reason |
| too large | a limit above was reached; there is no picture | the caption if there is one |
| damaged | DICOM, but the file or its pixel data cannot be read | the caption if there is one |

Things to know:

- The frame limits (`MaxFrameBytes`, `FrameMemoryBudget`, `MaxScans`, `MaxCropFactor`) belong to
  the **opened file**: every later decode, render and `OpenFrame` on that `DicomFile` is held to
  them. A file opened without them has the library defaults (4 GiB), which are meant for trusted
  data.
- **With the default budget of 256 MiB the largest picture that renders** is about 53 million
  pixels for 8-bit grayscale, 44 million for 16-bit and 26 million for 8-bit colour (the decoded
  frame plus 4 bytes per pixel, plus 3 more for colour); a larger image within `MaxPixels` ends
  as "too large" when it is rendered, after its frame was decoded. Raise `FrameMemoryBudget` if
  you need more, and the memory limit of the worker process with it.
- **There is no time limit in the API and a decode cannot be cancelled** - hence the separate
  process with a kill timer at the top of this section. The toolkit's test for this path
  (`tests/test_rc1_preview_alloc.cpp`: mutated and truncated files, decompression and deflate
  bombs, a million claimed frames, element and nesting bombs, the huge 1-bit and 8-bit frames)
  holds each preview to a memory budget; it cannot promise a time.
- **What the memory limit of the worker has to allow**: while a deflated file inflates, the
  bytes in use are at most twice `MaxInflatedSize`, but the memory the process holds is higher,
  because the freed growth steps of the buffer stay in the process heap: about three times the
  limit (measured on Linux with the recommended 128 MiB: about 380 MiB resident, both for a data
  set of 127 MiB and for a deflate bomb that is refused at the limit). After that comes
  `FrameMemoryBudget` for a frame. Give the worker about 1 GiB with the recommended settings
  (the C path alone peaked at 385 MiB; the .NET runtime needs room on top).
- A limit in `DicomPreviewOptions` must be greater than zero: `Create` throws
  `ArgumentOutOfRangeException` for a zeroed or negative limit instead of reading without it.
  `TransferSyntaxUid` in the result is digits and dots or empty, whatever the file says.
- The caption fields are text from the file. Escape them before they go into HTML
  (`DicomAttachmentPreview.HtmlEscape`).
- Colour, palette colour, signed and 12 / 16-bit images, MONOCHROME1 and the file's window are
  handled by the render call. Pixel spacing with non-square pixels is not corrected in the
  example.
- A cut-off file is flagged (`ReadReport`), never shown as if whole. Its picture is shown when
  the first frame is all there; uncompressed pixel data that ends early has no picture.

## 7a. Grouping a ZIP or a DICOMDIR (untrusted input)

A mail that carries a study usually carries it as a ZIP: a folder of DICOM files from a CD or a
PACS export, often with a DICOMDIR, sometimes inside a wrapping folder, in no particular order.
`DicomStudyGrouping` (C: `siaara_grouping_*`) turns those members into patient / study / series,
orders each series for scrolling, and sets what the DICOMDIR lists against what is in the ZIP -
all from bytes in memory. Nothing is unpacked to disk and no file's bytes are kept: each member
is read header-only while it is added.

> **The grouping shows what the files claim, not proof that they belong together** or to the
> patient they name. **Run it in the same separate worker process as the attachment preview**
> (section 7), with the same kill timer and memory limit: reading headers has no time bound.

**C#** (the work is in the package; `examples/csharp/StudyGrouping` is a complete program):

```csharp
using var grouping = new DicomStudyGrouping();           // DicomGroupingOptions.Recommended
using var zip = new ZipArchive(new MemoryStream(attachment), ZipArchiveMode.Read);
foreach (ZipArchiveEntry e in zip.Entries.Take(DicomGroupingOptions.DefaultMaxFiles))
{
    if (e.Length > DicomGroupingOptions.DefaultMaxFileBytes || e.FullName.EndsWith('/')) continue;
    byte[] data = ReadAll(e);
    if (Path.GetFileName(e.FullName).Equals("DICOMDIR", StringComparison.OrdinalIgnoreCase))
        grouping.AddDicomdir(data);                       // optional
    else
        grouping.Add(e.FullName, data);                   // never throws for bad bytes
}
DicomGroupingResult r = grouping.Result;
foreach (DicomGroupedSeries s in r.Series)                // each series in scrolling order
    foreach (DicomGroupedInstance i in s.Instances)
        Show(DicomAttachmentPreview.HtmlEscape(i.Name.Display), i.Position, i.SortKey);
foreach (DicomGroupingIssue issue in r.Issues)            // skipped, ungrouped, duplicates, conflicts,
    Report(issue.Kind, issue.Name.Display, issue.Subject.Display);  // listed-but-missing, unlisted
```

**C** (index-based getters; text by field, see `siaara.h`, "grouping"):

```c
siaara_grouping* g = NULL;
siaara_grouping_create(NULL, &g);                         /* NULL: every default */
siaara_grouping_add_file(g, name, name_len, data, size);  /* per ZIP member; SIAARA_OK for bad bytes too */
siaara_grouping_add_dicomdir(g, dicomdir, dicomdir_size); /* optional */
siaara_grouping_summary sum = {sizeof sum};
siaara_grouping_result(g, &sum);
for (uint32_t k = 0; k < sum.series; ++k) {
    siaara_grouped_series s = {sizeof s};
    siaara_grouping_series(g, k, &s);
    for (uint32_t i = s.first_instance; i < s.first_instance + s.instance_count; ++i) {
        siaara_grouping_text name = {sizeof name};
        siaara_grouping_text_get(g, SIAARA_GLEVEL_INSTANCE, i, SIAARA_GFIELD_NAME, &name);
        /* name.display: the display-safe copy; name.stored: the name exactly as given */
    }
}
siaara_grouping_free(g);                                  /* every pointer handed out ends here */
```

What it does:
- **Order within a series** (`Order`, `order`): by the position along the slice normal (the
  default) - Image Position (Patient) projected on row x column of Image Orientation (Patient)
  of the series' first file - so an oblique series and Instance Numbers that contradict the
  positions still scroll in space order. A file whose position is missing, not finite, or whose
  orientation differs from the series' by more than `OrientationTolerance` (default 1e-4 per
  direction cosine) comes after, by Slice Location, then by Instance Number, then as added; the
  instance's `SortKey` says which. By Instance Number or as added are options. Sorts are stable.
- **Text**: every value comes exactly as stored (decoded from its character set, padding
  removed) and next to it a display-safe copy: controls and line separators become a space,
  bidirectional overrides, zero-width and other format characters are removed, bytes that are
  not UTF-8 become U+FFFD. The ZIP member's name is returned exactly as given, with its own
  display-safe copy (a name like `scan<U+202E>gpj.exe` shows as `scangpj.exe`). The same cleaner
  is `siaara_text_display_safe` in C, and what `DicomAttachmentPreview.CaptionText` uses in C#.
  A value longer than its cap (UIDs 64 characters, other text 1,024 bytes) is not kept and is
  reported `TooLong`: never cut and passed on as if whole.
- **Grouping keys**: a UID groups only when it is a valid UID (1 to 64 digits and dots).
  Patients are told apart by Patient ID; Patient's Name and Patient ID are **not returned**
  unless `IncludePatientFields` is set. A Study UID under two Patient IDs gives two studies and
  an issue each; a series whose files disagree on Modality or Rows x Columns is reported.
- **Duplicates** (the same SOP Instance UID twice): both kept, the second marked (default), or
  only the first kept (`Duplicates = KeepFirst`); an issue names the second either way.
- **DICOMDIR against the ZIP**: the member's name is split on `/` and `\`, `.` components are
  dropped and ASCII letters compared in any case (`MatchCaseInsensitive`); a File ID of N
  components matches the name's last N components (`MatchByTrailingComponents`), so a wrapping
  folder does not matter. Listed-but-missing, present-but-unlisted and a File ID that two names
  match are issues. The DICOMDIR is read from bytes (`Dicomdir.Read(ReadOnlySpan<byte>, ...)`,
  `siaara_dicomdir_read_memory`): File IDs come as text and are never resolved to a path; an
  unsafe one (`..`, absolute, a drive letter, more than 8 components, characters outside
  PS3.10's set) is `Rejected`.

The limits (`DicomGroupingOptions`, `siaara_grouping_options`; C: 0 takes the default, C#: a zero
or negative limit throws `ArgumentOutOfRangeException`):

| Option | Default | Protects against |
|---|---|---|
| `MaxFiles` / `max_files` | 10,000 | a ZIP of millions of members |
| `MaxFileBytes` / `max_file_bytes` | 64 MiB | one huge member |
| `MaxTotalBytes` / `max_total_bytes` | 2 GiB | the sum of all members read |
| `MaxTotalInflated` / `max_total_inflated` | 2 GiB | many deflated members that each stay under the per-file limit |
| `MaxResultBytes` / `max_result_bytes` | 64 MiB | text kept: stored values plus display copies |
| `MaxUidChars` / `max_uid_chars` | 64 | a UID of megabytes |
| `MaxTextBytes` / `max_text_bytes` | 1,024 | long names and descriptions |
| `MaxDepth`, `MaxElements`, `MaxInflatedSize`, `MaxValueBytes` | 16, 200,000, 128 MiB, 65,536 | the preview's read limits, per file |

Once a total budget is reached, every later file is recorded as skipped for it; names of skipped
files are kept while the text budget allows. Each `Add` is one entry of small fixed size
whatever happens, so cap the number of members you pass as you cap the attachment's size. The
fixed part of a kept file is about 2.5 kB on top of its text (10,000 small files: about 25 MiB of
heap in the tests).

`Dicomdir.Read(path, ...)` and `siaara_dicomdir_read` read a DICOMDIR on disk exactly as before;
the record limits (`MaxRecords`, `MaxBytes`) are options there with 0 = no limit, and on by
default only for the reader from bytes (100,000 records, 64 MiB). The reader from bytes uses the
preview's element limit too (200,000), which a DICOMDIR of about 20,000 records reaches first.

## 8. The command-line tools

    siaara-dump image.dcm                        # elements, with private names from the dictionary
    siaara-echoscu -aec ARCHIVE host 104         # C-ECHO
    siaara-deid in.dcm out-folder                # de-identification (PS3.15 Annex E)
    siaara-repair validate in.dcm                # validation; "report" and "fix" repair with a report

and `siaara-storescu`, `siaara-storescp`, `siaara-findscu`, `siaara-movescu`, `siaara-getscu`,
`siaara-webcli` (QIDO-RS / WADO-RS / STOW-RS from the shell).

Each prints its options with `--help`.
