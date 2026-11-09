// SIAARA DICOM Toolkit - Structured Reports.
// Written from DICOM PS3.3 C.17 (SR Document General / SR Document Content modules: content
// items, value types, relationship types, by-reference relationships C.17.3.2.4), C.18 (content
// item macros: TEXT, CODE, NUM, DATETIME, DATE, TIME, UIDREF, PNAME, COMPOSITE, IMAGE, WAVEFORM,
// SCOORD, SCOORD3D, TCOORD, CONTAINER, TABLE), A.35 (SR IODs including Key Object Selection,
// A.35.4), C.17.6 (Key Object Document) and PS3.16 TID 2010 (Key Object Selection) / CID 7010.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "clinical/common.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara::clinical {

enum class SrValueType : std::uint8_t {
    Container, Text, Code, Num, DateTime, Date, Time, UidRef, PName, Composite, Image, Waveform,
    Scoord, Scoord3d, Tcoord, Table,
    Reference,  // by-reference relationship (Referenced Content Item Identifier, no value)
    Unknown,
};

enum class SrRelationship : std::uint8_t {
    None,  // the root
    Contains, HasProperties, HasObsContext, HasAcqContext, InferredFrom, SelectedFrom, HasConceptMod,
    Unknown,
};

enum class SrDocumentKind : std::uint8_t {
    BasicText, Enhanced, Comprehensive, Comprehensive3D, Extensible, KeyObjectSelection,
    XRayRadiationDose, EnhancedXRayRadiationDose, RadiopharmaceuticalRadiationDose, PatientRadiationDose,
    MammographyCad, ChestCad, ColonCad, ProcedureLog, AcquisitionContext, Other,
};

struct SrSopReference {
    std::string sop_class_uid, sop_instance_uid;
    std::vector<std::int64_t> frames;     // Referenced Frame Number
    std::vector<std::int64_t> segments;   // Referenced Segment Number
    std::vector<std::int64_t> channels;   // Referenced Waveform Channels (pairs)
    std::string presentation_class_uid, presentation_instance_uid;  // IMAGE: referenced presentation state
};

struct SrTableCell {
    std::int64_t row = 0, column = 0;     // 1-based; 0 when the item covers a whole column / row
    std::string vr;                       // Selector Attribute VR
    std::vector<std::string> values;      // values as text
    std::optional<Code> code;             // SQ cells: the code
    std::optional<Code> units;
    std::vector<std::uint32_t> reference; // Referenced Content Item Identifier
};

struct SrNode {
    std::int32_t parent = -1;
    std::vector<std::uint32_t> children;
    std::uint32_t depth = 0;
    std::vector<std::uint32_t> position;   // 1-based item positions from the root ({1} for the root)
    SrRelationship relationship = SrRelationship::None;
    SrValueType value_type = SrValueType::Unknown;
    std::string relationship_text, value_type_text;
    std::optional<Code> concept_name;

    std::string text;                      // TEXT, DATETIME, DATE, TIME, UIDREF, PNAME (UTF-8)
    std::optional<Code> code;              // CODE
    std::optional<double> numeric;         // NUM: Numeric Value (or Floating Point Value)
    std::string numeric_text;              //      as written (DS)
    std::optional<double> float_value;     //      Floating Point Value (FD) when present
    std::optional<Code> units, qualifier;  //      Measurement Units, Numeric Value Qualifier
    std::vector<SrSopReference> references;  // COMPOSITE, IMAGE, WAVEFORM (and SCOORD's parent IMAGE)
    std::string graphic_type;              // SCOORD, SCOORD3D
    std::vector<double> graphic_data;
    std::string frame_of_reference_uid;    // SCOORD3D
    std::string pixel_origin_interpretation;
    std::string temporal_range_type;       // TCOORD
    std::vector<double> sample_positions, time_offsets;
    std::vector<std::string> datetimes;
    std::string continuity;                // CONTAINER: SEPARATE / CONTINUOUS
    std::string template_id, mapping_resource;
    std::int64_t table_rows = 0, table_columns = 0;  // TABLE
    std::vector<SrTableCell> cells;
    std::vector<std::uint32_t> referenced_path;      // Reference nodes
    std::int32_t referenced_node = -1;               // resolved target, -1 if unresolved
    std::string observation_datetime, observation_uid;
};

struct SrDocument {
    SrDocumentKind kind = SrDocumentKind::Other;
    std::string sop_class_uid;
    std::string completion_flag, verification_flag;
    std::vector<SrNode> nodes;             // nodes[0] is the root container
    std::vector<std::string> warnings;
};

struct SrReadOptions {
    std::uint32_t max_depth = 64;
    std::uint32_t max_nodes = 1u << 20;
    bool lenient = true;   // unknown value / relationship types and bad references become warnings
};

// Reads the content tree of any SR document (the data set must carry Value Type CONTAINER at
// the top level). Fails with NotFound when there is no content tree, Malformed for a broken
// tree in strict mode, LimitExceeded past the limits.
Result<SrDocument> read_sr(const Dataset& ds, const SrReadOptions& opt = {});

std::string_view value_type_name(SrValueType v) noexcept;
std::string_view relationship_name(SrRelationship r) noexcept;

// Indented plain-text rendering for display, one line per node (truncated at max_bytes).
std::string sr_to_text(const SrDocument& doc, std::size_t max_bytes = 16u << 20);

// ---------------------------------------------------------------- Key Object Selection writer
struct KosReference {
    std::string study_uid, series_uid, sop_class_uid, sop_instance_uid;
    std::vector<std::int32_t> frames;
    bool composite = false;  // COMPOSITE content item instead of IMAGE (non-image objects)
};

struct KosWriteRequest {
    const Dataset* source = nullptr;   // Patient / General Study attributes are copied from it
    std::optional<Code> title;         // a CID 7010 code; default (113000, DCM, "Of Interest")
    std::string description;           // optional TEXT (113012, DCM, "Key Object Description")
    std::vector<KosReference> references;
    std::string series_uid, sop_instance_uid;  // generated when empty
    std::int32_t series_number = 9901, instance_number = 1;
    std::string manufacturer = "SIAARA";
};

// Builds a Key Object Selection Document (explicit VR little endian). The title must be a row of
// CID 7010 as generated from PS3.16.
Result<DicomFile> write_kos(const KosWriteRequest& req);

}  // namespace siaara::clinical
