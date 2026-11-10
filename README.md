# SIAARA DICOM Toolkit

A DICOM toolkit with **zero third-party dependencies**: a C++20 core, a stable **C API**
(`include/siaara/siaara.h`, usable from C and C++), **C# bindings** for .NET 10, and command-line
tools. It links only the operating system's libraries and the platform C/C++ runtime; TLS uses
the operating system's own TLS stack.

| | |
|---|---|
| Platforms | Windows (x64, x86, ARM64), Linux (x64, x86, ARM64; glibc), macOS 12+ (x64, ARM64, universal) |
| Interfaces | C API (shared and static library, CMake package, pkg-config); C# / .NET 10 (NuGet package `Siaara` with the native library for every .NET runtime) |
| Files | Part 10 read and write with byte-for-byte fidelity, lenient reading with a report of everything unusual, DICOM JSON and XML, validation against the IOD, repair |
| Pixel data | Uncompressed, RLE, JPEG (baseline, extended, lossless), JPEG-LS, JPEG 2000, HTJ2K, Deflate: decode, encode and transcode; rendering (LUTs, window, palettes, overlays, presentation states), 3D volumes and MPR |
| Text | Every Specific Character Set of the standard, ISO 2022 code extensions included; UTF-8 at the API |
| Network | DIMSE as SCU and SCP: Verification, Storage, Query/Retrieve (FIND, MOVE, GET), Modality Worklist, MPPS, Storage Commitment, UPS, Instance Availability; DICOM over TLS |
| DICOMweb | QIDO-RS, WADO-RS, WADO-URI, STOW-RS, client and server |
| Storage | A local instance store with an index, a quarantine for incomplete data, DICOMDIR read and write |
| Other | De-identification (PS3.15 Annex E), a private-tag dictionary built from vendor conformance statements, SR / SEG / RT Structure Set / RT Dose readers |

What is *not* in this release is listed, plainly, in [CHANGELOG.md](CHANGELOG.md) ("Known
limitations"). The DICOM Conformance Statement of the toolkit is
[docs/CONFORMANCE.md](docs/CONFORMANCE.md); a product built on the toolkit issues its own.

## Quick start

The snippets below are shortened from the programs in [`examples/`](examples) (fixed ports in
place of ephemeral ones, error checks left out); the programs themselves are built and run by the
test suite, so the calls shown here are known to compile and work. [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) walks through them and
shows how to link (CMake, pkg-config, NuGet).

### C: open a file, read by keyword and by path, read a pixel value

```c
#include <siaara/siaara.h>

siaara_file* file = NULL;
siaara_dataset* ds = NULL;
if (siaara_file_open(path, strlen(path), NULL, &file) != SIAARA_OK) return fail("open");
if (siaara_file_dataset(file, &ds) != SIAARA_OK) return fail("dataset");

/* Patient's Name by keyword: the dictionary gives the tag, the value comes as UTF-8. */
siaara_dict_entry entry;
memset(&entry, 0, sizeof entry);
entry.struct_size = sizeof entry;
siaara_dict_lookup_keyword("PatientName", strlen("PatientName"), &entry);
char text[256];
size_t needed = 0;
siaara_dataset_get_utf8(ds, entry.tag, 0, text, sizeof text, &needed);

/* The same by path; paths also reach into sequences: "BeamSequence[0].BeamName". */
siaara_path_value value;
memset(&value, 0, sizeof value);
value.struct_size = sizeof value;
siaara_get_by_path(ds, "PatientName", strlen("PatientName"), &value, text, sizeof text, &needed);

/* Stored value and modality value (rescale applied) at column 1, row 0 of frame 0. */
siaara_pixel_value pixel;
memset(&pixel, 0, sizeof pixel);
pixel.struct_size = sizeof pixel;
siaara_file_pixel_value(file, 0, 1, 0, 0, &pixel);
printf("stored %lld, modality value %g\n", (long long)pixel.stored, pixel.modality_value);

siaara_file_free(file);
```

Every function returns a `siaara_status`; `siaara_last_error_message()` gives the text of the
last error on the calling thread.

### C: an SCP, C-ECHO and C-STORE, and a DICOMweb query

```c
/* A Storage SCP (Verification is always on) that files what it receives in a store. */
siaara_store* store = NULL;
siaara_store_open(store_root, strlen(store_root), NULL, &store);
const char* peers[] = {"EXAMPLE_SCU"};          /* the calling AE titles that may connect */
siaara_scp_options options;
memset(&options, 0, sizeof options);
options.struct_size = sizeof options;
options.port = 11112;
options.ae_title = "EXAMPLE_SCP";
options.allowed_calling_aes = peers;
options.allowed_count = 1;
siaara_scp* scp = NULL;
siaara_scp_start(&options, store, NULL, NULL, &scp);

/* C-ECHO, then C-STORE of a file. */
siaara_dimse_peer peer;
memset(&peer, 0, sizeof peer);
peer.struct_size = sizeof peer;
peer.host = "127.0.0.1";
peer.port = 11112;
peer.calling_ae = "EXAMPLE_SCU";
peer.called_ae = "EXAMPLE_SCP";
siaara_dimse_result result;
memset(&result, 0, sizeof result);
result.struct_size = sizeof result;
siaara_dimse_echo(&peer, NULL, &result);
const char* files[] = {"image.dcm"};
siaara_dimse_store_files(&peer, files, NULL, 1, NULL, NULL, NULL, &result);

/* DICOMweb (QIDO-RS, WADO-RS, STOW-RS) over the same store, then a QIDO-RS query. */
siaara_web_server_options web_options;
memset(&web_options, 0, sizeof web_options);
web_options.struct_size = sizeof web_options;
web_options.port = 8080;                        /* plain HTTP is served on loopback only */
siaara_web_server* web = NULL;
siaara_web_server_start(&web_options, store, &web);
siaara_web_client* client = NULL;
const char* url = "http://127.0.0.1:8080/dicomweb";
siaara_web_client_create(url, strlen(url), 0, &client);
char json[8192];
size_t needed = 0;
const char* query = "PatientID=EX-0001";
siaara_web_qido(client, SIAARA_QIDO_STUDIES, NULL, NULL, query, strlen(query), json, sizeof json, &needed);
```

`siaara_scp_start_ex` adds Query/Retrieve, Modality Worklist, MPPS, Storage Commitment and UPS
on the same port; see [docs/NETWORKING.md](docs/NETWORKING.md).

### C++

The public interface for C++ programs is the same C API (the C++ classes of the core are
internal and not installed). A `unique_ptr` with a deleter is all the wrapping it needs:

```cpp
#include <siaara/siaara.h>

struct FileCloser {
    void operator()(siaara_file* f) const noexcept { siaara_file_free(f); }
};
using File = std::unique_ptr<siaara_file, FileCloser>;

File open_file(const std::string& path) {
    siaara_file* raw = nullptr;
    check(siaara_file_open(path.data(), path.size(), nullptr, &raw), "open");
    return File(raw);
}
```

### C#

```csharp
using Siaara;
using Siaara.Network;
using Siaara.Web;

using (DicomFile file = DicomFile.Open(sample))
{
    DicomDataset ds = file.Dataset;
    Console.WriteLine($"PatientName = {ds.GetString(DicomTag.Parse("PatientName"))}");
    Console.WriteLine($"PatientID   = {ds.GetString(DicomTags.PatientID)}");
    DicomPathValue? byPath = ds.GetByPath("PatientName");
    PixelValue pixel = file.GetPixelValue(frame: 0, column: 1, row: 0);
    Console.WriteLine($"pixel (1,0): stored {pixel.Stored}, modality value {pixel.ModalityValue}");
}

using DicomStore store = DicomStore.Open(Path.Combine(work, "store"));
using StorageScp scp = StorageScp.Start(
    new StorageScpOptions
    {
        Port = 11112,
        AeTitle = "EXAMPLE_SCP",
        AllowedCallingAeTitles = new[] { "EXAMPLE_SCU" },
    },
    store);

var peer = new DimsePeer { Host = "127.0.0.1", Port = scp.Port, CallingAeTitle = "EXAMPLE_SCU", CalledAeTitle = "EXAMPLE_SCP" };
DimseResult echo = Dimse.Echo(peer);
DimseResult stored = Dimse.StoreFiles(peer, new[] { sample });

using DicomWebServer web = DicomWebServer.Start(new DicomWebServerOptions { Port = 8080 }, store);
using var client = new DicomWebClient($"http://127.0.0.1:{web.Port}/dicomweb");
string json = client.Search(QidoLevel.Studies, "PatientID=EX-0001");
```

### Showing a DICOM file that arrived as untrusted bytes (a mail attachment, an upload)

Open from memory with explicit limits, decode a fixed number of frames, render at a fixed size,
encode PNG / JPEG - or say plainly why there is no picture. The recommended settings and what
each protects against are in
[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md#7-showing-a-dicom-attachment-safely-untrusted-input);
the programs are [`examples/csharp/AttachmentPreview`](examples/csharp/AttachmentPreview) and
[`examples/c/attachment_preview.h`](examples/c/attachment_preview.h).

## Command-line tools

`siaara-dump`, `siaara-repair`, `siaara-deid`, `siaara-echoscu`, `siaara-storescu`,
`siaara-storescp`, `siaara-findscu`, `siaara-movescu`, `siaara-getscu`, `siaara-webcli`. Each
prints its usage with `--help`.

## Documentation

| | |
|---|---|
| [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) | Linking and the examples, step by step |
| [docs/CONFORMANCE.md](docs/CONFORMANCE.md) | DICOM Conformance Statement of the toolkit |
| [docs/NETWORKING.md](docs/NETWORKING.md) | DIMSE and DICOMweb: every service, options, integration recipes |
| [docs/DATA_ACCESS.md](docs/DATA_ACCESS.md) | Paths, named data, pixel and voxel values |
| [docs/VALIDATION.md](docs/VALIDATION.md) | Validation against the IOD, repair |
| [docs/PRIVATE_DICTIONARY.md](docs/PRIVATE_DICTIONARY.md) | The private-tag dictionary and where it comes from |
| [docs/README.md](docs/README.md) | Index of all documents, the ones for maintainers included |
| [CHANGELOG.md](CHANGELOG.md) | What each release contains, and its known limitations |
| [SECURITY.md](SECURITY.md) | Reporting a vulnerability |

Building from source: [docs/BUILDING.md](docs/BUILDING.md).

## Licence

Proprietary. Copyright (c) 2026 Sthraas Intelligence Pvt Ltd. All rights reserved. No licence is
granted except by written permission; see [LICENSE](LICENSE).
