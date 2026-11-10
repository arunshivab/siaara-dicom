// SIAARA DICOM Toolkit - local instance store for viewers (no external database engine).
//
// ================================================================ design
// On-disk layout of a store rooted at R:
//
//   R/siaara-store.lock          lock file; held exclusively while a Store is open
//   R/index/snapshot.sxs         compacted snapshot of the whole index (replaced atomically)
//   R/index/journal.sxj          append-only write-ahead log of changes since the snapshot
//   R/data/ab/cd/<32 hex>.dcm    copied instances. The name is the first 128 bits of
//                                SHA-256(SOP Instance UID) in hex; ab and cd are its first two
//                                bytes (65,536 fan-out folders). Names never contain UIDs or
//                                patient data, and the path of an instance is a pure function of
//                                its SOP Instance UID.
//
// Instances can also be indexed in place ("referenced"): the file stays where it is and the
// index keeps its absolute path; such files are never modified or deleted by the store.
//
// Index format. Both index files are a 16-byte header (8-byte magic, u32 format version,
// u32 generation) followed by framed records: u32 payload length, u32 CRC-32 of type+payload,
// u8 type, payload. Integers are little endian, strings varint-length-prefixed UTF-8.
//  - journal records: 'A' (add or replace one instance: the full IndexRecord, its path and
//    flags) and 'D' (delete by SOP Instance UID). Applying a record sets the final state of one
//    SOP Instance UID, so replay is idempotent and order within one UID is all that matters.
//  - snapshot records: the hierarchy, written once per node: 'P' patient, 'S' study (patient
//    ordinal), 'E' series (study ordinal), 'I' instance (series ordinal), then 'Z' with the
//    counts. A patient's, study's or series' text is stored once, not per instance.
// At open the snapshot is loaded and the journal replayed into in-memory structures: hash maps
// from SOP Instance / Series / Study UID and patient key to nodes, and per-node child lists.
// Queries scan the (small) patient/study/series node sets with precomputed folded strings and
// numeric date keys, and expand to children; exact UID filters go through the hash maps.
//
// Crash safety. An add writes the instance to a uniquely named temporary file next to its final
// name (outside the index lock), renames it into place, then appends one journal record (and,
// with durable = true, fsyncs file, folder and journal before returning). A crash can leave:
//  - a torn journal tail: at open the journal is read record by record; the first record whose
//    frame is short or whose CRC fails ends the replay and the journal is truncated there
//    (Stats::recovered_bytes). Every acknowledged change before it is kept.
//  - an instance file without a journal record, or a temporary file: an "orphan". It was never
//    acknowledged; verify() reports it and verify(repair) deletes it. rebuild() re-indexes
//    every file under data/ instead.
// Compaction writes a new snapshot with generation g+1 to a temporary file, fsyncs it, renames it
// over the old one, then replaces the journal with an empty one of generation g+1. A journal
// whose generation is older than the snapshot's is already contained in it and is discarded, so
// a crash at any point of compaction loses nothing. A snapshot that fails its checks is an error
// (Errc::Malformed) unless StoreOptions::rebuild_if_corrupt, which re-indexes data/.
//
// Concurrency. One process opens a store at a time: the lock file is locked exclusively
// (flock / exclusive share mode, released by the OS when the process dies), and a process-wide
// registry refuses a second open in the same process. Inside the process any number of threads
// may call any function: a shared mutex lets queries run in parallel while a writer mutex
// serialises changes; parsing and copying instance data happen before the writer mutex is taken.
// Change callbacks run on the writing thread after all locks are released, so they may query or
// even change the store.
//
// Search normalisation (store/text.hpp): names are split into their alphabetic, ideographic and
// phonetic groups, '^' becomes a space, then each group is case-folded with Latin accents
// stripped (CJK kept as-is); a name filter matches when it matches any group. Descriptions are
// folded the same way. IDs, accession numbers and modalities match case-sensitively, as in
// DICOM C-FIND. All text filters accept the C-FIND wildcards '*' and '?'; dates accept ranges.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"
#include "store/index.hpp"

namespace siaara::store {

struct StoreOptions {
    bool create = true;                     // create the folder layout when missing
    bool durable = false;                   // fsync data, folders and journal on every change
    bool rebuild_if_corrupt = false;        // re-index data/ when the snapshot is unreadable
    std::uint64_t compact_min_records = 50'000;  // compact when the journal has more records
    double compact_ratio = 0.5;             //   than max(min, ratio * instances)
    // Disk limits for copied instances (R-074): adding fails with Errc::LimitExceeded (a DIMSE
    // receiver answers 0xA700) when the files would pass max_total_bytes (0 = no cap) or leave less
    // than min_free_disk_bytes free on the volume (0 = no floor).
    std::uint64_t max_total_bytes = 0;
    std::uint64_t min_free_disk_bytes = 64ull << 20;
    // Permissions of the folders and files the store creates (R-092). 0: owner only (folders 0700,
    // files 0600; an owner-only DACL on Windows). Otherwise the POSIX file mode (folders get the
    // matching search bits too; Windows: inherited permissions).
    std::uint32_t file_mode = 0;
};

enum class DuplicatePolicy { Keep, Replace };
enum class AddMode { Copy, Reference };
enum class AddOutcome { Added, Replaced, KeptExisting };

struct QueryFilter {
    Level level = Level::Study;
    // empty = no filter; text filters accept '*' and '?'
    std::string patient_name;       // folded; matches any component group
    std::string patient_id;
    std::string patient_birth_date; // date or range
    std::string patient_sex;
    std::string study_date;         // date or range "YYYYMMDD-YYYYMMDD", "-YYYYMMDD", "YYYYMMDD-"
    std::string accession_number;
    std::string study_description;  // folded
    std::string referring_physician;// folded
    std::string modality;           // one or more, backslash separated; matches modalities in study
    std::string series_description; // folded
    std::string body_part;
    std::string study_uid;          // exact
    std::string series_uid;         // exact
    std::string sop_instance_uid;   // exact
    std::size_t offset = 0;
    std::size_t limit = 0;          // 0 = all
    // false: the query may stop as soon as offset + limit rows are known (fast paging over large
    // stores); QueryResult::total is then only a lower bound when total_is_exact is false.
    bool want_total = true;
};

struct QueryResult {
    std::size_t total = 0;          // matches before offset/limit
    bool total_is_exact = true;
    std::vector<IndexRecord> rows;
};

enum class EventKind { Added = 0, Replaced = 1, Deleted = 2, Rebuilt = 3 };
struct Event {
    EventKind kind = EventKind::Added;
    std::string sop_instance_uid, series_uid, study_uid;
};
using EventCallback = std::function<void(const Event&)>;

struct VerifyReport {
    std::size_t checked = 0;
    std::size_t missing = 0;          // indexed, file absent
    std::size_t size_mismatch = 0;    // indexed size differs from the file
    std::size_t content_mismatch = 0; // deep: file no longer holds the indexed SOP Instance UID
    std::size_t orphans = 0;          // files under data/ the index does not know (incl. temps)
    std::size_t repaired = 0;         // index entries removed + orphan files deleted
    std::vector<std::string> missing_uids;    // SOP Instance UIDs
    std::vector<std::string> orphan_paths;    // relative to the store root
    bool ok() const noexcept { return missing == 0 && size_mismatch == 0 && content_mismatch == 0 && orphans == 0; }
};

struct StoreStats {
    std::uint64_t patients = 0, studies = 0, series = 0, instances = 0;
    std::uint64_t data_bytes = 0;       // sum of indexed file sizes
    std::uint64_t journal_bytes = 0, journal_records = 0, snapshot_bytes = 0;
    std::uint64_t generation = 0;
    std::uint64_t replayed_records = 0; // journal records applied at open
    std::uint64_t recovered_bytes = 0;  // torn journal tail cut at open
};

class Store {
public:
    static Result<std::unique_ptr<Store>> open(const std::string& utf8_root, const StoreOptions& opt = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Result<AddOutcome> add_file(const std::string& utf8_path, DuplicatePolicy policy = DuplicatePolicy::Keep,
                                AddMode mode = AddMode::Copy);
    // Buffers are always copied into the store (raw data sets are re-written as Part 10 files).
    Result<AddOutcome> add_buffer(const Bytes& data, DuplicatePolicy policy = DuplicatePolicy::Keep);

    // Bulk-indexes records of files kept in place (as AddMode::Reference, without reading the
    // files): each record needs SopInstanceUid, SeriesInstanceUid, StudyInstanceUid and an
    // absolute FilePath. Existing UIDs are replaced. For importing an existing index.
    Result<std::size_t> add_records(const std::vector<IndexRecord>& records);

    Result<QueryResult> query(const QueryFilter& f) const;
    // Series of a study ordered by Series Number, then Series Date/Time, then UID.
    Result<std::vector<IndexRecord>> series_of(const std::string& study_uid) const;
    // Instances of a series ordered by Instance Number, then position along the slice normal,
    // then UID; FilePath holds the absolute UTF-8 path.
    Result<std::vector<IndexRecord>> instances_of(const std::string& series_uid) const;
    Result<IndexRecord> instance(const std::string& sop_uid) const;

    // Removes everything under one patient key / study / series / instance. Copied files are
    // deleted; referenced files are left alone. Returns the number of instances removed.
    Result<std::size_t> remove(Level level, const std::string& key);
    // Copies the instances to `dest` (Replace policy) and removes them here.
    Result<std::size_t> move_to(Store& dest, Level level, const std::string& key);

    Result<VerifyReport> verify(bool deep, bool repair);
    // Re-indexes every file under data/ and every referenced file still present; writes a fresh
    // snapshot and an empty journal.
    Status rebuild();
    // As above with progress: called with (items done, items known so far; 0 = unknown) on the
    // calling thread; returning false stops with Errc::Cancelled and leaves the store unchanged.
    using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;
    Result<VerifyReport> verify(bool deep, bool repair, const Progress& progress);
    Status rebuild(const Progress& progress);
    Status compact();

    void set_callback(EventCallback cb);
    StoreStats stats() const;
    const std::string& root() const noexcept;

    // Patient key used for Level::Patient: "I" + Patient ID + 0x1F + Issuer, or, when the
    // Patient ID is empty, "N" + folded name + 0x1F + birth date.
    static std::string patient_key(const IndexRecord& r);

    struct Impl;

private:
    explicit Store(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Relative path of a copied instance: "data/ab/cd/<32 hex>.dcm" (generic separators).
std::string instance_relative_path(std::string_view sop_instance_uid);

}  // namespace siaara::store
