# SIAARA networking: RIS / PACS integration guide

Everything the toolkit does on the network is reachable through the C API
(`include/siaara/siaara.h`) and the C# bindings (`Siaara.Network`, `Siaara.Web`), as service
class user (SCU, the side that asks) and as service class provider (SCP, the side that answers).
This guide is for integrators: what each service does, how to configure it, how the pieces fit a
RIS / PACS / modality workflow, and what to put in the DICOM Conformance Statement (last section).

| Service (PS3.4) | SOP Class UID | SCU (C API / C#) | SCP (`siaara_scp_start_ex` / `StorageScp.Start`) |
|---|---|---|---|
| Verification | 1.2.840.10008.1.1 | `siaara_dimse_echo` / `Dimse.Echo` | always on |
| Storage (all storage SOP classes) | toolkit list, or yours | `siaara_dimse_store(_ex)`, `siaara_dimse_store_files(_ex)` / `Dimse.Store`, `Dimse.StoreFiles` | `storage` |
| Q/R Patient Root FIND / MOVE / GET | 1.2.840.10008.5.1.4.1.2.1.1/.2/.3 | `siaara_dimse_find(_ex)` / `move(_ex)` / `get(_ex)` / `Dimse.Find`, `Move`, `Get` | `find`, `move`, `get` (from the store) |
| Q/R Study Root FIND / MOVE / GET | 1.2.840.10008.5.1.4.1.2.2.1/.2/.3 | same | same |
| Composite Instance Root Retrieve MOVE / GET, Retrieve Without Bulk Data GET | 1.2.840.10008.5.1.4.1.2.4.2/.3, 1.2.840.10008.5.1.4.1.2.5.3 | C++ `net::Scu` (not in the C API) | `move`, `get` |
| Modality Worklist | 1.2.840.10008.5.1.4.31 | `siaara_dimse_find(SIAARA_QR_WORKLIST)` + `siaara_worklist_query` | `worklist` (callback or list) |
| Modality Performed Procedure Step | 1.2.840.10008.3.1.2.3.3 | `siaara_dimse_mpps_create` / `_set` / `Dimse.MppsCreate`, `MppsSet` | `mpps` |
| Storage Commitment Push Model | 1.2.840.10008.1.20.1 | `siaara_dimse_commit` (same association), `siaara_dimse_commit_ex` (new association) / `Dimse.Commit` | `commitment` (SCP), `commitment_reports` (SCU role receiver) |
| Unified Procedure Step Push / Pull / Watch / Query / Event | 1.2.840.10008.5.1.4.34.6.1-.5 | `siaara_dimse_n_request`, `siaara_dimse_find(SIAARA_QR_UPS)` / `Dimse.SendNRequest` | `ups` (in memory) |
| Instance Availability Notification | 1.2.840.10008.5.1.4.33 | `siaara_dimse_n_request` | `instance_availability` (events) |
| DICOMweb QIDO-RS / WADO-RS / WADO-URI / STOW-RS (PS3.18) | - | `siaara_web_*` / `DicomWebClient` | `siaara_web_server_start` / `DicomWebServer` |

Not offered: Print Management SCP (the C++ layer has a Print SCU over one association; it is not
in the C API because the per-call association model does not fit a print session), UPS-RS and
rendered DICOMweb resources (answer 406).

## 1. Quick start

C - an archive with Storage, Q/R from its store, Worklist and MPPS on port 11112:

```c
siaara_store* store;  siaara_store_open("/var/lib/pacs", 13, NULL, &store);
const char* peers[] = {"CT01", "RIS", "VIEWER"};
siaara_scp_options o = {sizeof o};
o.port = 11112; o.ae_title = "ARCHIVE";
o.allowed_calling_aes = peers; o.allowed_count = 3;
siaara_ae_entry viewer = {sizeof viewer, 104, "VIEWER", "10.0.0.20", 0};
siaara_scp_services s = {sizeof s};
s.storage = s.find = s.move = s.get = s.worklist = s.mpps = s.commitment = 1;
s.ae_table = &viewer; s.ae_count = 1;
siaara_scp* scp;
siaara_scp_start_ex(&o, &s, store, on_event, NULL, &scp);
siaara_scp_set_worklist_json(scp, json, json_len);   /* from the RIS */
```

C#:

```csharp
using var store = DicomStore.Open("/var/lib/pacs");
using var scp = StorageScp.Start(
    new StorageScpOptions { Port = 11112, AeTitle = "ARCHIVE", AllowedCallingAeTitles = new[] { "CT01", "RIS", "VIEWER" } },
    new ScpServiceOptions { Find = true, Move = true, Get = true, Worklist = true, Mpps = true, Commitment = true,
                            AeTable = new[] { new ScpAeEntry("VIEWER", "10.0.0.20", 104) },
                            WorklistProvider = q => ris.ScheduledSteps() },
    store);
scp.Received += (_, e) => Console.WriteLine($"{e.Kind} {e.CallingAeTitle} {e.DimseStatus:X4}");
```

## 2. The SCP

### 2.1 Listening, peers and limits (`siaara_scp_options` / `StorageScpOptions`)

| Setting | Default | Notes |
|---|---|---|
| `ae_title`, `port`, `bind_address` | SIAARA_SCP, ephemeral, all | `siaara_scp_port` returns the bound port |
| `allowed_calling_aes` | required | an empty list refuses to start unless `allow_any_calling_ae` (NET-05 / R-017) |
| `accept_any_called_ae` | 0 | associations addressed to another AE title are rejected |
| `max_associations` | 8 (at most 64) | also the worker pool size; per peer address 4 (R-016; IPv6 grouped by /64) |
| lifetime / operations per association | 2 h / 100000 | then A-ABORT |
| `idle_timeout_ms`, `dimse_timeout_ms`, `max_pdu` | 300 s, 60 s, 65536 | |
| `tls` | none | every connection on the port uses TLS (PS3.15 B.12 / B.13, BCP 195); run a second SCP for plain DIMSE |
| `strict_store_uids` | 0 | 1: C-STORE whose command UIDs differ from the data set's is refused 0xA900 (R-073) |
| `spool_*` | 16 MiB threshold, 64 MiB free-space floor | large data sets are received to an owner-only spool file (R-074) |
| `event_queue_limit` | 1024 | see 2.4 |
| `event_queue_max_bytes` | 64 MiB | data sets held by queued events; over it events are dropped as for a full queue (R-109) |
| `incomplete_policy` | `SIAARA_INCOMPLETE_DEFAULT` = QUARANTINE | a received data set that was not read completely; see 2.3b |
| port taken | | `siaara_scp_start(_ex)` and `siaara_web_server_start` return `SIAARA_ERR_ADDRESS_IN_USE` (16) with the port in the message |

### 2.2 Services (`siaara_scp_services` / `ScpServiceOptions`)

- **Storage**: `storage_sop_classes` (NULL: the toolkit's list, or any with `accept_any_sop_class`),
  `transfer_syntaxes` in preference order (NULL: native, plus compressed with `accept_compressed`),
  `store_transfer_syntax` to convert on arrival (lossy only with `allow_lossy`). Duplicates follow
  `duplicate_policy`. Store quota: `siaara_store_options.max_total_bytes` (0xA700 when full).
- **Query/Retrieve from the store**: Patient Root and Study Root, levels PATIENT / STUDY / SERIES /
  IMAGE, single values, UID lists, wildcards, date / time ranges, sequence matching, and the
  hierarchical key rules of PS3.4 C.4.1.2.1 unless the requestor negotiated relational queries
  (`relational`, `fuzzy_person_names`, `combined_datetime` enable the extended negotiation the SCP
  accepts). Optional keys the store cannot match on are treated as universal and returned empty.
  Responses are UTF-8 (`ISO_IR 192`). Every response carries Retrieve AE Title (0008,0054) -
  the SCP's own AE title - whether or not the identifier asked for it (PS3.4 C.4.1.1.3.2: a
  response holds Retrieve AE Title or the Storage Media File-Set ID / UID, and this SCP has no
  file-sets); Instance Availability (ONLINE) is returned when requested.
  `max_find_results` (10000; then 0xA700) and `max_retrieve_instances` (65535; then 0xA701).
  The index is read page by page and the walk ends at the limit (R-115); single-value Patient ID,
  Accession Number and Study Date keys (wild cards and ranges included) are matched by the
  store's indexes. A list-of-UIDs key may hold at most 10,000 values (more: 0xA700; R-128).
- **C-MOVE destinations** come from `ae_table` (`siaara_ae_entry`: AE title, host, port, `tls`).
  An unknown destination gets 0xA801. Entries with `tls = 1` use `outbound_tls`; with `tls = 1`
  and no `outbound_tls` the default TLS client settings apply (system trust store, host name
  check), so an association to a peer with a private certificate fails closed (R-153).
  `move_acls` (`siaara_move_acl`; C# `MoveAcls`) lists the destinations each calling AE may name;
  a caller without an entry may name any `ae_table` entry, and the SCP logs a warning at start
  when C-MOVE runs without them (R-129). The sub-association proposes, per SOP class, the stored
  transfer syntax, the native syntaxes (Implicit and Explicit VR Little Endian, also for
  compressed instances, which are then decompressed for a destination that accepts only those;
  T3.1) and the compressed syntaxes the transcoder writes.
  C-GET sends on the requestor's association (it must propose the storage SOP classes with the
  SCP role); the SCP accepts for those contexts every transfer syntax it can write (native first,
  lossy last and only with `allow_lossy`) and transcodes per instance.
- **Modality Worklist**: either `worklist_cb` (called per query; emit
  every candidate, the toolkit applies PS3.4 K.6 matching and returns only the requested keys)
  or the list set with `siaara_scp_set_worklist` / `siaara_scp_set_worklist_json` (DICOM JSON).
  C#: `WorklistProvider` or `SetWorklist`. The callback runs on a helper thread while the
  association thread waits for it (R-130): when it neither returns nor emits an item for
  `worklist_timeout_ms` (30 s), or the SCP is stopped, the C-FIND ends with 0x0110, `emit()`
  tells the callback to stop, and `siaara_scp_stop` does not wait for it. While four such calls
  are still running, further worklist queries are answered 0x0110 without calling the callback.
  `worklist_user` must stay valid until every callback has returned.
- **MPPS**: N-CREATE (status must be IN PROGRESS; duplicates 0x0111), N-SET until COMPLETED or
  DISCONTINUED (then 0x0110), per PS3.4 F.7.2.2. `siaara_scp_mpps_get` returns the current state;
  events `SIAARA_SCP_MPPS_CREATE` / `_SET` carry the data set (forward it to the RIS).
  Bounds (R-109, R-119): `max_mpps_instances` (10000) counts only IN PROGRESS instances;
  finished ones are kept up to `max_finished_instances` (10000) and `max_state_bytes` (256 MiB),
  the oldest evicted first; beyond the bounds a request gets 0xA700 and nothing changes.
  `bind_updates_to_creator = 1` accepts N-SET only from the AE that sent the N-CREATE (others
  0x0124); it is off by default because PS3.4 F.7.2 does not require it (R-131).
- **State folder** (`state_folder`; C# `StateFolder`): without it MPPS instances and UPS
  workitems live in memory and are gone after a restart. With it each one is kept in an
  owner-only file (`<folder>/mpps/<UID>.sst`, `<folder>/ups/<UID>.sst`; created exclusively, mode
  0600 / owner-only DACL, replaced atomically) and reloaded by the next `siaara_scp_start_ex`;
  UPS subscriptions are kept too. With a state folder a SOP Instance UID must be a valid UID
  (digits and dots, at most 64 characters; else 0x0106). Files that cannot be read are skipped,
  counted in a warning, and never deleted.
- **Storage Commitment**: each referenced instance is committed when the store holds it with the
  same SOP class (else Failure Reason 0x0112 No Such Object Instance, 0x0119 Class / Instance
  Conflict; more than `max_commitment_items` gives 0x0213 Resource Limitation). The report goes per
  `commitment_report_mode`: on the requestor's association, on a new association to the
  requestor's calling AE (looked up in `ae_table`, proposing the SCP role per PS3.4 J.3.3), or
  same-then-new (default). `commitment_reports = 1` makes the SCP also *receive* reports that a
  remote commitment SCP sends on a new association (for `siaara_dimse_commit_ex`). A report for
  a request that `siaara_dimse_commit_ex` is waiting for counts only when it comes from the AE
  the request was sent to and lists only requested instances; otherwise it is answered 0x0124 /
  0x0106, ignored and raised as `SIAARA_SCP_REFUSED` (R-118). Note that any calling AE allowed to
  use the commitment service learns from the answer which SOP Instance UIDs the store holds:
  restrict the service with an allow-list (R-153).
- **Event reports on new associations** (Storage Commitment results, UPS events) are sent by one
  sender thread per SCP with a bounded queue (256 jobs) and short timeouts (connect 5 s,
  association 5 s, DIMSE 10 s), so a receiver that does not answer never holds the requestor's
  association, and `siaara_scp_stop` aborts the association in progress (R-117). For a
  commitment report sent that way the `SIAARA_SCP_COMMIT_REQUEST` event (outcome 1 = delivered)
  is raised once the attempt has ended.
- **UPS**: Push / Pull / Watch / Query and Event reports to subscribers (receiving AEs from
  `ae_table`); `siaara_scp_ups_create` / `_get` for local scheduling. Per PS3.4 CC.2 the
  Requested / Affected SOP Class UID of the DIMSE-N commands is that of the UPS Push SOP Class
  (1.2.840.10008.5.1.4.34.6.1) on an association negotiated for any UPS SOP class; the SCP accepts
  that and the negotiated UID, and its N-EVENT-REPORTs name UPS Push on a context negotiated for
  UPS Event (T3.2). N-CREATE needs a context negotiated for UPS Push. Bounds: `max_ups_items`
  (10000 workitems not yet COMPLETED / CANCELED), `max_finished_instances`, `max_state_bytes`,
  `max_ups_subscribers` per workitem (16), 64 global subscribers, 10000 C-FIND responses; 0xA700
  beyond. Final workitems are evicted oldest first unless a subscriber holds a deletion lock.
- **Instance Availability Notification**: N-CREATE accepted, event `SIAARA_SCP_IAN` with the data set.
- **Allow-lists** (`acls`, `siaara_scp_acl` per `SIAARA_SVC_*`): a listed service is available only
  to those calling AEs (they must also pass `allowed_calling_aes`). Refusals answer 0x0124
  (Refused: Not Authorized), raise `SIAARA_SCP_REFUSED` and count in `refused`. A service that
  touches patient data and has no entry is open to every accepted peer: the SCP logs a warning
  naming those services when it starts (R-129).
- **Data set size**: data sets of every message other than C-STORE - N-service data sets, Q/R
  identifiers - may be at most `max_service_dataset_bytes` (4 MiB), checked while the fragments
  arrive; a larger one aborts the association (R-109). They are never spooled to disk.

### 2.3 Lossy transcoding and SOP Instance UIDs (R-093)

When an instance has to be coded lossily on the way (C-STORE SCU with `allow_lossy_transcode`,
C-GET / C-MOVE sub-operations, `store_transfer_syntax`), PS3.3 C.7.6.1.1.5 requires a new SOP
Instance UID. The toolkit uses that new UID consistently in the C-STORE command, the data set,
the file meta, the file name and the store index, and reports the mapping:
`siaara_uid_change_callback` (SCU), `SIAARA_SCP_UID_CHANGED` and
`siaara_scp_event.original_sop_instance_uid` (SCP), `UidChange` / `ScpEvent.OriginalSopInstanceUid`
(C#). A receiver with `strict_store_uids` therefore accepts lossy transfers.

The SCU learns per file what was sent: `siaara_dimse_store_files_report` / `siaara_dimse_store_report`
fill a `siaara_store_outcome` per file (SOP Class UID, the SOP Instance UID sent, the file's own
UID, the transfer syntax used, the DIMSE status, flags `SENT`, `UID_CHANGED`, `LOSSY`,
`INCOMPLETE_INPUT`, or the error when it was not sent). Its `sop_class_uid` / `sop_instance_uid`
go to `siaara_dimse_commit(_ex)` as they are (C#: `Dimse.StoreFilesWithOutcomes`,
`Dimse.StoreWithOutcome`, `StoreOutcome`). `siaara_dimse_peer.keep_uid_on_lossy = 1`
(`DimsePeer.KeepUidOnLossy`) keeps the file's UID instead, as `siaara_transcode_params` does.

**Lossy history lists.** Lossy Image Compression Ratio (0028,2112) and Method (0028,2114) are
parallel lists, one value per lossy step in the same order (PS3.3 C.7.6.1.1.5.1). Every lossy
transcode - `siaara_file_transcode(_ex)` and the network paths above, with or without
`keep_uid_on_lossy` - looks at how many values the source has in each and never guesses:

| Source lists | What is written | Reported as |
|---|---|---|
| Same number of values in both (or neither present) | one value appended to each | `SIAARA_LOSSY_HISTORY_ALIGNED` |
| One more Ratio than Method value, and the source pixel data is lossy coded in JPEG Baseline / Extended (.50 / .51: `ISO_10918_1`), JPEG-LS near-lossless (.81 with NEAR > 0: `ISO_14495_1`), JPEG 2000 (.91, irreversible: `ISO_15444_1`) or HTJ2K (.203, irreversible: `ISO_15444_15`) | the missing Method value of that earlier step is filled in with the term the source's own coding proves, then one value appended to each | `SIAARA_LOSSY_HISTORY_METHOD_FILLED` (`LOSSY_METHOD_FILLED`), with the term filled in |
| Anything else: the source is uncompressed or losslessly coded (also losslessly inside .81 / .91 / .203), two or more Method values are missing, or there are more Method than Ratio values | the Ratio of this step is appended; Method is left byte for byte as it was (the method of this step is in Derivation Description unless `keep_uid_on_lossy`) | `SIAARA_LOSSY_HISTORY_MISALIGNED` (`LOSSY_HISTORY_MISALIGNED`) |

A transcode that applies no lossy compression (including same-syntax pass-through) leaves both
attributes untouched. The report: `siaara_file_transcode_ex` fills a `siaara_transcode_report`
(`lossy_history`, `lossy_method_filled`, `lossy_history_note` with the counts found, what was
written and why; C#: `Transcoder.TranscodeWithReport`, `TranscodeReport.LossyHistory`); for
C-STORE the outcome flags `SIAARA_STORE_LOSSY_METHOD_FILLED` / `SIAARA_STORE_LOSSY_HISTORY_MISALIGNED`
(`StoreOutcome.LossyHistory`). Lossy conversions made by our SCP (`store_transfer_syntax`, C-GET /
C-MOVE sub-operations) follow the same rule and write the note (no UIDs, no patient data) to the
log (`siaara_set_log_callback`, INFO; C++ `ScpHandlers::log`).

### 2.3a Incomplete input is not sent (testing 10b P1)

A file whose read was not complete (truncated input, skipped bytes: `siaara_file_read_report`)
is refused by every C-STORE path - `siaara_dimse_store(_files)(_ex/_report)`, C++ `Scu::store`,
`store_many`, and the C-GET / C-MOVE sub-operations of our SCP - exactly as `siaara_file_save`
refuses it: the peer would store an instance that looks whole but holds less than the source.
It counts as failed (`SIAARA_ERR_REJECTED`, outcome flag `SIAARA_STORE_INCOMPLETE_INPUT`, a
message starting `INCOMPLETE_INPUT`); the association goes on with the other files. Opt in with
`siaara_dimse_peer.allow_incomplete = 1` (C#: `DimsePeer.AllowIncomplete`; C++:
`ScuConfig::allow_incomplete`, `ScpConfig::allow_incomplete` for sub-operations - from the C API
`siaara_scp_services.allow_incomplete`, C# `ScpServiceOptions.AllowIncomplete`;
`siaara-storescu --allow-incomplete`).

STOW-RS follows the same rule: `siaara_web_stow` refuses such a file before any request
(`SIAARA_ERR_REJECTED`, message starting `INCOMPLETE_INPUT`); `siaara_web_stow_ex` with
`siaara_web_stow_options.allow_incomplete = 1` sends what was read (C#:
`DicomWebClient.Store(file, study, allowIncomplete: true)`). The C++ `DicomwebClient::store` sends
Part 10 bytes as they are, so it first reads every part back and sends nothing when one cannot be
read or was not read completely (`StowOptions::allow_incomplete` skips the check;
`siaara-webcli stow --allow-incomplete`).

### 2.3b Incomplete data on the receiving side (R-116)

A data set that arrives truncated, with bytes the reader had to skip, or that only a recovery
heuristic could read is not something to store behind a plain Success. What happens is an
option, `incomplete_policy` (`SIAARA_INCOMPLETE_*`; C# `IncompletePolicy`), shared by the
Storage SCP (`siaara_scp_options`), C-GET (`siaara_get_options`), the STOW-RS server
(`siaara_web_server_options`) and `siaara_web_wado_retrieve_to_ex`:

| Policy | What is kept | Sender is told | Event / report |
|---|---|---|---|
| `QUARANTINE` (default) | the bytes exactly as received, in `<store>/quarantine`, owner-only, outside the index (no C-FIND / QIDO / C-GET / C-MOVE / WADO returns them) | the *kept* answer: WARNING 0xB007 by default (STOW-RS: Warning Reason 0xB007, no Retrieve URL) | `SIAARA_SCP_QUARANTINED` with `read_loss`; retrieve report `quarantined` (also counted in `failed`) |
| `REFUSE` | nothing | FAILURE 0xC000 (STOW-RS Failure Reason) | `SIAARA_SCP_STORE_FAILED` with `read_loss`; `incomplete_refused` |
| `REFUSE_KEEP_COPY` | a copy in `<store>/rejected` for diagnosis (best effort) | as REFUSE | as REFUSE |
| `ACCEPT_AS_RECEIVED` | the bytes exactly as received in the archive, indexed with `SIAARA_IDXF_INCOMPLETE`; never converted to `store_transfer_syntax` | the *kept* answer (STOW-RS: Warning Reason 0xB006, status 202) | `SIAARA_SCP_STORED` with `read_loss` != 0; `incomplete_accepted` |

Options around it (each default is one named constant in `src/core/incomplete_policy.hpp` or
`src/store/quarantine.hpp`):

| Option | Default | Choices |
|---|---|---|
| `incomplete_policy` (`kDefaultIncompletePolicy`) | QUARANTINE | the four policies above |
| `incomplete_overrides` (`siaara_incomplete_override`; C# `IncompletePolicyByCallingAe`) | none | a policy per calling AE (Storage SCP) |
| `quarantine_response` (`kDefaultQuarantineResponse`; C# `IncompleteAnswer`) | WARNING | WARNING, FAILURE, SUCCESS - the answer for instances that were kept (quarantined or accepted) |
| `incomplete_warning_status` (`kQuarantineWarningStatus`) | 0xB007 | any warning status (0xB000, 0xB006 ...) |
| `incomplete_failure_status` (`kIncompleteFailureStatus`) | 0xC000 | any failure status (0xA900 ...) |
| `siaara_store_options.quarantine_max_items` / `_max_bytes` | 10000 / 4 GiB | caps of the quarantine; `rejected_max_*` for the rejected area |
| `quarantine_when_full` / `rejected_when_full` (`kDefaultQuarantineEvictOldest`) | refuse (C-STORE 0xA700, STOW-RS Failure Reason 0xA700) | refuse, or evict the oldest items |
| Storage Commitment for flagged instances (`kCommitFlaggedIncomplete`, compile-time) | committed | quarantined and rejected copies are never committed (they are not in the archive: Failure Reason 0x0112) |
| network read recovery (`kRecoverOverlongLengthsNetwork`, compile-time) | off | the overlong-length recovery heuristic is not applied to data received over the network |

Quarantine items are identified by the SHA-256 of their bytes (the same bytes arriving again are
one item) and carry a small record: time, source, calling AE / peer address, SOP Class and
Instance UID when readable, size, read-loss bits and the first read warning - no other patient
data. `siaara_store_quarantine_count / _get / _bytes / _release / _export / _delete` (C#
`DicomStore.QuarantineList / QuarantineBytes / QuarantineRelease / QuarantineExport /
QuarantineDelete`) take the area (`SIAARA_AREA_QUARANTINE`, `SIAARA_AREA_REJECTED`). Release
stores the bytes unchanged in the archive, flagged `SIAARA_IDXF_INCOMPLETE`; an item without a
Part 10 preamble cannot be stored unchanged and stays (`SIAARA_ERR_UNSUPPORTED`).

Incomplete data is never transcoded and never rewritten, under any policy. A STOW-RS or WADO-RS
part without a Part 10 preamble that is incomplete is therefore never stored in the archive
(`ACCEPT_AS_RECEIVED` refuses it; `QUARANTINE` keeps its bytes); a complete one is stored as a
new Part 10 file and reported (STOW-RS Warning Reason 0xB000 with status 202; `rewritten` in the
retrieve report). Where a receiver has no quarantine area - a C-GET into a folder only, a C++
DICOMweb server over a custom store without `Store::quarantine` - `QUARANTINE` falls back to
`REFUSE`. The sender-side `allow_incomplete` settings of 2.3a are unchanged.

Not built yet (owner decision 2026-11-11, points E to G): a policy table keyed by the kind of
damage, repair-on-receipt, per-calling-AE overrides of the sub-options (only the policy itself
can be set per AE), and per-client policy for STOW-RS.

### 2.4 Events and the drop policy (R-094)

One dispatcher thread per SCP calls `siaara_scp_callback` in order, with no toolkit lock held.
Fields: kind, DIMSE status, outcome, calling AE, peer address, SOP class / instance, UID mismatch
and command UIDs, original UID, Transaction UID, destination AE, completed / failed / warning,
command field, a borrowed data set (copy it with `siaara_dataset_clone` to keep it),
`association_id` (unique among the associations of the process run, so the events of concurrent
associations can be told apart) and `read_loss` (2.3b). C#: `ScpEvent.AssociationId`, `ReadLoss`;
`ScpEvent.DatasetJson` is converted on first use, not for every event. When
`event_queue_limit` events wait, or the data sets of the waiting events pass
`event_queue_max_bytes`, an association waits at most 1 s for room, then the event is
dropped and counted (`siaara_scp_stats.events_dropped`, warning logged once). After
`siaara_scp_stop` nothing waits, so stopping never depends on the callback. The DIMSE operation
itself is never failed because of the callback.

### 2.5 Statistics

`siaara_scp_stats`: associations (active / handled / rejected), instances stored, store failures,
echoes, bytes, dropped events, finds, retrieves, sub-operations, MPPS, commitments, refusals.

## 3. The SCU

Every call opens one association, performs the operation and releases it; calls may run on many
threads. `siaara_dimse_peer` holds host, port, AE titles, TLS, timeouts, `max_pdu` and
`allow_lossy_transcode`. Return codes: `SIAARA_OK` for Success / Warning, `SIAARA_ERR_REJECTED`
with the DIMSE status in `result->status`, `SIAARA_ERR_CANCELLED` (token or callback),
`SIAARA_ERR_NETWORK` / `SIAARA_ERR_TIMEOUT`.

When no association was established, `result->connect_failure` (`SIAARA_CONNECT_*`; C#
`DimseResult.ConnectFailure`, `SiaaraException.ConnectFailure`) says at which step: host name not
resolved, connection refused, connect timeout, TLS failure, A-ASSOCIATE-RJ, no answer to the
association request, closed by the peer, cancelled. For an A-ASSOCIATE-RJ, `reject_result`,
`reject_source` and `reject_reason` hold the PS3.8 9.3.4 fields (source 1 / reason 7: called AE
title not recognized; reason 3: calling AE title not recognized) and the message says it in
words. These fields are written only when `struct_size` covers them. The cancel token ends the
TCP connect, the TLS handshake and the wait for the A-ASSOCIATE response at once (on macOS the
TLS connect itself runs to its timeout).

- **Identifiers**: `siaara_qr_identifier(level, keys)` builds a Q/R identifier with the level's
  return keys (UTF-8); `siaara_worklist_query` a worklist identifier. C#:
  `Dimse.CreateQueryIdentifier`, `Dimse.CreateWorklistQuery`.
- **Extended negotiation**: `siaara_qr_options.flags` (`SIAARA_QR_RELATIONAL`, `_COMBINED_DATETIME`,
  `_FUZZY_NAMES`, `_TIMEZONE`) with `siaara_dimse_find_ex` / `move_ex` / `get_ex`;
  `accepted_flags` tells what the peer agreed to. `max_matches` stops a C-FIND early (not an error).
- **C-STORE** proposes, per SOP class, the file's syntax, the native syntaxes, a context with
  every syntax the transcoder writes without loss, and with `allow_lossy_transcode` a context
  with the lossy ones (JPEG Baseline / Extended), so it reaches SCPs that accept only compressed
  data, a JPEG-Baseline-only archive included. The C++ `Scu::add_contexts_for_files`, the C-MOVE
  sub-association of our SCP and `siaara-storescu --allow-lossy` propose the same.
  `siaara_dimse_peer` fields after `reserved0` (`allow_incomplete`, `keep_uid_on_lossy`) are read
  only when `struct_size` covers them.
- **C-GET** options: store and / or folder, duplicate policy, native only, strict UIDs, SOP classes.
- **MPPS**: `siaara_dimse_mpps_create` (returns the UID) and `siaara_dimse_mpps_set`.
- **Storage Commitment**: `siaara_dimse_commit` waits on the same association;
  `siaara_dimse_commit_ex` with `receiver` (an SCP started with `commitment_reports = 1`, reachable
  by the commitment SCP under our calling AE title) also accepts the report on a new association,
  matched by Transaction UID, within `wait_ms`. `same_association_ms` bounds the wait on the
  first association (-1: release at once, as many archives expect).
- **Generic DIMSE-N**: `siaara_dimse_n_request` (N-CREATE / SET / GET / ACTION / DELETE /
  EVENT-REPORT) for UPS, Instance Availability and other N-services, with an optional Meta SOP
  Class context and SCP role selection. For UPS set `abstract_syntax` to the SOP class the
  association is for (UPS Pull, Watch ...) and `sop_class_uid` to UPS Push
  (1.2.840.10008.5.1.4.34.6.1), as PS3.4 CC.2 requires; the C++ helpers (`ups_get`, `ups_set`,
  `ups_change_state`, `ups_subscribe` ...) do so themselves.
- **C# `Dimse.StoreFilesWithOutcomes`**: a cancel throws `StoreCanceledException` (an
  `OperationCanceledException`) whose `Files` hold the outcome of every file sent before it.
- C#: every call exists synchronously (`Dimse.Echo`, `Find`, `Move`, `Get`, `Store`, `StoreFiles`,
  `Commit`, `MppsCreate`, `MppsSet`, `SendNRequest`) and as `...Async` on the thread pool;
  `Dimse.Find(..., Func<DicomDataset, bool> onMatch)` streams matches without copying them into
  a list.

## 4. DICOMweb

**Server** (`siaara_web_server_start` / `DicomWebServer.Start`): QIDO-RS at study, series and
instance level (any attribute, `includefield`, `fuzzymatching`, paging, `max_qido_results`),
WADO-RS (study / series / instance, metadata, frames, bulk data), WADO-URI and STOW-RS (unless
`read_only`), all over the store. Plain HTTP only on loopback; anywhere else TLS plus `authorize`
or client certificates (`require_client_certificate`), unless `allow_insecure_remote`. The
`authorize` callback sees method, target and the Authorization header for every request
(401 when it returns 0). CORS only for a named origin.

Retrieves (R-110): the instances under a study / series / instance are listed from the store's
index with their sizes; a request whose instances are larger in total than `max_response_bytes`
(1 GiB) is answered 413 before any file is opened. Otherwise the multipart reply is written part
by part with chunked transfer coding from memory-mapped files, one instance open at a time (an
instance that must be decompressed for the requested transfer syntax is converted in memory, one
at a time). Metadata is produced one mapped instance at a time and never reads pixel data. A
response must be written within `response_timeout_ms` (10 minutes) and the client must read at
least `min_response_rate` bytes per second (1024, averaged over 30 s); a slower client is
disconnected (R-120). QIDO-RS results carry Retrieve URL (0008,1190) for their level, built from
`public_base_url`, or from the request's Host header when none is set (T-P3-1). STOW-RS parts
follow `incomplete_policy` (2.3b). Freeing the store while a server or SCP still uses it is
deferred until that server is freed (R-150).

**Client**: QIDO, WADO (instance, metadata, frames, retrieve, URI), STOW, cancellation,
`siaara_web_client_set_limits` (read timeout, body limit and deadline, part count / size) and
`siaara_web_wado_retrieve_to`, which streams a study / series into a store or folder part by part
(memory about one part), with progress and cancellation. `siaara_web_wado_retrieve_to_ex` (C#
`RetrieveToWithReport`) adds the receive policy and a report: a part whose Study / Series / SOP
Instance UID is not the one requested is not kept (`allow_other_uids = 1` keeps it; R-152), an
incomplete part follows `incomplete_policy`, and a part the store refuses leaves no file in the
folder (unless the file existed before).

## 5. Integration recipes

1. **Modality worklist from the RIS**: run the SCP with `worklist = 1`; feed it either with a
   `worklist_cb` that reads the RIS's scheduled procedure steps, or by pushing the list with
   `siaara_scp_set_worklist_json` whenever the RIS changes it. Restrict it with an allow-list of
   modality AEs.
2. **MPPS to the RIS**: `mpps = 1`; forward `SIAARA_SCP_MPPS_CREATE` / `_SET` events (data set
   attached) to the RIS, or query `siaara_scp_mpps_get`.
3. **Modality → archive with commitment**: the modality stores, then calls
   `siaara_dimse_commit_ex` with its own receiver SCP; the archive answers per
   `commitment_report_mode`. Only delete local copies for instances with failure reason 0.
4. **Viewer pulls from the PACS**: `siaara_dimse_find_ex` (study list) then `siaara_dimse_get_ex`
   into the local store, or `siaara_dimse_move_ex` to the viewer's own Storage SCP (its AE must be
   in the PACS's AE table).
5. **Web viewers**: `siaara_web_server_start` over the same store the SCP fills; TLS and an
   `authorize` callback that checks your tokens.

## 6. Security checklist

- List the peers (`allowed_calling_aes`), keep `accept_any_called_ae = 0`, add per-service
  allow-lists for MPPS / Worklist / Commitment / Q/R as appropriate.
- Use TLS on any network you do not control (DIMSE and DICOMweb); `outbound_tls` for C-MOVE /
  report associations.
- Keep `strict_store_uids = 1` where every sender is conformant.
- Size `max_associations`, the spool threshold and the store quota for the disk you have.
- The store's folders and files are owner-only (R-092; `siaara_store_options.file_mode` to change).
  A store made by an older version, or copied, may have index and data folders that group or
  others can enter: on POSIX they are tightened on open when this user owns them (a warning
  `STORE_PERMISSIONS_TIGHTENED` is logged), or reported (`STORE_PERMISSIONS_WIDE`) when not, or
  with `keep_permissions = 1` (R-135).
- Windows: a file written by the toolkit (stored instances, received files, state files) is
  created with an owner-only DACL and renamed into place, so a file that *replaces* an existing
  one takes that owner-only DACL, not the DACL the replaced file or the folder had (R-151). Where
  other accounts must read the files, open the store with `file_mode` set (the folders and files
  then inherit the parent folder's permissions) or grant access on the folder afterwards. A
  replace that meets a sharing violation is retried for at most 1 s; a read-only target fails at
  once; the temporary file is removed whenever a write fails (R-133).
- Give C-MOVE callers a destination list (`move_acls`) and every PHI service an allow-list; the
  SCP warns at start about those that have none (R-129).
- Incomplete instances go to the quarantine by default (2.3b): review it (`siaara_store_quarantine_*`) and size its caps.
- Log callbacks never receive patient data unless `siaara_set_log_include_phi(1)`.

## 7. Verification

- Loopback (ours ↔ ours), every service: `tests/test_capi_net2.cpp`, `tests/test_capi_net.cpp`,
  `tests/test_net_*.cpp`, C# `Network2Tests`.
- Interoperability: `tests/crosscheck/capi_net_crosscheck.py BUILD_DIR` (DCMTK echoscu, storescu,
  findscu incl. worklist, movescu, getscu; pynetdicom worklist, MPPS, Storage Commitment, UPS,
  and our SCUs against pynetdicom SCPs), `tests/crosscheck/services_crosscheck.py`,
  `tests/crosscheck/net_crosscheck.py`, `tests/crosscheck/web_crosscheck.py`.

## 8. DICOM Conformance Statement: networking section outline (PS3.2)

Fill the bracketed parts for the product that embeds the toolkit; the toolkit's facts are given.

1. **Conformance Statement Overview**: table of SOP classes with User / Provider columns (from
   the table at the top of this guide, restricted to what the product enables).
2. **Table of Contents**.
3. **Introduction**: revision history, audience, remarks, terms and definitions, basics of DICOM
   communication, abbreviations, references (PS3.x edition [year]).
4. **Networking**
   1. *Implementation Model*: application data flow diagram ([product] AEs: e.g. STORE-SCP,
      QR-SCP, WORKLIST-SCP, MPPS-SCP, COMMIT-SCP, the SCU AEs); functional definition of each AE;
      sequencing of real-world activities (store → commit → MPPS).
   2. *AE Specifications*, per AE:
      - SOP classes (User / Provider) and the presentation contexts: abstract syntax, transfer
        syntaxes (native: Implicit VR LE, Explicit VR LE, Explicit VR BE; compressed: Deflated,
        RLE, JPEG Baseline / Extended / Lossless SV1 / Lossless, JPEG-LS lossless / near-lossless,
        JPEG 2000 lossless / any, HTJ2K lossless / RPCL / any), role, extended negotiation
        (Q/R: relational, combined date-time, fuzzy names; Storage SCP: none).
      - Association policies: General (Application Context Name 1.2.840.10008.3.1.1.1; maximum PDU
        size received 65536 by default, configurable); Number of Associations (acceptor: `max_associations`
        default 8, per peer 4; initiator: one per operation, several in parallel); Asynchronous
        nature (Asynchronous Operations Window accepted up to 16 outstanding, performed in order);
        Implementation Identifying Information (Implementation Class UID
        2.25.208936390756097257212108686309372694101, Version Name SIAARA_010, or the product's own).
      - Association Initiation Policy, per activity (C-STORE, C-FIND, C-MOVE, C-GET, MPPS, Storage
        Commitment N-ACTION, report N-EVENT-REPORT on a new association, C-MOVE sub-associations):
        description and sequencing, proposed presentation contexts, SOP specific conformance
        with the status codes the AE handles.
      - Association Acceptance Policy, per activity: accepted presentation contexts; SOP specific
        conformance: Storage (level 2 conformance [as configured], element coercion: none, SOP
        Instance UID changed on lossy compression as in 2.3; duplicates kept or replaced; status
        codes 0000, A700, A900, C000, 0124); Q/R (levels, supported keys per level - the key table
        in `src/capi/store_services.cpp` - matching types, UTF-8 responses, statuses 0000, FF00,
        FE00, A700, A701, A801, A900, B000, 0124); Worklist (keys, matching, statuses); MPPS
        (attributes stored, state rules, statuses 0000, 0106, 0110, 0111, 0112, 0124); Storage
        Commitment (commitment decision from the archive's store, Failure Reasons 0112 / 0119 /
        0213, report mode and timing, retry: none); UPS (states, subscription, events);
        Verification.
   3. *Network Interfaces*: physical network (TCP/IP, IPv4 and IPv6), additional protocols
      (DNS for host names), IPv6 support.
   4. *Configuration*: AE title / presentation address mapping (local AE title and port; the
      remote AE table for C-MOVE destinations and report receivers), parameters with defaults
      (timeouts: connect 15 s, ACSE 30 s, DIMSE 60 s, ARTIM 30 s, idle 300 s; limits from 2.1).
5. **Media Interchange** (if the product writes DICOMDIR media; see `siaara_dicomdir_write`).
6. **Support of Character Sets**: all ISO 2022 single-byte and multi-byte sets, UTF-8 (ISO_IR 192),
   GB18030, GBK; Q/R responses in ISO_IR 192; identifiers decoded from their own character set.
7. **Security**: Secure Transport Connection Profiles (TLS 1.2 / 1.3, BCP 195 non-downgrading /
   Modified BCP 195 RFC 8996; certificate validation; mutual TLS), user identity negotiation
   (if the product uses it), audit trail [product], the allow-lists of 2.2.
8. **Annexes**: IOD contents of created instances [product], data dictionary of private
   attributes (none from the toolkit), coded terminology and templates [product], grayscale
   image consistency [product], standard extended / specialized / private SOP classes (none),
   private transfer syntaxes (none).
