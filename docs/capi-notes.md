# C API additions for the PACS application: design notes

Open design choices taken on branch `wip-capi`, with the simplest robust option chosen and the
alternatives, for review with the owner. Everything here can still change before the API is
frozen; none of it is in a released ABI yet.

## 1. Where the code lives

**Chosen:** the new exports are in their own translation units (`src/capi/capi_view2d.cpp`,
`capi_net.cpp`, `capi_web.cpp`, `capi_misc.cpp`). `capi.cpp` only gained
`src/capi/capi_internal.hpp` (shared handle structs) and a short bridge block at its end
(thread error, TLS option conversion, live-handle counter).

- Pro: merges with work on `capi.cpp` stay small; much faster rebuilds.
- Con: the handle structs (`siaara_file`, `siaara_store`, `siaara_web_client`, `siaara_volume`)
  moved to a header. The other option, one big `capi.cpp`, means one 3000+ line file.

## 2. Blocking calls, async and cancellation

**Chosen:** every C call is synchronous. Long calls take a `siaara_cancel*` token (DIMSE, volume
build, store verify / rebuild, DICOMDIR write) or have one attached (`siaara_web_client_set_cancel`).
`siaara_cancel_set` may be called from any thread: network calls abort at once (the DIMSE
association gets A-ABORT; the HTTP connection is shut down), file loops stop at the next file.
C# runs the call on a pool thread and ties its `CancellationToken` to a native token.

- Pro: one simple model, no callbacks into managed code to complete operations, no native thread
  pool or event loop to own, works the same in C, C# and NativeAOT.
- Con: a pool thread is busy for the whole call (fine at PACS scale, not for thousands of
  concurrent requests).
- Con: a TCP connect (and TLS handshake) in progress cannot be interrupted; it waits for the
  connect timeout. Closing that gap needs non-blocking connects in `net/socket`.
- Con: cancelling a DIMSE retrieve aborts the association instead of sending C-CANCEL (C-CANCEL is
  sent when a progress / match callback returns non-zero). A gentler "C-CANCEL, then abort after a
  grace period" can be added later without an API change.
- Alternative: a true asynchronous C API (completion callbacks plus a library thread pool). More
  code, harder ownership rules, and every binding then needs marshalling of completions.

## 3. One association per DIMSE call

**Chosen:** `siaara_dimse_*` take a `siaara_dimse_peer` description (host, port, AE titles,
timeouts, max PDU, TLS) and open, use and release one association per call. No SCU handle.

- Pro: stateless and thread-safe; no association lifetime or state machine for callers;
  `siaara_dimse_store_files` already sends many files over one association.
- Con: a caller doing many small C-FINDs pays one association setup each (a few ms on a LAN).
- Alternative: a `siaara_association` handle (connect / find / move / release). Can be added
  later next to the stateless calls.

## 4. C-FIND results

**Chosen:** a callback gets each match as a borrowed `siaara_dataset*` (read with the usual data
set functions or `siaara_dataset_to_json`); returning non-zero sends C-CANCEL. C# collects the
matches as `DicomFile`s (through DICOM JSON).

- Pro: streaming, no result object to free, no limit on the number of matches.
- Con: C# converts each match through JSON (simple, slightly slower than walking the data set).

## 5. 2D rendering: decode once, render at image resolution, then place

**Chosen:** `siaara_frame_open` decodes a frame and copies the data set; `siaara_frame_render`
runs the VOI / colour pipeline at image resolution and then resamples into the viewport
(flips, rotation, zoom, pan; nearest or bilinear on the 8-bit output). The frame is immutable,
so several threads may render one frame at once.

- Pro: window / level drags never decode again; simple and exact pipeline; thread safe.
- Con: bilinear interpolation works on display values, not on modality values (the usual viewer
  behaviour; visible only at large zoom with steep windows). Interpolating modality values first
  would need the pipeline per output pixel.
- Con: cubic interpolation is refused for 2D (`SIAARA_ERR_UNSUPPORTED`).
- Con: with a presentation state the GSPS renderer decodes the frame again on every call
  (`render_with_presentation_state` takes an `Image`, not a decoded frame). Fix later in
  `clinical/gsps` if GSPS window drags matter.
- Open: presentation state text objects are not drawn (the C++ returns them placed; no C API to
  get them yet).

## 6. Logging

**Chosen:** one process-wide callback (`siaara_set_log_callback`) with a level and a
`contains_phi` flag per message. Messages naming SOP Instance UIDs or patient data are flagged;
association, TLS and status messages are not. The callback may run on several threads at once.

- Pro: one place to route diagnostics; the PHI flag lets the app keep identifiers out of general
  logs.
- Con: process-wide (no per-handle loggers). The C# side keeps every replaced handler alive (one
  small object per replacement) rather than risk a call into a freed handle.

## 7. Storage SCP

**Chosen:** `siaara_scp_start` serves Verification and Storage into a `siaara_store`, with an
allowed-peer list; an empty list needs `allow_any_calling_ae` (as main's NET-05 rule for
`ScpServer`). Duplicates follow `SIAARA_DUP_*`. Events go to one callback, serialised per SCP.

- Open: no C-FIND / C-MOVE / C-GET SCP from the store yet (a Q/R SCP over `siaara_store_query`
  would be the next step if the app should act as a PACS for modalities).
- Open: large instances are received in memory (the C++ spool to disk is not wired up).

## 8. Encoders

**Chosen:** `siaara_encode_jpeg` (the toolkit's baseline encoder, YCbCr 4:4:4, a JFIF APP0 added)
and `siaara_encode_png` (written fresh: CRC-32, Adler-32, zlib wrapper over the toolkit's DEFLATE,
per-row adaptive filters). Output in an owned `siaara_buffer`.

- Pro: no double encoding for size queries (the output size is unknown in advance).
- Con: one more handle type to free.

## 9. Smaller choices

- `siaara_dataset_get_strings` returns NUL-separated values in one call, also for sequence items:
  an item without its own (0008,0005) carries the enclosing data set's value (F15, see section 11).
- `siaara_debug_live_handles` counts every handle through a member in each handle struct, so no
  create / free function had to change.
- Lossless JPEG-LS of signed samples codes the two's complement bit patterns (the decoder
  already sign-extends); near-lossless JPEG-LS of signed data stays refused because NEAR would
  bound the error of the bit pattern, not of the value.
- `siaara_gpu_select` affects views that have not rendered on the GPU yet; views that did keep
  their device.

## 9a. Opening by path: read, map, or decide by size (R-160)

`siaara_read_options.open_mode` (C++ `ReadOptions::open_mode`, `io::OpenMode`; C#
`DicomReadOptions.OpenMode`):

| value | what happens | default constant |
|---|---|---|
| `SIAARA_OPEN_AUTO` (default, `io::kDefaultOpenMode`) | files up to `open_read_threshold` are read into owned memory, larger ones are mapped | `io::kDefaultOpenReadThreshold` = 256 MiB |
| `SIAARA_OPEN_MAP` | always the zero-copy mapping | - |
| `SIAARA_OPEN_READ` | always owned memory; a file above `open_read_max_bytes` is `SIAARA_ERR_LIMIT_EXCEEDED`, a failed allocation `SIAARA_ERR_OUT_OF_MEMORY` | `io::kDefaultOpenReadMaxBytes` = 4 GiB |

Why: a mapped file is not a private copy. A file on a shared or network folder that another
process shortens while it is mapped ends the process when a missing page is touched (SIGBUS;
an in-page exception on Windows). Owned memory has no such exposure.

- AUTO applies to `siaara_file_open` and to everything else that opens someone else's file by
  path: `siaara_store_add_file`, DICOMDIR reading and writing, repair and the validation scan,
  volumes built from paths, the files an SCU sends.
- MAP is kept, on purpose, for the store's own instance files when the SCP and the DICOMweb
  server send them (owner-only files that are only ever replaced by rename), for the store's
  index files, and for header-only scans (`siaara_scan_folder`, `siaara_extract_index`), which
  touch a few pages of each file. A store that *references* files in place
  (`SIAARA_ADD_REFERENCE`) serves those files mapped too: keep referenced files on storage that
  nobody rewrites in place.
- What MAP re-checks: the file's size against the mapped length, right after mapping and again
  when frames are opened for decoding (`SIAARA_ERR_TRUNCATED`). On POSIX the check is by name and
  inode (a file replaced by rename is a different file and the old mapping stays whole); on
  Windows through the open handle. This narrows the window and cannot close it: the file can
  still be shortened between the check and the access, and element values other than pixel data
  are read through the mapping without a second check.
- Identical in all modes: values, zero-copy accessors, warnings and the read report, and
  keep-as-read saves (byte-identical).

## 9b. Limits for untrusted input belong to the file handle

`siaara_read_options` carries, appended: `max_inflated_size`, `max_elements` (also the cap on
sequence items of the whole read), `frame_memory_budget` and `max_frame_bytes`. 0 keeps each
library default; nothing changes for callers that do not set them.

- The first two act while the bytes are parsed. The last two are stored in the `siaara_file` and
  apply to every frame function called on it later (`siaara_frame_decode`, `siaara_render_rgba8`,
  `siaara_frame_open`, `siaara_file_pixel_value`, the frame warnings): a decoded frame larger than
  `max_frame_bytes` is `SIAARA_ERR_LIMIT_EXCEEDED` before anything is decoded, and decoding runs
  within `frame_memory_budget`.
- Why on the file and not per call: the caller that opens untrusted bytes is the one that knows
  they are untrusted; every later use of the handle - also by code that only gets the handle -
  is then bounded without having to pass limits again.
- Not covered: transcoding (`siaara_file_transcode`) and volumes built from files decode with the
  library defaults. Do not transcode untrusted input without your own size check.
- There is no time or work limit in the API. See docs/GETTING_STARTED.md section 7.

## 10. Removed: SUV and ROI statistics (ABI change)

Owner's rule: the toolkit returns DICOM data; applications do the calculations. Removed from the
C API: `siaara_suv_factor`, `siaara_suv`, `SIAARA_SUV_*` (warnings, `SIAARA_SUV_SRC_*`,
`SIAARA_SUV_DECAY_*`), `siaara_roi_stats_compute`, `siaara_roi_stats`, `SIAARA_ROI_*`; in C#
`SuvFactors`, `SuvWarnings`, `SuvTimeSource`, `SuvDecayCorrection`, `RoiStatistics`, `RoiShape`; in
C++ `clinical::compute_suv` and the body-size / decay formulas (`clinical/suv.*`), `roi_stats`,
`roi_mask`, `Roi`, `RoiStats`, `RoiOptions`. The Blazor viewer computes SUV and ROI statistics
itself (`app/src/Siaara.Viewer.Core/Measure/`). Every input stays reachable as data: Patient's
Weight / Size / Sex, Units, Decay Correction, Corrected Image, Series / Acquisition Date and Time,
Frame Acquisition DateTime, the Radiopharmaceutical Information Sequence (Total Dose, Half Life,
Start Time / DateTime), private factors by creator through the path API, and modality values with
the pixel spacing for ROIs. `SIAARA_ABI_VERSION` stays 1: no ABI has been released yet (see the top
of this file); the exported-symbol baseline was updated.

## 11. Item character sets (F15)

**Chosen:** a sequence item records the Specific Character Set in force in its enclosing data set
(`Dataset::inherited_charset`, the raw (0008,0005) value). The readers (Part 10 / raw, DICOM
JSON, DICOM XML) fill it in one pass after parsing; `Dataset::put` / `remove` of (0008,0005) or of
a sequence and `siaara_dataset_append_item` keep it current. `charset::character_set_of` uses an
item's own (0008,0005) first, then an explicit inherited value from the caller (as
`siaara_dataset_get_utf8_cs`), then the recorded value, then the default repertoire. So a
borrowed item handle (`siaara_dataset_item`, `siaara_dataset_by_path`) decodes and encodes text
on its own, and C# no longer walks the parent chain for every string.

- Pro: reads stay read-only (item handles may be read from many threads; nothing is computed or
  cached at access time); one native call per value or per element again.
- Con: one string per item. Code that edits the raw bytes of (0008,0005) in place (not through
  `put` / `set_string` / `remove`) must call `propagate_inherited_charset` itself.
- Alternative: a parent pointer in every item (fragile across copies and moves) or computing the
  value when an item handle is fetched (a write during a read: a data race for concurrent readers).
