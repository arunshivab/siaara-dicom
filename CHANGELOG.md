# Changelog

All notable changes of the SIAARA DICOM Toolkit, newest first. The topmost section is the text
of the GitHub release notes of the release being cut (`tools/release/version.py
--release-notes`). The first release's section carries no version number on purpose: its number
is the owner's choice and lives in one place only (docs/RELEASING.md). Later sections are headed
with their version (`## x.y.z`).

## First release

C ABI version 1 (`SIAARA_ABI_VERSION`).

### What it contains

**Interfaces and packaging**
- C API in one header (`include/siaara/siaara.h`), shared and static library, CMake package
  (`find_package(siaara)`), pkg-config file, ten command-line tools.
- C# bindings for .NET 10 as one NuGet package (`Siaara`) carrying the native library for
  win-x64, win-x86, win-arm64, linux-x64, linux-arm64, osx-x64, osx-arm64; no package
  dependencies; trimmable and NativeAOT compatible. Published releases are on the owner's
  GitHub Packages feed.
- A preview of untrusted DICOM bytes (a mail attachment, an upload) is part of the C# package:
  `DicomAttachmentPreview.Create` gives a caption and a small picture, or the reason there is
  none, within explicit limits (`DicomPreviewOptions`; `ToReadOptions()` is the preset for any
  read of untrusted bytes). Captions lose control and format characters;
  `DicomAttachmentPreview.HtmlEscape` escapes them for a page. It bounds memory, not time: see
  the known limitations.
- Platforms: Windows (x64, x86, ARM64), Linux glibc (x64, x86, ARM64), macOS 12+ (universal).
- Zero third-party dependencies. Reproducible archives, an SPDX 2.3 SBOM per archive, SHA-256
  checksums, build provenance attestation.

**Files and data sets**
- Part 10 reading and writing with byte-for-byte fidelity for unedited files; lenient reading
  with a full report of what was unusual and whether the read was complete; files that were not
  read completely are never saved or sent as if whole unless explicitly allowed.
- Opening a file by path is an option (`open_mode`): by default files up to 256 MiB are read
  into the library's own memory and larger ones are memory-mapped; always-read and always-map
  can be chosen. Values, the read report and unchanged saves are the same in every mode.
- Data dictionary and UID registry from the standard; a private-tag dictionary built only from
  vendor conformance statements (Siemens, GE, Philips, Canon/Toshiba, Varian, Elekta).
- Access by tag, keyword, path and named module; DICOM JSON and XML.
- Validation against the IOD of the SOP class, and a repair function with a report.
- All Specific Character Sets of the standard, ISO 2022 code extensions included.

**Pixel data**
- Codecs written from the standards: RLE, JPEG baseline / extended / lossless, JPEG-LS, JPEG 2000,
  HTJ2K, Deflate, encapsulated uncompressed. Decode, encode, transcode.
- A lossy transcode makes a derived image as the standard requires (new SOP Instance UID, source
  reference, derivation and lossy-compression attributes), with the history lists kept aligned or
  the misalignment reported.
- Limits for untrusted input, per opened file: nesting depth, number of elements and items,
  inflated size, largest decoded frame, frame memory budget; a worked example for previewing a
  mail attachment safely (C and C#).
- Rendering: Modality and VOI LUTs, windowing, palettes, overlays, shutters, Grayscale Softcopy
  Presentation States; JPEG and PNG export.
- Volumes from series, multi-planar reformatting, projections, fusion; optional GPU backends.
- Stored value and modality value at a point (2D and 3D); geometry; RT Dose scaling.

**Network**
- DIMSE as SCU and SCP: Verification, Storage, Query/Retrieve (C-FIND, C-MOVE, C-GET; Patient
  Root and Study Root), Modality Worklist, MPPS, Storage Commitment, Unified Procedure Step,
  Instance Availability Notification.
- DICOM over TLS and HTTPS with the operating system's TLS stack (PS3.15 B.12 / B.13 profiles).
- DICOMweb client and server: QIDO-RS, WADO-RS (instances, metadata, frames, bulk data),
  WADO-URI, STOW-RS.
- Safe defaults: an SCP accepts only listed calling AE titles; incomplete data received is
  quarantined (checked against the quarantine's caps before it is copied, and streamed to disk);
  resource limits on every input; allow-lists per service. A DICOMweb retrieve that has to
  decode is sized before it is sent (413 up front) and decoding draws on a server-wide budget.

**Storage and media**
- A local instance store with an index, duplicate policies, quota, verification and rebuild, and
  a quarantine area for incomplete data.
- DICOMDIR reading and writing (General Purpose CD and USB profiles); folder scanning.

**Clinical objects and privacy**
- Readers for Structured Reports, Segmentations, RT Structure Sets, RT Dose, waveforms, Key
  Object Selection.
- De-identification after PS3.15 Annex E (Basic Profile and options), with keyed, repeatable
  UID and identifier replacement.

### Known limitations

Stated plainly, so that nobody finds them by surprise.

**Not included**
- **Video codecs and JPEG XL**: MPEG-2, MPEG-4 AVC / H.264, HEVC / H.265 and JPEG XL pixel data
  is read, stored, sent and written untouched, but it cannot be decoded, displayed or encoded.
  JPIP and SMPTE ST 2110 referenced pixel data is not fetched.
- **DICOMweb**: rendered resources (`/rendered`, thumbnails) are not supported (the server
  answers 406); UPS-RS is not supported; STOW-RS takes `application/dicom` parts only (no
  metadata + bulk data form).
- **Print Management is not in the C API or the C# bindings.** (The C++ core has a Print SCU;
  it is not part of the installed interface. There is no Print SCP.)
- **SUV and ROI calculations are intentionally not in the toolkit.** It gives stored values,
  modality values, units and geometry; calculations on top belong to the application.
- User Identity negotiation, and setting a product's own Implementation Class UID / Version
  Name, are not available through the C API.
- No audit trail (PS3.15 audit messages) and no user authentication: these are the product's.

**Behaviour to know**
- **MPPS and UPS state is held in memory** unless the product configures a state folder
  (`state_folder`); without one it is lost when the SCP stops.
- **Incomplete-data policies: five of seven are available.** `QUARANTINE` (the default),
  `REFUSE`, `REFUSE_KEEP_COPY` and `ACCEPT_AS_RECEIVED` for receivers, plus the sender-side
  `allow_incomplete`. `TIERED_BY_DAMAGE` and `REPAIR_ON_RECEIPT` are not built yet, and STOW-RS
  has no per-client policy (one policy per server; the Storage SCP has per-calling-AE overrides).
- **Files zero-padded to a block size now carry read loss.** A file followed by a run of zero
  bytes (some archives and media pad files to a block size) is read, and the bytes after the
  data set are reported as not read: such a file is refused by a save unless incomplete saves
  are allowed, strict mode rejects it, and when received over the network it goes to the
  receive policy (quarantine by default).
- One recovery heuristic of the file reader (overlong element lengths as written by some GE
  systems) is on, in a narrowed form, for local file reads and off for all network input. It can
  misread a non-conformant file whose bytes happen to look like that length error; every such
  read is flagged (`SIAARA_READ_RECOVERED`, warning `ELEMENT_LENGTH_RECOVERED`), is never saved
  or sent as if whole unless explicitly allowed, and the heuristic can be switched off
  (`recover_overlong_lengths`).
- A Segmentation object that states a false frame count can cost up to about 15 seconds and
  336 MB before it is rejected.
- 3D rendering and volume resampling cannot be cancelled once started.
- **Decoding has no time bound and cannot be cancelled.** The limits for untrusted input
  (`siaara_read_options`, `DicomPreviewOptions`) bound memory and the amount of work, not the
  time: one frame of a progressive JPEG with hundreds of legal scans can take many seconds
  (`max_scans` / `MaxScans` caps the scans; the preview uses 64). Previews and any other decode
  of untrusted input must run in a **separate process with a kill timer and a memory limit**;
  a thread cannot be abandoned. (A cancel token and a deadline for decodes are planned.)
- **Inflating needs up to three times the inflated size, and up to twice `max_inflated_size`**,
  while a deflated data set is read: the output buffer grows by doubling, and the old and the
  new buffer exist together for a moment. Measured with a limit of 128 MiB: 187 MiB for a data
  set of 63 MiB, 227 MiB for one of 100 MiB or for a stream that is refused at the limit. Size
  the memory limit of a preview worker for it.
- **A header-only open reads the whole file in the default open mode.** With
  `SIAARA_OPEN_AUTO`, `stop_before_pixel_data` still brings a file of up to 256 MiB into memory
  before it is parsed. For header-only scans of your own files use `SIAARA_OPEN_MAP`.
- **`SIAARA_OPEN_AUTO` holds up to 256 MiB per open file in memory**, outside any budget
  (`open_read_threshold` lowers it; larger files are mapped).
- **A rendered frame costs 4 bytes per pixel at image resolution**, whatever the size of the
  viewport, and is refused (`SIAARA_ERR_LIMIT_EXCEEDED`) when that and the decoded frame do not
  fit `frame_memory_budget`; the viewport is not sampled straight from the source yet.
- **A text value is decoded whole by the string getters** (`siaara_dataset_get_utf8` and the
  others report the needed size, so they convert the entire value first). For untrusted input
  check the stored length first (`siaara_dataset_get_raw`, `DicomDataset.TryGetValueLength`), as
  the preview does (`MaxValueBytes`).
- **On Windows a mapped file cannot be shortened or replaced by another process**, so the
  hazard of the next item does not arise there; it applies to POSIX systems.
- **More than about 64 SOP classes in one C-STORE batch** need more than the 128 presentation
  contexts an association can carry (two per class, more with transcoding targets):
  `siaara_dimse_store_files` then fails with `SIAARA_ERR_LIMIT_EXCEEDED` before connecting. Send such
  a set in smaller batches.
- A crash while an instance is being written into the store can leave a `*.siaara-tmp` file
  under `data/`; `siaara_store_verify` with repair removes it. (Leftover temporary files of the
  quarantine and of the spool are removed automatically after an hour.)
- There is no single C function that fills `siaara_read_options` with the limits for untrusted
  input; the values are in `examples/c/attachment_preview.h` (`preview_default_limits`) and, in
  C#, `DicomPreviewOptions.Recommended.ToReadOptions()`.
- New UIDs are not fork-safe: a process that calls `fork()` and creates UIDs in both parent and
  child without an `exec` can produce the same UID twice.
- **Memory-mapped files and files that shrink.** A file that is memory-mapped (larger than
  256 MiB by default, or opened with `SIAARA_OPEN_MAP`; also files a store references in place,
  and header-only folder scans) and is shortened by another process while mapped can end the
  process when the missing part is touched. The size is re-checked after mapping and before
  pixel data is decoded (`SIAARA_ERR_TRUNCATED`), which narrows the window but cannot close it.
  For files on shared or network folders that others rewrite in place, use `SIAARA_OPEN_READ`.
- **The DICOMweb server does not compress on retrieve.** WADO-RS and WADO-URI serve the stored
  transfer syntax or an uncompressed one; a request for another compressed transfer syntax is
  answered 406. (Planned for the next release.)
- **Lossy coding on request exists for JPEG-LS Near-Lossless and JPEG 2000 only.** HTJ2K is
  written losslessly only; a request for lossy HTJ2K is refused.
- **JPEG-LS near-lossless coding of signed pixel data is refused** (the error bound would apply
  to the bit pattern, not to the value); lossless JPEG-LS of signed data works.
- **JPEG Extended (Process 2 & 4) needs 8- or 12-bit data**; other bit depths are refused for
  that transfer syntax.
- The validator decides the conditions of the Segmentation functional groups it can decide from
  the data set (Segment Identification, Pixel Measures, Plane Position and Orientation); a
  missing Frame Content group ("required if not empty") is not reported.
- Only 8- and 16-bit integer pixel data can be recoded by the transcoder; other pixel data is
  carried unchanged or the transcode is refused.
- A blocking C call occupies its thread; the C# `...Async` methods run it on a pool thread.
  On macOS a TLS connect in progress runs to its timeout when cancelled.
- musl-based Linux (Alpine) is not a supported target; 32-bit x86 Linux has no .NET package
  (there is no such .NET runtime).

**Platforms and assurance**
- **Windows- and macOS-specific code is tested by CI only**, not on the developers' machines:
  the SChannel TLS backend (`src/net/tls_schannel.cpp`), the Network.framework TLS backend
  (`src/net/tls_apple.cpp`), the Metal GPU backend (`src/gpu/metal_backend.cpp`), Winsock and
  the Windows parts of file handling (owner-only DACLs, atomic replace, long paths:
  `src/core/secure_file.hpp`, `src/core/io.cpp`, `src/net/socket.cpp`, `src/net/spool.cpp`,
  `src/store/`). CI builds and runs the full test suite on Windows x64 / x86 / ARM64 and macOS
  (both slices of the universal binary). Reproducibility of the archives has been verified on
  Linux only.
- **linux-arm64 release binaries carry no BTI / PAC marking**: the code is compiled with
  `-mbranch-protection=standard`, but the statically linked C/C++ runtime of the build image
  lacks the marking, so the linker drops the property for the whole binary. The other hardening
  measures are in place and checked.
- **The binaries and the NuGet package are not code signed** in this release. Integrity is
  given by `SHA256SUMS` and the build provenance attestation.
- GPU rendering backends depend on the drivers of the machine; the CPU path is the reference.

**Planned for the second release**
- A programme of further configurability: an audit of the toolkit's fixed choices listed 257
  items that are to become options (the "options audit"). They keep their present behaviour as
  the default.
- For incomplete data: a policy table by kind of damage, repair on receipt, and per-client
  policy for STOW-RS.
