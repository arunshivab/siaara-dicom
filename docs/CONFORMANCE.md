# SIAARA DICOM Toolkit - DICOM Conformance Statement

Structure: DICOM PS3.2 (Conformance), following the layout of its Annex N template.

> **This is the statement of a toolkit, not of a product.** SIAARA is a library. What a product
> built on it does on a network or on media depends on which services the product enables and how
> it configures them. **A product built on the toolkit must issue its own DICOM Conformance
> Statement**; it may copy the tables of this document, restricted to what it enables, and must
> add what only the product knows (its Application Entities, real-world activities, the IODs it
> creates, its configuration). Section 4.2 is therefore written per *service*, with the default
> of every configurable behaviour, not per Application Entity of a fixed product.

How this document is kept true: the tables between `BEGIN GENERATED` / `END GENERATED` markers
are produced from the code by `tools/release/gen_conformance.py` (transfer syntax registry, codec
capabilities, storage SOP class list, the SOP classes of every service, Query/Retrieve keys,
character sets, default limits). The test `conformance_statement_up_to_date` (ctest) fails when
they differ from the code. The text around them is written by hand from the same sources and from
`docs/NETWORKING.md`; the C API is `include/siaara/siaara.h`, the C# API is in `bindings/csharp`.

## 1. Conformance Statement Overview

The SIAARA DICOM Toolkit reads and writes DICOM files (PS3.10), decodes and encodes pixel data,
and implements DICOM network services as Service Class User (SCU) and Service Class Provider
(SCP), DICOMweb services as user agent and origin server, and DICOMDIR media directories.

Implementation identification:

<!-- BEGIN GENERATED: identity -->
| Item | Value |
|---|---|
| Application Context Name | `1.2.840.10008.3.1.1.1` |
| Implementation Class UID | `2.25.208936390756097257212108686309372694101` |
| Implementation Version Name | `SIAARA_010` |
| Toolkit version | 0.1.0 |
<!-- END GENERATED: identity -->

The Implementation Version Name is derived from the toolkit version (`SIAARA_` + the version
digits) and changes with every release. A product may not change these two values through the C
API in this release.

### Table 1-1: Network services

"Yes" under User / Provider means the C API (and the C# bindings) offer the role. "C++ layer
only" means the C++ core implements it but the installed C API does not expose it in this
release: a product cannot use it and must not claim it.

<!-- BEGIN GENERATED: overview-network -->
| SOP Class | UID | User of Service (SCU) | Provider of Service (SCP) |
|---|---|---|---|
| Verification SOP Class | `1.2.840.10008.1.1` | Yes | Yes |
| Storage: every SOP class of table B.1 (35), or any list the application gives | see B.1 | Yes | Yes |
| Patient Root Query/Retrieve Information Model - FIND | `1.2.840.10008.5.1.4.1.2.1.1` | Yes | Yes |
| Study Root Query/Retrieve Information Model - FIND | `1.2.840.10008.5.1.4.1.2.2.1` | Yes | Yes |
| Patient Root Query/Retrieve Information Model - MOVE | `1.2.840.10008.5.1.4.1.2.1.2` | Yes | Yes |
| Study Root Query/Retrieve Information Model - MOVE | `1.2.840.10008.5.1.4.1.2.2.2` | Yes | Yes |
| Patient Root Query/Retrieve Information Model - GET | `1.2.840.10008.5.1.4.1.2.1.3` | Yes | Yes |
| Study Root Query/Retrieve Information Model - GET | `1.2.840.10008.5.1.4.1.2.2.3` | Yes | Yes |
| Composite Instance Root Retrieve - MOVE | `1.2.840.10008.5.1.4.1.2.4.2` | C++ layer only | Yes |
| Composite Instance Root Retrieve - GET | `1.2.840.10008.5.1.4.1.2.4.3` | C++ layer only | Yes |
| Composite Instance Retrieve Without Bulk Data - GET | `1.2.840.10008.5.1.4.1.2.5.3` | C++ layer only | Yes |
| Modality Worklist Information Model - FIND | `1.2.840.10008.5.1.4.31` | Yes | Yes |
| Modality Performed Procedure Step SOP Class | `1.2.840.10008.3.1.2.3.3` | Yes | Yes |
| Storage Commitment Push Model SOP Class | `1.2.840.10008.1.20.1` | Yes | Yes |
| Unified Procedure Step - Push SOP Class | `1.2.840.10008.5.1.4.34.6.1` | Yes (generic DIMSE-N, C-FIND) | Yes |
| Unified Procedure Step - Watch SOP Class | `1.2.840.10008.5.1.4.34.6.2` | Yes (generic DIMSE-N, C-FIND) | Yes |
| Unified Procedure Step - Pull SOP Class | `1.2.840.10008.5.1.4.34.6.3` | Yes (generic DIMSE-N, C-FIND) | Yes |
| Unified Procedure Step - Event SOP Class | `1.2.840.10008.5.1.4.34.6.4` | C++ layer only (receiving event reports) | Yes (sends event reports to subscribers) |
| Unified Procedure Step - Query SOP Class | `1.2.840.10008.5.1.4.34.6.5` | Yes (generic DIMSE-N, C-FIND) | Yes |
| Instance Availability Notification SOP Class | `1.2.840.10008.5.1.4.33` | Yes (generic DIMSE-N) | Yes |
| Basic Grayscale Print Management Meta SOP Class | `1.2.840.10008.5.1.1.9` | C++ layer only (not in the C API) | No |
| Basic Color Print Management Meta SOP Class | `1.2.840.10008.5.1.1.18` | C++ layer only (not in the C API) | No |
| Basic Film Session SOP Class | `1.2.840.10008.5.1.1.1` | C++ layer only (not in the C API) | No |
| Basic Film Box SOP Class | `1.2.840.10008.5.1.1.2` | C++ layer only (not in the C API) | No |
| Basic Grayscale Image Box SOP Class | `1.2.840.10008.5.1.1.4` | C++ layer only (not in the C API) | No |
| Basic Color Image Box SOP Class | `1.2.840.10008.5.1.1.4.1` | C++ layer only (not in the C API) | No |
| Printer SOP Class | `1.2.840.10008.5.1.1.16` | C++ layer only (not in the C API) | No |
<!-- END GENERATED: overview-network -->

Unified Procedure Step: on an association negotiated for any UPS SOP class, the Requested /
Affected SOP Class UID of the DIMSE-N commands is that of UPS Push, as PS3.4 CC.2 requires; the
SCP accepts that and the negotiated UID.

### Table 1-2: DICOMweb services (PS3.18)

| Service | User agent (`siaara_web_*`, `DicomWebClient`) | Origin server (`siaara_web_server_start`, `DicomWebServer`) |
|---|---|---|
| QIDO-RS: search for studies, series, instances | Yes | Yes |
| WADO-RS: retrieve study / series / instance | Yes | Yes |
| WADO-RS: retrieve metadata | Yes | Yes |
| WADO-RS: retrieve frames | Yes | Yes |
| WADO-RS: retrieve bulk data | via the BulkDataURI of the metadata | Yes |
| WADO-RS: retrieve **rendered** resources (`/rendered`, thumbnails) | No | **No** (answered 406) |
| WADO-URI | Yes | Yes, `contentType=application/dicom` only |
| STOW-RS: store instances | Yes (`application/dicom` parts) | Yes (`multipart/related; type="application/dicom"` only) |
| UPS-RS (worklist service) | No | **No** |
| Capabilities, server-side rendering, storage of metadata + bulk data (`application/dicom+json` / `+xml` STOW) | No | **No** |

### Table 1-3: Media services (PS3.10, PS3.11)

| Media Storage Application Profile | Write (File-set Creator) | Read (File-set Reader) |
|---|---|---|
| General Purpose CD-R / DVD-RAM (STD-GEN-CD, STD-GEN-DVD-RAM): `SIAARA_PROFILE_GENERAL_CD` | Yes (files and DICOMDIR; the product writes the medium) | Yes |
| General Purpose USB / flash (STD-GEN-USB-*, STD-GEN-SEC-USB-* without the security envelope): `SIAARA_PROFILE_GENERAL_USB` | Yes | Yes |

The toolkit writes a file-set to a folder. It does not burn or format media, and it does not
create or open the secure (encrypted) envelopes of the SEC profiles.

## 2. Table of Contents

1. Conformance Statement Overview
2. Table of Contents
3. Introduction
4. Networking (4.1 Implementation Model, 4.2 AE Specifications, 4.3 Network Interfaces, 4.4 Configuration)
5. DICOMweb
6. Media Interchange
7. Support of Character Sets
8. Security
9. Annexes (A: IOD contents, B: tables of SOP classes and transfer syntaxes, C: data dictionary of
   private attributes, D: coded terminology, E: grayscale and colour consistency, F: extensions,
   specializations, privatizations, G: what is not supported)

## 3. Introduction

### 3.1 Revision history

| Document version | Toolkit version | Notes |
|---|---|---|
| 1 | see table "Implementation identification" above | First release of the toolkit |

### 3.2 Audience, remarks

For integrators and for the authors of the Conformance Statements of products built on the
toolkit. Conformance to DICOM does not by itself guarantee interoperability: the integrator
compares this statement (and the product's) with the statement of the other system and tests
the connection.

### 3.3 Terms, abbreviations, references

Terms as in DICOM PS3.2 to PS3.18. AE: Application Entity. SCU / SCP: Service Class User /
Provider. MWL: Modality Worklist. MPPS: Modality Performed Procedure Step. UPS: Unified Procedure
Step. IAN: Instance Availability Notification. TLS: Transport Layer Security.

The data dictionary, UID registry, de-identification table, context groups and standard palettes
compiled into the toolkit are generated from the DICOM standard's published DocBook sources
(PS3.6, PS3.7, PS3.15 Table E.1-1, PS3.16); the edition is the one the generators were last run
on (see `tools/gen_*.py` and the SBOM of the release).

## 4. Networking

### 4.1 Implementation Model

#### 4.1.1 Application data flow

The toolkit has no fixed Application Entity. It gives the product two building blocks:

- **Initiator calls** (`siaara_dimse_*`, C# `Siaara.Network.Dimse`): each call opens one
  association under the calling AE title the product passes, performs one operation (C-ECHO, a
  C-FIND, a C-MOVE, a C-GET, C-STORE of one or many files, an MPPS N-CREATE or N-SET, a Storage
  Commitment request, one generic DIMSE-N request) and releases the association. Calls may run
  on many threads at the same time.
- **An acceptor** (`siaara_scp_start`, `siaara_scp_start_ex`, C# `StorageScp`): one listening
  port and one AE title, with the services the product enables: Verification (always), Storage
  into a local store, Query/Retrieve from that store, Modality Worklist, MPPS, Storage
  Commitment, UPS, Instance Availability Notification. A product that needs several AE titles
  starts several acceptors.

A product's statement names its AEs (for example STORE-SCP, QR-SCP, WORKLIST-SCP and the AEs
of its initiator activities) and draws its own data flow diagram.

#### 4.1.2 Functional definition

- *Storage SCP*: receives composite instances and adds them to the local store (a folder tree
  with an index). Optionally converts them to one transfer syntax on arrival.
- *Query/Retrieve SCP*: answers C-FIND from the index of the store; performs C-MOVE to the
  destinations of its AE table and C-GET on the requestor's association.
- *Worklist SCP*: answers C-FIND from a list the product sets or from a product callback.
- *MPPS SCP*: keeps performed procedure steps (N-CREATE, N-SET) and raises events for the product.
- *Storage Commitment SCP*: decides commitment from what the store holds and sends the result.
- *UPS SCP*: Push, Pull, Watch, Query; event reports to subscribers.
- *IAN SCP*: accepts notifications and raises an event.
- The SCU calls are the counterparts; the product decides when they run.

#### 4.1.3 Sequencing of real-world activities

Decided by the product. The toolkit imposes only: a Storage Commitment request refers to
instances already sent; MPPS N-SET follows N-CREATE; C-MOVE needs the destination in the SCP's
AE table.

### 4.2 AE Specifications (per service)

#### 4.2.1 Association policies (all services)

<!-- BEGIN GENERATED: defaults -->
| Parameter | Default |
|---|---|
| Local AE title, acceptor / initiator | `SIAARA_SCP` / `SIAARA_SCU` |
| Maximum PDU length offered (received) | 65536 bytes (configurable; hard cap 16777216 bytes) |
| Presentation contexts per association | at most 128 |
| Simultaneous associations accepted | 8, and 4 per peer address |
| Association lifetime / operations per association (acceptor) | 2 h / 100000, then A-ABORT |
| Asynchronous Operations Window accepted | up to 16 operations invoked, performed in order of arrival |
| TCP connect timeout | 15 s |
| ACSE timeout (A-ASSOCIATE / A-RELEASE response) | 30 s |
| DIMSE timeout (response or next fragment) | 60 s |
| ARTIM timer | 30 s |
| Idle timeout (acceptor waiting for a request) | 300 s |
| Write timeout (send-buffer space) | 60 s |
| Incomplete data set received | quarantined; sender answered warning 0xB007 (failure status when refused: 0xC000) |
| Overlong-length recovery heuristic on network input | off |
| QIDO-RS results per request (server) | at most 10000 |
<!-- END GENERATED: defaults -->

- **Application Context**: the DICOM Application Context Name above is the only one proposed
  and accepted.
- **Number of associations**: an initiator call uses one association; the product may run calls
  in parallel. The acceptor limits are in the table (configurable; at most 64 simultaneous
  associations, and one further bound per peer address, IPv6 peers grouped by /64 prefix).
- **Asynchronous nature**: the initiator calls do not propose an Asynchronous Operations Window
  (one operation at a time). The acceptor accepts a proposed window up to the limit in the
  table and performs the requests in the order of arrival.
- **Called / calling AE titles**: the acceptor rejects associations addressed to another AE
  title unless `accept_any_called_ae` is set, and accepts only the calling AE titles the product
  lists (`allowed_calling_aes`); an empty list refuses to start unless `allow_any_calling_ae` is
  set explicitly. Rejections use A-ASSOCIATE-RJ with the PS3.8 reasons (called AE title not
  recognized, calling AE title not recognized).
- **Implementation identifying information**: the Implementation Class UID and Version Name of
  section 1.
- **User Identity negotiation** (PS3.7 D.3.3.7): not offered by the C API in this release (the
  C++ layer can send and check it). A product on the C API does not claim it.
- **SOP Class Extended Negotiation**: Query/Retrieve only (4.2.4). None for Storage.
- **SCU/SCP Role Selection**: proposed for C-GET (the requestor is SCP of the storage SOP
  classes it wants to receive) and, on request, for Storage Commitment reports and generic
  DIMSE-N calls; accepted by the acceptor for C-GET and for Storage Commitment reports.
- **Transfer syntax selection** by the acceptor: the first transfer syntax of its own preference
  list that the presentation context proposes.

The **native transfer syntaxes**, in the order the toolkit prefers and proposes them:

<!-- BEGIN GENERATED: ts-native -->
1. Explicit VR Little Endian (`1.2.840.10008.1.2.1`)
2. Implicit VR Little Endian (`1.2.840.10008.1.2`)
3. Explicit VR Big Endian (`1.2.840.10008.1.2.2`)
<!-- END GENERATED: ts-native -->

#### 4.2.2 Verification

SCU: `siaara_dimse_echo` proposes Verification with the native transfer syntaxes. SCP: always
accepted, with the native transfer syntaxes. Status returned: 0000.

#### 4.2.3 Storage

**SOP classes.** As SCP: the SOP classes of Annex B.1 by default; the product may give its own
list (`storage_sop_classes`) or accept any SOP class (`accept_any_sop_class`). As SCU: the SOP
class of each file sent, whatever it is - the list of Annex B.1 does not limit sending.

**SCP, accepted transfer syntaxes.** By default the native ones (above). With
`accept_compressed`, also, in this order of preference after the native ones:

<!-- BEGIN GENERATED: ts-scp-compressed -->
1. Deflated Explicit VR Little Endian (`1.2.840.10008.1.2.1.99`)
2. RLE Lossless (`1.2.840.10008.1.2.5`)
3. JPEG Baseline (Process 1) (`1.2.840.10008.1.2.4.50`)
4. JPEG Extended (Process 2 & 4) (`1.2.840.10008.1.2.4.51`)
5. JPEG Lossless, Non-Hierarchical (Process 14) (`1.2.840.10008.1.2.4.57`)
6. JPEG Lossless, Non-Hierarchical, First-Order Prediction (Process 14 [Selection Value 1]) (`1.2.840.10008.1.2.4.70`)
7. JPEG-LS Lossless Image Compression (`1.2.840.10008.1.2.4.80`)
8. JPEG-LS Lossy (Near-Lossless) Image Compression (`1.2.840.10008.1.2.4.81`)
9. JPEG 2000 Image Compression (Lossless Only) (`1.2.840.10008.1.2.4.90`)
10. JPEG 2000 Image Compression (`1.2.840.10008.1.2.4.91`)
11. High-Throughput JPEG 2000 Image Compression (Lossless Only) (`1.2.840.10008.1.2.4.201`)
12. High-Throughput JPEG 2000 with RPCL Options Image Compression (Lossless Only) (`1.2.840.10008.1.2.4.202`)
13. High-Throughput JPEG 2000 Image Compression (`1.2.840.10008.1.2.4.203`)
<!-- END GENERATED: ts-scp-compressed -->

The product may instead give its own list in its own order (`transfer_syntaxes`).

**SCP behaviour.**

- Conformance level: the data set is stored as received, all attributes including private ones
  (Level 2, full). No attribute is coerced. Exception, when the product configures
  `store_transfer_syntax`: instances are converted to that transfer syntax before storing; a
  lossy conversion happens only with `allow_lossy` and then changes the instance as described
  in Annex A.2.
- C-STORE command UIDs that differ from the data set's: stored under the data set's UIDs and
  reported to the product by default; refused with A900 when `strict_store_uids` is set.
- Duplicates (same SOP Instance UID): kept or replaced as `duplicate_policy` says (default: keep
  the existing instance).
- A data set that was not received or read completely (truncated, bytes skipped, readable only
  by a recovery heuristic): **quarantined by default** - the bytes are kept exactly as received
  outside the archive index, no query or retrieve returns them, and the sender is answered with
  the warning status of the table in 4.2.1. Configurable per acceptor and per calling AE
  (`incomplete_policy`: quarantine, refuse, refuse and keep a copy, accept as received) with
  configurable answer and status codes (`docs/NETWORKING.md` section 2.3b). The overlong-length
  recovery heuristic of the file reader is never applied to network input.
- Large data sets are received to an owner-only spool file above a threshold (16 MiB by default).
- Status codes returned: 0000 Success; B007 (default) warning for a kept incomplete instance;
  A700 out of resources (store or spool quota, quarantine full); A900 data set does not match
  SOP class / UID mismatch in strict mode; C000 cannot understand (also an incomplete instance
  when the policy is refuse); 0124 not authorized (per-service allow-list).

**SCU behaviour** (`siaara_dimse_store`, `siaara_dimse_store_files` and their `_ex` / `_report`
forms). For each SOP class of the files to send, the SCU proposes:

1. a presentation context with the transfer syntax the file is stored in;
2. a presentation context with the native transfer syntaxes;
3. a presentation context with the transfer syntaxes the transcoder writes without loss:

<!-- BEGIN GENERATED: ts-scu-lossless -->
1. Deflated Explicit VR Little Endian (`1.2.840.10008.1.2.1.99`)
2. RLE Lossless (`1.2.840.10008.1.2.5`)
3. JPEG Lossless, Non-Hierarchical (Process 14) (`1.2.840.10008.1.2.4.57`)
4. JPEG Lossless, Non-Hierarchical, First-Order Prediction (Process 14 [Selection Value 1]) (`1.2.840.10008.1.2.4.70`)
5. JPEG-LS Lossless Image Compression (`1.2.840.10008.1.2.4.80`)
6. JPEG 2000 Image Compression (Lossless Only) (`1.2.840.10008.1.2.4.90`)
7. High-Throughput JPEG 2000 Image Compression (Lossless Only) (`1.2.840.10008.1.2.4.201`)
8. JPEG-LS Lossy (Near-Lossless) Image Compression (`1.2.840.10008.1.2.4.81`)
9. JPEG 2000 Image Compression (`1.2.840.10008.1.2.4.91`)
10. High-Throughput JPEG 2000 Image Compression (`1.2.840.10008.1.2.4.203`)
<!-- END GENERATED: ts-scu-lossless -->

   (JPEG-LS Near-Lossless, JPEG 2000 and HTJ2K are in this list because the toolkit codes
   losslessly in them unless lossy coding is asked for.)
4. only with `allow_lossy_transcode`: a presentation context with the lossy ones:

<!-- BEGIN GENERATED: ts-scu-lossy -->
1. JPEG Baseline (Process 1) (`1.2.840.10008.1.2.4.50`)
2. JPEG Extended (Process 2 & 4) (`1.2.840.10008.1.2.4.51`)
<!-- END GENERATED: ts-scu-lossy -->

The instance is sent in the file's own transfer syntax when accepted, else in a native one, else
transcoded without loss, else (only when allowed) lossily. A lossy transcode on the way gives the
instance a new SOP Instance UID (Annex A.2) unless `keep_uid_on_lossy`; the new UID is used in
the command, the data set and reported per file. A file whose read was not complete is not sent
unless `allow_incomplete`. At most 128 presentation contexts per association. Statuses handled:
0000, warnings (B000, B006, B007: counted as stored with warning), failures (reported per file;
the association continues with the next file).

#### 4.2.4 Query/Retrieve

**SOP classes**: Patient Root and Study Root, FIND, MOVE and GET (table 1-1), as SCU and SCP.
The SCP also accepts Composite Instance Root Retrieve MOVE / GET and Composite Instance Retrieve
Without Bulk Data GET. Patient/Study Only is not supported. Transfer syntaxes of the query and
retrieve contexts: the native ones.

**SCP (from the store).**

- Levels PATIENT, STUDY, SERIES, IMAGE. Matching: single value, list of UIDs (at most 10,000
  values), wild cards, date / time range, sequence matching, universal. The hierarchical key
  rules of PS3.4 C.4.1.2.1 apply unless relational queries were negotiated.
- Extended negotiation accepted when the product enables it (all off by default): relational
  queries and retrieval, combined date-time matching, fuzzy semantic matching of person names.
  Timezone query adjustment is not supported by the SCP.
- Keys the store matches on and returns:

<!-- BEGIN GENERATED: qr-keys -->
| Attribute | Tag | VR | Level | Source | In the default identifier (`siaara_qr_identifier`) |
|---|---|---|---|---|---|
| Patient's Name | (0010,0010) | PN | PATIENT | index field | yes |
| Patient ID | (0010,0020) | LO | PATIENT | index field | yes |
| Issuer of Patient ID | (0010,0021) | LO | PATIENT | index field | no |
| Patient's Birth Date | (0010,0030) | DA | PATIENT | index field | yes |
| Patient's Sex | (0010,0040) | CS | PATIENT | index field | yes |
| Number of Patient Related Studies | (0020,1200) | IS | PATIENT | computed from the index | no |
| Number of Patient Related Series | (0020,1202) | IS | PATIENT | computed from the index | no |
| Number of Patient Related Instances | (0020,1204) | IS | PATIENT | computed from the index | no |
| Study Instance UID | (0020,000D) | UI | STUDY | index field | yes |
| Study Date | (0008,0020) | DA | STUDY | index field | yes |
| Study Time | (0008,0030) | TM | STUDY | index field | yes |
| Accession Number | (0008,0050) | SH | STUDY | index field | yes |
| Study ID | (0020,0010) | SH | STUDY | index field | yes |
| Study Description | (0008,1030) | LO | STUDY | index field | yes |
| Referring Physician's Name | (0008,0090) | PN | STUDY | index field | yes |
| Modalities in Study | (0008,0061) | CS | STUDY | index field | yes |
| Number of Study Related Series | (0020,1206) | IS | STUDY | computed from the index | yes |
| Number of Study Related Instances | (0020,1208) | IS | STUDY | computed from the index | yes |
| Series Instance UID | (0020,000E) | UI | SERIES | index field | yes |
| Modality | (0008,0060) | CS | SERIES | index field | yes |
| Series Description | (0008,103E) | LO | SERIES | index field | yes |
| Series Number | (0020,0011) | IS | SERIES | computed from the index | yes |
| Body Part Examined | (0018,0015) | CS | SERIES | index field | no |
| Series Date | (0008,0021) | DA | SERIES | index field | no |
| Series Time | (0008,0031) | TM | SERIES | index field | no |
| Frame of Reference UID | (0020,0052) | UI | SERIES | index field | no |
| Number of Series Related Instances | (0020,1209) | IS | SERIES | computed from the index | yes |
| SOP Instance UID | (0008,0018) | UI | IMAGE | index field | yes |
| SOP Class UID | (0008,0016) | UI | IMAGE | index field | yes |
| Instance Number | (0020,0013) | IS | IMAGE | computed from the index | yes |
| Content Date | (0008,0023) | DA | IMAGE | index field | no |
| Content Time | (0008,0033) | TM | IMAGE | index field | no |
| Image Type | (0008,0008) | CS | IMAGE | index field | no |
| Rows | (0028,0010) | US | IMAGE | computed from the index | no |
| Columns | (0028,0011) | US | IMAGE | computed from the index | no |
| Number of Frames | (0028,0008) | IS | IMAGE | computed from the index | no |
| Retrieve AE Title | (0008,0054) | AE | every level | set by the SCP in responses | no |
| Instance Availability | (0008,0056) | CS | every level | set by the SCP in responses | no |

38 keys (`key_table()`, src/capi/store_services.cpp).
<!-- END GENERATED: qr-keys -->

  Other optional keys are treated as universal matching and returned empty.
- Responses are encoded in UTF-8 (Specific Character Set `ISO_IR 192`); request identifiers are
  decoded from their own Specific Character Set. Every response carries Retrieve AE Title (the
  SCP's own AE title); Instance Availability (ONLINE) is returned when requested.
- Limits: 10,000 matches per C-FIND (`max_find_results`; then A700 after the matches sent);
  65,535 instances per retrieve (`max_retrieve_instances`; then A701 without sub-operations).
- C-MOVE: the destination AE must be in the SCP's AE table (title, host, port, TLS flag), and,
  when the product sets destination lists per calling AE (`move_acls`), allowed for the caller;
  otherwise A801. The sub-association proposes, per SOP class of the instances to send: the
  stored transfer syntax; the native transfer syntaxes (also for compressed instances, which are
  decompressed for a destination that accepts only those); and the compressed transfer syntaxes
  the transcoder writes (lossy only with `allow_lossy`).
- C-GET: sub-operations go on the requestor's association, in the storage contexts the
  requestor proposed with the SCP role; the SCP accepts for them every transfer syntax it can
  write (native first, lossy last and only with `allow_lossy`) and transcodes per instance.
- A stored file whose read is not complete is not sent as a sub-operation (that sub-operation
  fails) unless `allow_incomplete`.
- Status codes returned: 0000; FF00 pending; FE00 cancel; A700, A701 out of resources; A801 move
  destination unknown; A900 identifier does not match SOP class; B000 sub-operations complete
  with failures or warnings; C000 unable to process; 0124 not authorized.

**SCU.** `siaara_dimse_find`, `_move`, `_get` (and `_ex`): Patient Root or Study Root; extended
negotiation proposed on request (relational, combined date-time, fuzzy names, timezone
adjustment) and the accepted flags reported; C-CANCEL through the cancel token or by returning
from the callback. C-GET proposes the storage SOP classes of Annex B.1 (or the product's list, at
most 120) with the SCP role, with the native transfer syntaxes and, unless `native_only`, the
compressed ones listed in 4.2.3; what is received goes to a store and / or a folder and follows
the same incomplete-data policy as the Storage SCP.

#### 4.2.5 Modality Worklist

SCP: C-FIND on the Modality Worklist Information Model. The candidates come from the product
(a list set with `siaara_scp_set_worklist` / `_json`, or a callback per query); the toolkit
applies the matching of PS3.4 K.6 (single value, wild card, range, sequence) and returns the
requested keys. At most 10,000 items per query by default. A callback that does not answer within
30 s (default) ends the C-FIND with 0110. SCU: `siaara_dimse_find` with the worklist model;
`siaara_worklist_query` builds an identifier. Native transfer syntaxes.

#### 4.2.6 Modality Performed Procedure Step

SCP: N-CREATE (Performed Procedure Step Status must be IN PROGRESS; a duplicate SOP Instance UID
gets 0111) and N-SET until the status is COMPLETED or DISCONTINUED (then 0110), following PS3.4
F.7.2.2. The attributes are kept as sent and handed to the product in events; the toolkit does
not forward them. State is held in memory, or in a state folder when the product configures one
(then it survives a restart). Statuses: 0000, 0106, 0110, 0111, 0112, 0124, A700 (bounds).
SCU: `siaara_dimse_mpps_create`, `siaara_dimse_mpps_set`. Native transfer syntaxes.

#### 4.2.7 Storage Commitment Push Model

SCP: N-ACTION (request). Each referenced instance is committed when the store holds it with the
same SOP class; otherwise it is listed as failed with Failure Reason 0112 (no such object
instance) or 0119 (class / instance conflict); a request above `max_commitment_items` (10,000)
fails every item with 0213 (resource limitation). Instances in the quarantine are not in the
archive and are not committed. The N-EVENT-REPORT is sent, as configured
(`commitment_report_mode`), on the requestor's association, on a new association to the
requestor (looked up in the AE table, proposing the SCP role), or first the one and then the
other (default). No retry of an undelivered report.

SCU: `siaara_dimse_commit` (waits for the report on the same association) and
`siaara_dimse_commit_ex` (also accepts the report on a new association, received by an acceptor
of the product started with `commitment_reports`; reports are matched by Transaction UID and
counted only when they come from the AE the request was sent to).

#### 4.2.8 Unified Procedure Step

SCP: UPS Push, Pull, Watch and Query (N-CREATE, N-GET, N-SET, N-ACTION for state changes and
subscriptions, C-FIND), with N-EVENT-REPORT to subscribers on new associations (receiving AEs
from the AE table). Workitems are held in memory, or in the state folder. N-CREATE needs a
context negotiated for UPS Push. Bounds are configurable (10,000 open workitems by default).
SCU: through the generic DIMSE-N call `siaara_dimse_n_request` and `siaara_dimse_find` with the
UPS model; there are no dedicated UPS functions in the C API. UPS-RS is not supported.

#### 4.2.9 Instance Availability Notification

SCP: N-CREATE is accepted and handed to the product as an event; nothing else is done with it.
SCU: through `siaara_dimse_n_request`.

#### 4.2.10 Print Management

Not offered by the C API or the C# bindings in this release, in either role. (The C++ core
contains a Basic Grayscale / Color Print Management SCU over one association; it is not part of
the installed interface.) There is no Print SCP.

### 4.3 Network Interfaces

TCP/IP over the operating system's sockets, IPv4 and IPv6. Host names are resolved by the
operating system's resolver (DNS). No DHCP, NTP, LDAP or other configuration protocol is used.
Secure transport: section 8.

### 4.4 Configuration

All configuration is given by the product in code (structures of the C API, option records of
the C# API); the toolkit reads no configuration file, registry key or environment variable.

- Local AE title and port: `siaara_scp_options.ae_title`, `port` (0: an ephemeral port).
  Default AE titles are in the table of 4.2.1. PS3.15 reserves port 2762 for DICOM over TLS and
  port 104 / 11112 for DICOM; the toolkit has no default port for the acceptor.
- Remote AEs: the peer of every initiator call (host, port, AE titles, TLS); the acceptor's AE
  table for C-MOVE destinations and report receivers; the lists of allowed calling AEs and the
  allow-lists per service.
- Parameters and defaults: the table of 4.2.1; all timeouts and limits are per call or per
  acceptor.

## 5. DICOMweb

Client and server speak HTTP/1.1; TLS as in section 8. The server serves a local store.

- **QIDO-RS** (server): `/studies`, `/series`, `/instances`, `/studies/{study}/series`,
  `/studies/{study}/instances`, `/studies/{study}/series/{series}/instances`. Matching on
  attributes by keyword or tag, `includefield`, `fuzzymatching`, `limit` and `offset`. Responses:
  `application/dicom+json` (default) or `multipart/related; type="application/dicom+xml"`.
  Results carry the Retrieve URL of their level. A query that matches more than the limit of the
  table in 4.2.1 is cut and answered with a `Warning` header.
- **WADO-RS** (server): study, series and instance as `multipart/related;
  type="application/dicom"`, in the stored transfer syntax or one the `Accept` header asks for
  when the toolkit can convert to it; metadata as JSON or XML (binary values above 1024 bytes as
  BulkDataURI); frames and bulk data as `application/octet-stream` or the compressed media type
  of the stored transfer syntax (`image/jpeg`, `image/jls`, `image/jp2`, `image/jphc`,
  `image/dicom-rle`). **Rendered resources are not supported**: every `/rendered` path answers 406.
- **WADO-URI** (server): `requestType=WADO` with `studyUID`, `seriesUID`, `objectUID`;
  `contentType=application/dicom` only; `transferSyntax` honoured when the toolkit can convert.
  No rendered content types, no `anonymize`, no presentation parameters.
- **STOW-RS** (server): `POST /studies[/{study}]` with `multipart/related;
  type="application/dicom"`. Metadata + bulk data requests (`application/dicom+json`, `+xml`)
  are not supported (415). The response is the PS3.18 store instances response (Referenced SOP
  Sequence, Failed SOP Sequence with failure reasons). Incomplete parts follow the same policy as
  the Storage SCP (quarantine by default). Not offered when the server is read-only (501).
- **Client**: QIDO-RS (JSON), WADO-RS instance / study / series / metadata / frames, WADO-URI,
  STOW-RS of Part 10 files; streaming retrieve of a study or series into a store or folder.
- **Not supported**: UPS-RS, rendered and thumbnail resources, the capabilities service,
  WebSocket notifications, QIDO-RS for other resources than studies / series / instances.
- **Access control** (server): plain HTTP is served on the loopback interface only; on any
  other interface the server requires TLS together with an authorization callback of the product
  (it sees method, target and the Authorization header; 401 when it refuses) or client
  certificates, unless the product explicitly allows otherwise. CORS only for a named origin.

## 6. Media Interchange

### 6.1 Implementation model

`siaara_dicomdir_write` / `_ex` (File-set Creator): copies the given instance files to
`<root>/DICOM/Pnnnnnnn/Snnnnnnn/Ennnnnnn/Innnnnnn` (File IDs of up to 8 characters from A-Z and
0-9, made from counters, never from patient data) and writes `<root>/DICOMDIR` in Explicit VR
Little Endian with PATIENT, STUDY, SERIES records and one leaf record per instance, the record
type chosen from the SOP class (IMAGE, RT DOSE, RT STRUCTURE SET, RT PLAN, RT TREAT RECORD,
SR DOCUMENT, KEY OBJECT DOC, PRESENTATION, ENCAP DOC, WAVEFORM, RAW DATA, REGISTRATION,
FIDUCIAL, SURFACE, VALUE MAP, SPECTROSCOPY, STEREOMETRIC). File-set ID: up to 16 characters.

`siaara_dicomdir_read` (File-set Reader): walks the directory records through their offset
links, reports each record with the keys inherited from its ancestors, and resolves Referenced
File IDs under the DICOMDIR's folder (unsafe File IDs are rejected, never followed). A damaged
link structure is reported and read in sequence order instead.

### 6.2 Application profiles

| Profile | Transfer syntaxes of the instance files written |
|---|---|
| General Purpose CD-R / DVD-RAM (default) | Explicit VR Little Endian only |
| General Purpose USB / flash | Explicit VR Little Endian and every non-retired, little endian, explicit VR transfer syntax of Annex B.2, as the files are stored |

A file in a transfer syntax the profile does not allow is refused unless the product sets
`allow_any_transfer_syntax` (then it is copied and a warning is counted: the file-set no longer
conforms to the profile). The toolkit does not transcode while writing a file-set. SOP classes:
any composite SOP class; the product's statement lists those it puts on media. No private
application profile, no augmented profile.

## 7. Support of Character Sets

The toolkit decodes and encodes the following values of Specific Character Set (0008,0005),
alone or, for the ISO 2022 terms, combined with code extensions (multi-valued (0008,0005),
escape sequences in the values):

<!-- BEGIN GENERATED: charsets -->
| Defined Term (single value / first value) | Defined Term with code extensions | Character sets |
|---|---|---|
| - | ISO 2022 IR 6 | G0: ISO 646 (ISO-IR 6) (the default repertoire; also when (0008,0005) is absent or empty) |
| ISO_IR 100 | ISO 2022 IR 100 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-1 Latin alphabet No. 1 (ISO-IR 100) |
| ISO_IR 101 | ISO 2022 IR 101 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-2 Latin alphabet No. 2 (ISO-IR 101) |
| ISO_IR 109 | ISO 2022 IR 109 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-3 Latin alphabet No. 3 (ISO-IR 109) |
| ISO_IR 110 | ISO 2022 IR 110 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-4 Latin alphabet No. 4 (ISO-IR 110) |
| ISO_IR 144 | ISO 2022 IR 144 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-5 Cyrillic (ISO-IR 144) |
| ISO_IR 127 | ISO 2022 IR 127 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-6 Arabic (ISO-IR 127) |
| ISO_IR 126 | ISO 2022 IR 126 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-7 Greek (ISO-IR 126) |
| ISO_IR 138 | ISO 2022 IR 138 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-8 Hebrew (ISO-IR 138) |
| ISO_IR 148 | ISO 2022 IR 148 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-9 Latin alphabet No. 5 (ISO-IR 148) |
| ISO_IR 203 | ISO 2022 IR 203 | G0: ISO 646 (ISO-IR 6); G1: ISO 8859-15 Latin alphabet No. 9 (ISO-IR 203) |
| ISO_IR 13 | ISO 2022 IR 13 | G0: JIS X 0201 Romaji (ISO-IR 14); G1: JIS X 0201 Katakana (ISO-IR 13) |
| ISO_IR 166 | ISO 2022 IR 166 | G0: ISO 646 (ISO-IR 6); G1: TIS 620-2533 Thai (ISO-IR 166) |
| - | ISO 2022 IR 87 | G0: JIS X 0208 Kanji (ISO-IR 87) |
| - | ISO 2022 IR 159 | G0: JIS X 0212 Supplementary Kanji (ISO-IR 159) |
| - | ISO 2022 IR 149 | G1: KS X 1001 Hangul and Hanja (ISO-IR 149) |
| - | ISO 2022 IR 58 | G1: GB 2312 Simplified Chinese (ISO-IR 58) |
| ISO_IR 192 | - | Unicode in UTF-8 (no code extensions) |
| GB18030 | - | GB 18030 (no code extensions) |
| GBK | - | GBK (no code extensions) |

20 Specific Character Set defined terms.
<!-- END GENERATED: charsets -->

- Text is exchanged with the application as UTF-8 (`siaara_dataset_get_utf8` / `set_utf8`, all
  strings of the C# API). Bytes that are not characters of the character set in force are
  reported (replacement character in the returned text); the stored bytes are never changed by
  reading.
- A sequence item without its own (0008,0005) uses the value of its enclosing data set.
- An unknown defined term, or a data set without (0008,0005) that holds non-ASCII bytes, is
  reported; a product decides the fallback.
- Network: Query/Retrieve and worklist responses of the SCP are sent in `ISO_IR 192` (UTF-8).
  Stored instances keep their own character set; the toolkit never converts the character set
  of an instance it stores or forwards.
- DICOMweb: JSON and XML are UTF-8.

## 8. Security

### 8.1 Secure transport

DICOM over TLS for DIMSE (initiator and acceptor) and HTTPS for DICOMweb, using the operating
system's TLS implementation (Windows: SChannel; macOS: Network.framework; Linux: the system
OpenSSL 3, loaded when TLS is first used). The toolkit contains no cryptographic library.

| Item | Behaviour |
|---|---|
| PS3.15 profiles | B.13 Modified BCP 195 RFC 8996 TLS Secure Transport Connection Profile (default) and B.12 BCP 195 RFC 8996 (C++ setting; the C API uses the default) |
| Protocol versions | TLS 1.2 and TLS 1.3 (1.3 preferred); TLS 1.3 only on request (`tls13_only`) |
| Refused | SSL 2 / 3, TLS 1.0 / 1.1, compression, renegotiation; any cipher suite outside the list below (checked again after the handshake) |
| TLS 1.2 cipher suites | ECDHE key exchange with AES-GCM or ChaCha20-Poly1305 only |
| TLS 1.3 cipher suites | TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256 |
| Peer certificates | validated by default: chain to a trust anchor (OS store or a PEM file), validity dates, and on the client the host name or IP address. Keys: RSA / RSA-PSS of at least 2048 bits or ECDSA of at least 256 bits; signatures SHA-256 or stronger. MD5, SHA-1, SHA-224 signatures and DSA are refused |
| Revocation | checked "soft-fail" by default: a certificate known to be revoked is refused |
| Mutual TLS | client certificate sent when configured; the acceptor / server can require one |
| Fallback | never to plain TCP. A TLS acceptor serves only TLS on its port; plain DIMSE needs a second acceptor on another port |
| Validation off | possible only through an explicit option (`insecure_skip_validation`), logged as a warning on every connection |

TLS is off unless the product configures it.

### 8.2 Association level security

Allowed calling AE titles (required), called AE title check, allow-lists per service
(a refused request is answered 0124), destination lists for C-MOVE per calling AE, limits on
associations per peer, association lifetime and operations per association. AE titles are not
authentication: on a network the product does not control it uses TLS with client certificates.

### 8.3 Application level security

- The toolkit provides no user authentication, no audit trail (PS3.15 Annex A audit messages
  are the product's task) and no encryption of stored files. Files and folders the toolkit
  creates (store, spool, quarantine, state) are created for the owner only by default.
- Log callbacks never receive patient data unless the product turns that on explicitly.
- De-identification (`siaara_deidentify`): PS3.15 Annex E Basic Application Level
  Confidentiality Profile with its options; Annex A.3.

## 9. Annexes

### Annex A: IOD contents

#### A.1 Created SOP instances

The toolkit does not create SOP instances of its own accord. Through the C API a product builds
data sets attribute by attribute (`siaara_file_create`, the `siaara_dataset_set_*` functions) and
is responsible for their IOD conformance; `siaara_validate_json` checks a data set against the
IOD of its SOP class (docs/VALIDATION.md). The product's statement lists the IODs it creates.

What the toolkit itself writes:

- **File Meta Information** of every file it saves: Media Storage SOP Class and Instance UID
  from the data set, the Transfer Syntax UID, and the Implementation Class UID and
  Implementation Version Name of section 1. Other attributes of the data set are written as held.
- **Media Storage Directory** (DICOMDIR): section 6.
- **Response data sets** of its SCPs: Query/Retrieve and worklist responses (the requested
  keys), the Storage Commitment result, the STOW-RS response.

#### A.2 Attributes changed by a transcode

A transcode to another transfer syntax (`siaara_file_transcode`, C-STORE when the peer accepts
only other transfer syntaxes, C-GET / C-MOVE sub-operations, `store_transfer_syntax`) changes:

| Case | Attributes changed |
|---|---|
| Any transcode | Pixel Data (7FE0,0010) and its encapsulation (Basic or Extended Offset Table); Transfer Syntax UID (0002,0010); Planar Configuration (0028,0006) = 0 for colour; Photometric Interpretation (0028,0004) when the codec requires another colour model (for example YBR_FULL_422 for lossy JPEG) |
| **No** lossy compression applied (also same-syntax pass-through) | nothing else: SOP Instance UID, Lossy Image Compression attributes and derivation attributes stay as they are. Lossy Image Compression (0028,2110) stays "01" once it was "01" |
| **Lossy** compression applied (only when the product allows it) | Lossy Image Compression (0028,2110) = "01"; Lossy Image Compression Ratio (0028,2112): the ratio of this step appended; Lossy Image Compression Method (0028,2114): see the alignment rule below; and, unless the product chooses to keep the UID: a **new SOP Instance UID** (0008,0018) (and Media Storage SOP Instance UID), Image Type (0008,0008) value 1 = DERIVED (other values kept), Source Image Sequence (0008,2112) with an item for the predecessor (Referenced SOP Class / Instance UID, Purpose of Reference Code Sequence: (121320, DCM, "Uncompressed predecessor") or (121330, DCM, "Lossy compressed predecessor")), Derivation Code Sequence (0008,9215) with (113040, DCM, "Lossy Compression") appended, Derivation Description (0008,2111) with method and ratio appended |

Lossy compression is applied only by JPEG Baseline and Extended, and by JPEG-LS Near-Lossless,
JPEG 2000 and HTJ2K when the product asks for lossy coding in them (by default those three are
written losslessly). Method terms written: `ISO_10918_1`, `ISO_14495_1`, `ISO_15444_1`,
`ISO_15444_15`.

**Alignment rule for the lossy history lists.** Ratio (0028,2112) and Method (0028,2114) are
parallel lists, one value per lossy step (PS3.3 C.7.6.1.1.5.1). The toolkit looks at how many
values the source has in each and never guesses:

| Source lists | Written | Reported to the product as |
|---|---|---|
| Same number of values in both (or neither present) | one value appended to each | aligned |
| Exactly one Ratio value and no Method value, and the source pixel data is itself lossy coded in JPEG Baseline / Extended, JPEG-LS near-lossless, JPEG 2000 irreversible or HTJ2K irreversible | the missing Method value of that one earlier step is filled in with the term the source's own coding proves; then one value appended to each | method filled (with the term) |
| Anything else (for example 2 Ratio and 1 Method value, or more Method than Ratio values) | the Ratio of this step is appended; Method is left byte for byte as it was | misaligned (with the counts found) |

After a misaligned step the two lists are not pairs value by value, also when they have become
equally long (a source with one more Method than Ratio value).

The new SOP Instance UID of a lossy network transfer is used consistently in the C-STORE
command, the data set and the file meta, and the mapping from the original UID is reported.
Only 8- and 16-bit integer pixel data is recoded; other pixel data is carried unchanged or the
transcode is refused.

#### A.3 Attributes changed by de-identification

`siaara_deidentify` applies PS3.15 Table E.1-1 (Basic Profile and the options the product
selects) and records it: Patient Identity Removed (0012,0062) = YES, De-identification Method
(0012,0063) and De-identification Method Code Sequence (0012,0064) with the codes of the profile
and options applied (Annex D), Longitudinal Temporal Information Modified (0028,0303) where the
date options apply, Burned In Annotation (0028,0301) when pixel data was cleaned. UIDs are
replaced consistently.

#### A.4 Attributes changed by the repair function

`siaara_repair_file` changes only what its report lists (docs/VALIDATION.md); it never runs
implicitly.

#### A.5 Usage of attributes from received IODs

Stored and forwarded instances are not interpreted beyond the attributes needed for the index
(the keys of 4.2.4), for the directory records of section 6 and for transcoding (the Image Pixel
module). Display functions use the attributes of PS3.3 C.7.6.3, C.11 and the functional groups;
see Annex E.

### Annex B: SOP classes and transfer syntaxes

#### B.1 Storage SOP classes of the default list

Accepted by the Storage SCP and proposed by the C-GET SCU unless the product gives its own list
(or lets the SCP accept any SOP class). Sending (C-STORE SCU, C-MOVE / C-GET sub-operations) is
not limited to this list.

<!-- BEGIN GENERATED: storage-sop-classes -->
| # | SOP Class Name | SOP Class UID |
|---|---|---|
| 1 | Computed Radiography Image Storage | `1.2.840.10008.5.1.4.1.1.1` |
| 2 | Digital X-Ray Image Storage - For Presentation | `1.2.840.10008.5.1.4.1.1.1.1` |
| 3 | Digital X-Ray Image Storage - For Processing | `1.2.840.10008.5.1.4.1.1.1.1.1` |
| 4 | Digital Mammography X-Ray Image Storage - For Presentation | `1.2.840.10008.5.1.4.1.1.1.2` |
| 5 | Digital Mammography X-Ray Image Storage - For Processing | `1.2.840.10008.5.1.4.1.1.1.2.1` |
| 6 | CT Image Storage | `1.2.840.10008.5.1.4.1.1.2` |
| 7 | Enhanced CT Image Storage | `1.2.840.10008.5.1.4.1.1.2.1` |
| 8 | Ultrasound Multi-frame Image Storage | `1.2.840.10008.5.1.4.1.1.3.1` |
| 9 | MR Image Storage | `1.2.840.10008.5.1.4.1.1.4` |
| 10 | Enhanced MR Image Storage | `1.2.840.10008.5.1.4.1.1.4.1` |
| 11 | Ultrasound Image Storage | `1.2.840.10008.5.1.4.1.1.6.1` |
| 12 | Secondary Capture Image Storage | `1.2.840.10008.5.1.4.1.1.7` |
| 13 | Multi-frame Single Bit Secondary Capture Image Storage | `1.2.840.10008.5.1.4.1.1.7.1` |
| 14 | Multi-frame Grayscale Byte Secondary Capture Image Storage | `1.2.840.10008.5.1.4.1.1.7.2` |
| 15 | Multi-frame Grayscale Word Secondary Capture Image Storage | `1.2.840.10008.5.1.4.1.1.7.3` |
| 16 | Multi-frame True Color Secondary Capture Image Storage | `1.2.840.10008.5.1.4.1.1.7.4` |
| 17 | Grayscale Softcopy Presentation State Storage | `1.2.840.10008.5.1.4.1.1.11.1` |
| 18 | X-Ray Angiographic Image Storage | `1.2.840.10008.5.1.4.1.1.12.1` |
| 19 | X-Ray Radiofluoroscopic Image Storage | `1.2.840.10008.5.1.4.1.1.12.2` |
| 20 | Nuclear Medicine Image Storage | `1.2.840.10008.5.1.4.1.1.20` |
| 21 | Raw Data Storage | `1.2.840.10008.5.1.4.1.1.66` |
| 22 | Segmentation Storage | `1.2.840.10008.5.1.4.1.1.66.4` |
| 23 | VL Whole Slide Microscopy Image Storage | `1.2.840.10008.5.1.4.1.1.77.1.6` |
| 24 | Basic Text SR Storage | `1.2.840.10008.5.1.4.1.1.88.11` |
| 25 | Enhanced SR Storage | `1.2.840.10008.5.1.4.1.1.88.22` |
| 26 | Comprehensive SR Storage | `1.2.840.10008.5.1.4.1.1.88.33` |
| 27 | Encapsulated PDF Storage | `1.2.840.10008.5.1.4.1.1.104.1` |
| 28 | Positron Emission Tomography Image Storage | `1.2.840.10008.5.1.4.1.1.128` |
| 29 | Enhanced PET Image Storage | `1.2.840.10008.5.1.4.1.1.130` |
| 30 | RT Image Storage | `1.2.840.10008.5.1.4.1.1.481.1` |
| 31 | RT Dose Storage | `1.2.840.10008.5.1.4.1.1.481.2` |
| 32 | RT Structure Set Storage | `1.2.840.10008.5.1.4.1.1.481.3` |
| 33 | RT Beams Treatment Record Storage | `1.2.840.10008.5.1.4.1.1.481.4` |
| 34 | RT Plan Storage | `1.2.840.10008.5.1.4.1.1.481.5` |
| 35 | RT Ion Plan Storage | `1.2.840.10008.5.1.4.1.1.481.8` |

35 storage SOP classes in the default list (`net::storage_sop_classes()`, src/net/dimse.cpp).
<!-- END GENERATED: storage-sop-classes -->

#### B.2 Transfer syntaxes

"Data set read / written": files and network data sets in this transfer syntax are parsed and
written, the pixel data carried as it is. "Pixel data decoded": frames can be decoded for
display, measurement and transcoding. "Pixel data encoded": the transcoder writes it.

<!-- BEGIN GENERATED: transfer-syntaxes -->
| Transfer Syntax | UID | Data set read / written | Pixel data decoded | Pixel data encoded (transcode target) | Notes |
|---|---|---|---|---|---|
| Implicit VR Little Endian | `1.2.840.10008.1.2` | yes | yes | yes |  |
| Explicit VR Little Endian | `1.2.840.10008.1.2.1` | yes | yes | yes |  |
| Encapsulated Uncompressed Explicit VR Little Endian | `1.2.840.10008.1.2.1.98` | yes | yes | yes |  |
| Deflated Explicit VR Little Endian | `1.2.840.10008.1.2.1.99` | yes | yes | yes |  |
| Explicit VR Big Endian | `1.2.840.10008.1.2.2` | yes | yes | yes | retired |
| JPEG Baseline (Process 1) | `1.2.840.10008.1.2.4.50` | yes | yes | yes | lossy |
| JPEG Extended (Process 2 & 4) | `1.2.840.10008.1.2.4.51` | yes | yes | yes | lossy |
| JPEG Extended (Process 3 & 5) | `1.2.840.10008.1.2.4.52` | yes | yes | no | retired |
| JPEG Spectral Selection, Non-Hierarchical (Process 6 & 8) | `1.2.840.10008.1.2.4.53` | yes | yes | no | retired |
| JPEG Spectral Selection, Non-Hierarchical (Process 7 & 9) | `1.2.840.10008.1.2.4.54` | yes | yes | no | retired |
| JPEG Full Progression, Non-Hierarchical (Process 10 & 12) | `1.2.840.10008.1.2.4.55` | yes | yes | no | retired |
| JPEG Full Progression, Non-Hierarchical (Process 11 & 13) | `1.2.840.10008.1.2.4.56` | yes | yes | no | retired |
| JPEG Lossless, Non-Hierarchical (Process 14) | `1.2.840.10008.1.2.4.57` | yes | yes | yes |  |
| JPEG Lossless, Non-Hierarchical (Process 15) | `1.2.840.10008.1.2.4.58` | yes | yes | no | retired |
| JPEG Lossless, Non-Hierarchical, First-Order Prediction (Process 14 [Selection Value 1]) | `1.2.840.10008.1.2.4.70` | yes | yes | yes |  |
| JPEG-LS Lossless Image Compression | `1.2.840.10008.1.2.4.80` | yes | yes | yes |  |
| JPEG-LS Lossy (Near-Lossless) Image Compression | `1.2.840.10008.1.2.4.81` | yes | yes | yes | written losslessly unless lossy coding is requested |
| JPEG 2000 Image Compression (Lossless Only) | `1.2.840.10008.1.2.4.90` | yes | yes | yes |  |
| JPEG 2000 Image Compression | `1.2.840.10008.1.2.4.91` | yes | yes | yes | written losslessly unless lossy coding is requested |
| JPEG 2000 Part 2 Multi-component Image Compression (Lossless Only) | `1.2.840.10008.1.2.4.92` | yes | yes | no |  |
| JPEG 2000 Part 2 Multi-component Image Compression | `1.2.840.10008.1.2.4.93` | yes | yes | no |  |
| JPIP Referenced | `1.2.840.10008.1.2.4.94` | yes | no | no |  |
| JPIP Referenced Deflate | `1.2.840.10008.1.2.4.95` | yes | no | no |  |
| MPEG2 Main Profile / Main Level | `1.2.840.10008.1.2.4.100` | yes | no | no | pixel data carried as it is (no codec) |
| Fragmentable MPEG2 Main Profile / Main Level | `1.2.840.10008.1.2.4.100.1` | yes | no | no | pixel data carried as it is (no codec) |
| MPEG2 Main Profile / High Level | `1.2.840.10008.1.2.4.101` | yes | no | no | pixel data carried as it is (no codec) |
| Fragmentable MPEG2 Main Profile / High Level | `1.2.840.10008.1.2.4.101.1` | yes | no | no | pixel data carried as it is (no codec) |
| MPEG-4 AVC/H.264 High Profile / Level 4.1 | `1.2.840.10008.1.2.4.102` | yes | no | no | pixel data carried as it is (no codec) |
| Fragmentable MPEG-4 AVC/H.264 High Profile / Level 4.1 | `1.2.840.10008.1.2.4.102.1` | yes | no | no | pixel data carried as it is (no codec) |
| MPEG-4 AVC/H.264 BD-compatible High Profile / Level 4.1 | `1.2.840.10008.1.2.4.103` | yes | no | no | pixel data carried as it is (no codec) |
| Fragmentable MPEG-4 AVC/H.264 BD-compatible High Profile / Level 4.1 | `1.2.840.10008.1.2.4.103.1` | yes | no | no | pixel data carried as it is (no codec) |
| MPEG-4 AVC/H.264 High Profile / Level 4.2 For 2D Video | `1.2.840.10008.1.2.4.104` | yes | no | no | pixel data carried as it is (no codec) |
| Fragmentable MPEG-4 AVC/H.264 High Profile / Level 4.2 For 2D Video | `1.2.840.10008.1.2.4.104.1` | yes | no | no | pixel data carried as it is (no codec) |
| MPEG-4 AVC/H.264 High Profile / Level 4.2 For 3D Video | `1.2.840.10008.1.2.4.105` | yes | no | no | pixel data carried as it is (no codec) |
| Fragmentable MPEG-4 AVC/H.264 High Profile / Level 4.2 For 3D Video | `1.2.840.10008.1.2.4.105.1` | yes | no | no | pixel data carried as it is (no codec) |
| MPEG-4 AVC/H.264 Stereo High Profile / Level 4.2 | `1.2.840.10008.1.2.4.106` | yes | no | no | pixel data carried as it is (no codec) |
| Fragmentable MPEG-4 AVC/H.264 Stereo High Profile / Level 4.2 | `1.2.840.10008.1.2.4.106.1` | yes | no | no | pixel data carried as it is (no codec) |
| HEVC/H.265 Main Profile / Level 5.1 | `1.2.840.10008.1.2.4.107` | yes | no | no | pixel data carried as it is (no codec) |
| HEVC/H.265 Main 10 Profile / Level 5.1 | `1.2.840.10008.1.2.4.108` | yes | no | no | pixel data carried as it is (no codec) |
| JPEG XL Lossless | `1.2.840.10008.1.2.4.110` | yes | no | no | pixel data carried as it is (no codec) |
| JPEG XL JPEG Recompression | `1.2.840.10008.1.2.4.111` | yes | no | no | pixel data carried as it is (no codec) |
| JPEG XL | `1.2.840.10008.1.2.4.112` | yes | no | no | pixel data carried as it is (no codec) |
| High-Throughput JPEG 2000 Image Compression (Lossless Only) | `1.2.840.10008.1.2.4.201` | yes | yes | yes |  |
| High-Throughput JPEG 2000 with RPCL Options Image Compression (Lossless Only) | `1.2.840.10008.1.2.4.202` | yes | yes | yes |  |
| High-Throughput JPEG 2000 Image Compression | `1.2.840.10008.1.2.4.203` | yes | yes | yes | written losslessly unless lossy coding is requested |
| JPIP HTJ2K Referenced | `1.2.840.10008.1.2.4.204` | yes | no | no |  |
| JPIP HTJ2K Referenced Deflate | `1.2.840.10008.1.2.4.205` | yes | no | no |  |
| RLE Lossless | `1.2.840.10008.1.2.5` | yes | yes | yes |  |
| Deflated Image Frame Compression | `1.2.840.10008.1.2.8.1` | yes | yes | no |  |
| GE Private Implicit VR Little Endian with Big Endian Pixel Data | `1.2.840.113619.5.2` | yes | yes | no | private transfer syntax of another vendor (GE): read only |
| SMPTE ST 2110-20 Uncompressed Progressive Active Video | `1.2.840.10008.1.2.7.1` | yes | no | no |  |
| SMPTE ST 2110-20 Uncompressed Interlaced Active Video | `1.2.840.10008.1.2.7.2` | yes | no | no |  |
| SMPTE ST 2110-30 PCM Digital Audio | `1.2.840.10008.1.2.7.3` | yes | no | no |  |
| JPEG Extended, Hierarchical (Process 16 & 18) | `1.2.840.10008.1.2.4.59` | no | no | no | retired; named only: not read or written |
| JPEG Extended, Hierarchical (Process 17 & 19) | `1.2.840.10008.1.2.4.60` | no | no | no | retired; named only: not read or written |
| JPEG Spectral Selection, Hierarchical (Process 20 & 22) | `1.2.840.10008.1.2.4.61` | no | no | no | retired; named only: not read or written |
| JPEG Spectral Selection, Hierarchical (Process 21 & 23) | `1.2.840.10008.1.2.4.62` | no | no | no | retired; named only: not read or written |
| JPEG Full Progression, Hierarchical (Process 24 & 26) | `1.2.840.10008.1.2.4.63` | no | no | no | retired; named only: not read or written |
| JPEG Full Progression, Hierarchical (Process 25 & 27) | `1.2.840.10008.1.2.4.64` | no | no | no | retired; named only: not read or written |
| JPEG Lossless, Hierarchical (Process 28) | `1.2.840.10008.1.2.4.65` | no | no | no | retired; named only: not read or written |
| JPEG Lossless, Hierarchical (Process 29) | `1.2.840.10008.1.2.4.66` | no | no | no | retired; named only: not read or written |
| RFC 2557 MIME encapsulation | `1.2.840.10008.1.2.6.1` | no | no | no | retired; named only: not read or written |
| XML Encoding | `1.2.840.10008.1.2.6.2` | no | no | no | retired; named only: not read or written |
| Papyrus 3 Implicit VR Little Endian | `1.2.840.10008.1.20` | no | no | no | retired; named only: not read or written |

64 transfer syntaxes in the registry: 53 whose data sets are read and written, 27 whose pixel data is decoded, 17 that the transcoder writes.
<!-- END GENERATED: transfer-syntaxes -->

Not included in this release: the video codecs (MPEG-2, MPEG-4 AVC / H.264, HEVC / H.265) and
JPEG XL - instances in these transfer syntaxes are read, stored, sent and written with their
pixel data untouched, but not decoded or encoded; JPIP and SMPTE ST 2110 referenced pixel data
is not fetched.

### Annex C: Data dictionary of private attributes

**The toolkit creates no private attributes and reserves no private creator.** (Verified in the
source at this release: no code path writes an element with an odd group number of the toolkit's
own, and no private creator string is defined by the toolkit.)

Private attributes of other implementations are preserved when instances are read, stored,
forwarded and transcoded. The toolkit contains a dictionary of private data elements of several
vendors for interpreting them (names, VRs, VMs), built only from vendor DICOM Conformance
Statements; see docs/PRIVATE_DICTIONARY.md. De-identification removes private attributes unless
the product selects the options that retain them.

### Annex D: Coded terminology and templates

| Use | Codes |
|---|---|
| Lossy transcode (Annex A.2) | (113040, DCM, "Lossy Compression") from CID 7203; (121320, DCM, "Uncompressed predecessor") and (121330, DCM, "Lossy compressed predecessor") from CID 7202 |
| De-identification (Annex A.3) | CID 7050 De-identification Method: (113100, DCM) Basic Application Confidentiality Profile, and the option codes 113101 to 113112 as applied |

Structured Reports, Segmentations, Presentation States, RT Structure Sets and Key Object
Selection documents are *read* (content trees, segments, contours, references); the context
groups of PS3.16 are available for lookup. Templates of created documents are the product's.

### Annex E: Grayscale image consistency and colour

The toolkit renders to 8-bit pixel buffers for the product to display; it does not drive a
display and applies no display calibration. The rendering pipeline applies, in the order of the
standard, Modality LUT or rescale, VOI LUT or window (linear, linear exact, sigmoid), Presentation
LUT shape, and for colour Palette Color LUTs and YBR to RGB conversion. **Conformance to the
Grayscale Standard Display Function (PS3.14) is the product's responsibility** (display
calibration and its P-value mapping). ICC profiles are carried, not applied.

### Annex F: Extensions, specializations, privatizations

- Standard extended, specialized or private SOP classes: **none**.
- Private transfer syntaxes: **none** created or proposed. One private transfer syntax of
  another vendor (GE, `1.2.840.113619.5.2`) is recognised when reading files.
- Private attributes: none (Annex C).

### Annex G: Not supported in this release

- Print Management in the C API and C# bindings (either role).
- Video codecs (MPEG-2, H.264, HEVC) and JPEG XL: carried, not decoded or encoded.
- DICOMweb rendered resources, thumbnails, UPS-RS, capabilities, STOW-RS with metadata + bulk data.
- User Identity negotiation and changing the Implementation Class UID / Version Name through
  the C API.
- Patient/Study Only Query/Retrieve; Storage Commitment Pull Model (retired); media security
  envelopes; Detached and other retired management services.
- Calculations on images beyond stored / modality values and geometry (SUV, ROI statistics) are
  intentionally left to the product.
