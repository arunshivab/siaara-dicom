// SIAARA DICOM Toolkit - DICOMDIR (Media Storage Directory) read and write.
// Written from PS3.10 (7, 8: File IDs, file-set structure), PS3.3 Annex F (Basic Directory IOD,
// F.3 Basic Directory Information Object, F.5 directory record types and their keys) and PS3.11
// (General Purpose CD-R and USB/flash Application Profiles).
//
// Reading: the record hierarchy is walked through the offset links (0004,1200) Offset of the
// First Directory Record of the Root Directory Entity, (0004,1400) Offset of the Next Directory
// Record and (0004,1420) Offset of Referenced Lower-Level Directory Entity. Offsets count bytes
// from the first byte of the file (the preamble is part of the File Meta Information, PS3.10
// 7.1). Every link must land on a record's item tag; cycles, depth beyond the limit and links to
// nowhere are reported and cut, never followed. When the root link itself is unusable the records
// are taken in sequence order and nested by record type (PATIENT > STUDY > SERIES > leaf).
// Each record's keys are reported together with the keys inherited from its ancestors, so a leaf
// record carries the patient, study and series keys as well. Referenced File IDs are sanitised
// (store/path.hpp) and resolved under the DICOMDIR's folder, case-insensitively if needed.
//
// Writing: instance files are copied to <root>/DICOM/Pnnnnnnn/Snnnnnnn/Ennnnnnn/Innnnnnn (names
// made from counters only: no patient data, all components <= 8 characters of A-Z 0-9 as
// PS3.10 8.2 and PS3.11 require), and <root>/DICOMDIR is written in Explicit VR Little Endian with
// PATIENT, STUDY, SERIES and leaf records (record type chosen from the SOP Class).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "store/index.hpp"

namespace siaara::store {

struct DicomdirRecord {
    std::size_t index = 0;        // position in the Directory Record Sequence
    std::int64_t parent = -1;     // index of the parent record, -1 at the root
    std::uint32_t depth = 0;
    std::uint64_t offset = 0;     // file offset of the record's item (0 if unknown)
    bool in_use = true;           // Record In-use Flag (0004,1410) != 0
    std::vector<std::string> file_id;  // Referenced File ID components as stored (may be unsafe)
    Status file_status;           // Ok, or Rejected (unsafe File ID) / NotFound (no such file)
    // own + inherited keys; RecordType; FilePath = resolved path (read_dicomdir) or the File ID
    // as text, components joined with '/' (read_dicomdir_memory; never a path)
    IndexRecord keys;
};

// Defaults of the limits of read_dicomdir_memory (0 in DicomdirReadOptions means these there;
// read_dicomdir takes 0 as "no limit", its behaviour before the limits existed).
inline constexpr std::uint64_t kDicomdirMemoryDefaultMaxRecords = 100'000;
inline constexpr std::uint64_t kDicomdirMemoryDefaultMaxBytes = std::uint64_t{64} << 20;


struct DicomdirReadOptions {
    bool include_inactive = false;   // also report records whose in-use flag is 0
    bool resolve_files = true;       // resolve and check Referenced File IDs
    bool case_insensitive = true;    // look File IDs up case-insensitively when needed; false
                                     // leaves matching to the file system (see resolve_file_id)
    std::uint32_t max_depth = 16;
    // Records visited (every visit counts, a refused second visit too) and items in the Directory
    // Record Sequence: more is Errc::LimitExceeded. 0: no limit (read_dicomdir), or
    // kDicomdirMemoryDefaultMaxRecords (read_dicomdir_memory).
    std::uint64_t max_records = 0;
    // Size of the DICOMDIR in bytes: a larger one is Errc::LimitExceeded before it is parsed.
    // 0: no limit (read_dicomdir), or kDicomdirMemoryDefaultMaxBytes (read_dicomdir_memory).
    std::uint64_t max_bytes = 0;
};

struct DicomdirSummary {
    std::string file_set_id;
    std::size_t records = 0;         // items in the sequence
    std::size_t reported = 0;        // records passed to the callback
    std::size_t files = 0;           // records with a Referenced File ID that resolved
    std::size_t missing_files = 0;
    std::size_t rejected_paths = 0;
    std::size_t unreachable = 0;     // records no link leads to
    bool sequential_fallback = false;
    std::vector<std::string> warnings;  // never contain patient data
};

// Return false from the callback to stop (the call then fails with Errc::Cancelled).
using DicomdirCallback = std::function<bool(const DicomdirRecord&)>;

Result<DicomdirSummary> read_dicomdir(const std::string& utf8_path, const DicomdirCallback& cb,
                                      const DicomdirReadOptions& opt = {});

// A DICOMDIR held in memory (for example a member of a ZIP that came by mail). Never touches the
// disk. Parsed as untrusted input: Part 10 only, no overlong-length recovery, the untrusted
// nesting, element and inflate limits (core/untrusted.hpp), and the record limits above. A
// Referenced File ID is never resolved: DicomdirRecord::file_id holds its components,
// keys[Str::FilePath] the components joined with '/', and file_status is Ok when the File ID is
// well formed or Errc::Rejected when it is not (file_id_well_formed). resolve_files and
// case_insensitive are not used. `data` is only read during the call.
Result<DicomdirSummary> read_dicomdir_memory(const std::uint8_t* data, std::size_t size, const DicomdirCallback& cb,
                                             const DicomdirReadOptions& opt = {});

// Most components of a well-formed File ID, and most characters of one component (PS3.10 8.5).
inline constexpr std::size_t kFileIdMaxComponents = 8;
inline constexpr std::size_t kFileIdMaxComponentChars = 8;
// Is a Referenced File ID (its components, split_file_id) well formed by PS3.10 8.5: 1 to
// kFileIdMaxComponents components, each of 1 to kFileIdMaxComponentChars characters, made of the
// File ID characters (upper-case A-Z, digits 0-9 and underscore) only? Rules out "..", absolute
// paths, drive letters, every separator and over-long components.
bool file_id_well_formed(const std::vector<std::string>& components) noexcept;

enum class MediaProfile {
    GeneralCd,   // STD-GEN-CD / STD-GEN-DVD-RAM: Explicit VR Little Endian only
    GeneralUsb,  // STD-GEN-USB-* / STD-GEN-SEC-USB-*: explicit VR transfer syntaxes incl. compressed
};

struct DicomdirWriteOptions {
    MediaProfile profile = MediaProfile::GeneralCd;
    std::string file_set_id;              // (0004,1130), up to 16 of A-Z 0-9 _ space
    bool overwrite = false;               // replace an existing DICOMDIR / files
    bool allow_any_transfer_syntax = false;  // copy files the profile does not allow (warned)
    // Called before each input file is read and before each is copied: (steps done, 2 x files).
    // Returning false stops with Errc::Cancelled (files copied so far stay; no DICOMDIR is written).
    std::function<bool(std::uint64_t done, std::uint64_t total)> progress;
};

struct DicomdirWriteSummary {
    std::size_t files = 0, patients = 0, studies = 0, series = 0;
    std::vector<std::string> warnings;    // never contain patient data
};

Result<DicomdirWriteSummary> write_dicomdir(const std::string& utf8_root,
                                            const std::vector<std::string>& utf8_files,
                                            const DicomdirWriteOptions& opt = {});

// Directory record type for a SOP Class (PS3.3 F.5), "IMAGE" when not otherwise defined.
std::string_view record_type_for(std::string_view sop_class_uid) noexcept;

}  // namespace siaara::store
