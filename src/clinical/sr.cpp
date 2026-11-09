// SIAARA DICOM Toolkit - Structured Reports (see sr.hpp for the references).
#include "clinical/sr.hpp"

#include <algorithm>
#include <map>
#include <utility>

#include "charset/charset.hpp"
#include "clinical/codes.hpp"
#include "core/transfer_syntax.hpp"

namespace siaara::clinical {

namespace {

constexpr Tag kReferencedSegmentNumber = 0x0062000B;
constexpr Tag kStudyInstanceUid = 0x0020000D;
constexpr Tag kSelectorCodeSequenceValue = 0x00720080;
constexpr Tag kReferencedFrameOfReferenceUid = 0x30060024;

struct Kind {
    std::string_view uid;
    SrDocumentKind kind;
};
constexpr Kind kKinds[] = {
    {"1.2.840.10008.5.1.4.1.1.88.11", SrDocumentKind::BasicText},
    {"1.2.840.10008.5.1.4.1.1.88.22", SrDocumentKind::Enhanced},
    {"1.2.840.10008.5.1.4.1.1.88.33", SrDocumentKind::Comprehensive},
    {"1.2.840.10008.5.1.4.1.1.88.34", SrDocumentKind::Comprehensive3D},
    {"1.2.840.10008.5.1.4.1.1.88.35", SrDocumentKind::Extensible},
    {"1.2.840.10008.5.1.4.1.1.88.40", SrDocumentKind::ProcedureLog},
    {"1.2.840.10008.5.1.4.1.1.88.50", SrDocumentKind::MammographyCad},
    {"1.2.840.10008.5.1.4.1.1.88.59", SrDocumentKind::KeyObjectSelection},
    {"1.2.840.10008.5.1.4.1.1.88.65", SrDocumentKind::ChestCad},
    {"1.2.840.10008.5.1.4.1.1.88.67", SrDocumentKind::XRayRadiationDose},
    {"1.2.840.10008.5.1.4.1.1.88.68", SrDocumentKind::RadiopharmaceuticalRadiationDose},
    {"1.2.840.10008.5.1.4.1.1.88.69", SrDocumentKind::ColonCad},
    {"1.2.840.10008.5.1.4.1.1.88.71", SrDocumentKind::AcquisitionContext},
    {"1.2.840.10008.5.1.4.1.1.88.73", SrDocumentKind::PatientRadiationDose},
    {"1.2.840.10008.5.1.4.1.1.88.76", SrDocumentKind::EnhancedXRayRadiationDose},
};
constexpr std::string_view kKosSopClass = "1.2.840.10008.5.1.4.1.1.88.59";

struct VtName {
    std::string_view name;
    SrValueType vt;
};
constexpr VtName kValueTypes[] = {
    {"CONTAINER", SrValueType::Container}, {"TEXT", SrValueType::Text},       {"CODE", SrValueType::Code},
    {"NUM", SrValueType::Num},             {"DATETIME", SrValueType::DateTime}, {"DATE", SrValueType::Date},
    {"TIME", SrValueType::Time},           {"UIDREF", SrValueType::UidRef},   {"PNAME", SrValueType::PName},
    {"COMPOSITE", SrValueType::Composite}, {"IMAGE", SrValueType::Image},     {"WAVEFORM", SrValueType::Waveform},
    {"SCOORD", SrValueType::Scoord},       {"SCOORD3D", SrValueType::Scoord3d}, {"TCOORD", SrValueType::Tcoord},
    {"TABLE", SrValueType::Table},
};
struct RelName {
    std::string_view name;
    SrRelationship rel;
};
constexpr RelName kRelationships[] = {
    {"CONTAINS", SrRelationship::Contains},         {"HAS PROPERTIES", SrRelationship::HasProperties},
    {"HAS OBS CONTEXT", SrRelationship::HasObsContext}, {"HAS ACQ CONTEXT", SrRelationship::HasAcqContext},
    {"INFERRED FROM", SrRelationship::InferredFrom}, {"SELECTED FROM", SrRelationship::SelectedFrom},
    {"HAS CONCEPT MOD", SrRelationship::HasConceptMod},
};

std::vector<std::int64_t> ints(const Dataset& d, Tag t) {
    std::vector<std::int64_t> out;
    if (auto v = get_numbers(d, t, 1u << 20))
        for (double x : *v) out.push_back(static_cast<std::int64_t>(x));
    return out;
}

class Reader {
public:
    Reader(const SrReadOptions& o, SrDocument& d, const charset::CharacterSet& cs) : opt_(o), doc_(d), cs_(cs) {}

    std::string utf8(const Dataset& d, Tag t) {
        auto v = charset::get_utf8(d, t, 0, &cs_);
        return v ? *v : std::string();
    }

    Status warn_or_fail(std::string msg) {
        if (!opt_.lenient) return Status(Errc::Malformed, std::move(msg));
        if (doc_.warnings.size() < 1000) doc_.warnings.push_back(std::move(msg));
        return Status::ok();
    }

    std::vector<SrSopReference> sop_refs(const Dataset& item) {
        std::vector<SrSopReference> out;
        for (const Dataset& r : items_of(item, tags::ReferencedSopSequence)) {
            SrSopReference s;
            s.sop_class_uid = get_text(r, tags::ReferencedSopClassUid).value_or("");
            s.sop_instance_uid = get_text(r, tags::ReferencedSopInstanceUid).value_or("");
            s.frames = ints(r, tags::ReferencedFrameNumber);
            s.segments = ints(r, kReferencedSegmentNumber);
            s.channels = ints(r, tags::ReferencedWaveformChannels);
            if (const Dataset* p = first_item(r, tags::ReferencedSopSequence)) {
                s.presentation_class_uid = get_text(*p, tags::ReferencedSopClassUid).value_or("");
                s.presentation_instance_uid = get_text(*p, tags::ReferencedSopInstanceUid).value_or("");
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    // Fills the value of node `n` from content item `item`.
    Status read_value(const Dataset& item, SrNode& n) {
        switch (n.value_type) {
            case SrValueType::Container:
                n.continuity = get_text(item, tags::ContinuityOfContent).value_or("");
                if (const Dataset* t = first_item(item, tags::ContentTemplateSequence)) {
                    n.template_id = get_text(*t, tags::TemplateIdentifier).value_or("");
                    n.mapping_resource = get_text(*t, tags::MappingResource).value_or("");
                }
                break;
            case SrValueType::Text: n.text = utf8(item, tags::TextValue); break;
            case SrValueType::DateTime: n.text = get_text(item, tags::DateTimeValue).value_or(""); break;
            case SrValueType::Date: n.text = get_text(item, tags::DateValue).value_or(""); break;
            case SrValueType::Time: n.text = get_text(item, tags::TimeValue).value_or(""); break;
            case SrValueType::UidRef: n.text = get_text(item, tags::UidValue).value_or(""); break;
            case SrValueType::PName: n.text = utf8(item, tags::PersonNameValue); break;
            case SrValueType::Code:
                n.code = read_code(item, tags::ConceptCodeSequence);
                if (!n.code) return warn_or_fail("CODE content item without Concept Code Sequence");
                break;
            case SrValueType::Num: {
                if (const Dataset* mv = first_item(item, tags::MeasuredValueSequence)) {
                    n.numeric_text = get_text(*mv, tags::NumericValue).value_or("");
                    if (!n.numeric_text.empty()) n.numeric = parse_decimal(n.numeric_text);
                    if (auto f = mv->get_double(tags::FloatingPointValue)) {
                        n.float_value = *f;
                        n.numeric = *f;  // FD carries the full precision (C.18.1.1)
                    }
                    if (!n.numeric) {
                        auto num = mv->get_int(tags::RationalNumeratorValue);
                        auto den = mv->get_int(tags::RationalDenominatorValue);
                        if (num && den && *den != 0) n.numeric = static_cast<double>(*num) / static_cast<double>(*den);
                    }
                    n.units = read_code(*mv, tags::MeasurementUnitsCodeSequence);
                }
                n.qualifier = read_code(item, tags::NumericValueQualifierCodeSequence);
                if (!n.numeric && !n.qualifier) return warn_or_fail("NUM content item without a value or qualifier");
                break;
            }
            case SrValueType::Composite:
            case SrValueType::Image:
            case SrValueType::Waveform:
                n.references = sop_refs(item);
                if (n.references.empty()) return warn_or_fail("reference content item without Referenced SOP Sequence");
                break;
            case SrValueType::Scoord:
            case SrValueType::Scoord3d: {
                n.graphic_type = get_text(item, tags::GraphicType).value_or("");
                auto g = get_numbers(item, tags::GraphicData, 1u << 24);
                if (g) n.graphic_data = std::move(*g);
                n.pixel_origin_interpretation = get_text(item, tags::PixelOriginInterpretation).value_or("");
                if (n.value_type == SrValueType::Scoord3d)
                    n.frame_of_reference_uid = get_text(item, kReferencedFrameOfReferenceUid).value_or("");
                const std::size_t dim = n.value_type == SrValueType::Scoord ? 2 : 3;
                if (n.graphic_data.empty() || n.graphic_data.size() % dim)
                    return warn_or_fail("SCOORD content item with bad Graphic Data");
                break;
            }
            case SrValueType::Tcoord: {
                n.temporal_range_type = get_text(item, tags::TemporalRangeType).value_or("");
                if (auto v = get_numbers(item, tags::ReferencedSamplePositions, 1u << 24)) n.sample_positions = std::move(*v);
                if (auto v = get_numbers(item, tags::ReferencedTimeOffsets, 1u << 24)) n.time_offsets = std::move(*v);
                const std::size_t k = item.value_multiplicity(tags::ReferencedDateTime);
                for (std::size_t i = 0; i < k && i < (1u << 20); ++i)
                    if (auto s = get_text(item, tags::ReferencedDateTime, i)) n.datetimes.push_back(*s);
                break;
            }
            case SrValueType::Table: {
                const Dataset* tv = first_item(item, tags::TabulatedValuesSequence);
                if (!tv) return warn_or_fail("TABLE content item without Tabulated Values Sequence");
                n.table_rows = tv->get_int(tags::NumberOfTableRows).value_or(0);
                n.table_columns = tv->get_int(tags::NumberOfTableColumns).value_or(0);
                for (const Dataset& c : items_of(*tv, tags::CellValuesSequence)) {
                    SrTableCell cell;
                    cell.row = c.get_int(tags::TableRowNumber).value_or(0);
                    cell.column = c.get_int(tags::TableColumnNumber).value_or(0);
                    cell.vr = get_text(c, tags::SelectorAttributeVr).value_or("");
                    for (auto x : ints(c, tags::ReferencedContentItemIdentifier))
                        cell.reference.push_back(static_cast<std::uint32_t>(x));
                    cell.units = read_code(c, tags::MeasurementUnitsCodeSequence);
                    if (const Dataset* cs = first_item(c, kSelectorCodeSequenceValue)) cell.code = read_code_item(*cs);
                    for (const Element& e : c.elements()) {
                        if (tag_group(e.tag) != 0x0072 || tag_element(e.tag) < 0x005E || tag_element(e.tag) > 0x0083 ||
                            e.tag == kSelectorCodeSequenceValue || e.is_sequence())
                            continue;
                        const std::size_t k = c.value_multiplicity(e.tag);
                        for (std::size_t i = 0; i < k && i < (1u << 16); ++i) {
                            std::optional<std::string> s = charset::vr_uses_character_set(e.vr) ? charset::get_utf8(c, e.tag, i, &cs_)
                                                                                                : c.get_string(e.tag, i);
                            if (s) cell.values.push_back(*s);
                        }
                        break;
                    }
                    n.cells.push_back(std::move(cell));
                }
                break;
            }
            case SrValueType::Reference:
            case SrValueType::Unknown: break;
        }
        return Status::ok();
    }

    Status walk(const Dataset& item, std::int32_t parent, std::uint32_t depth, std::vector<std::uint32_t> position) {
        if (depth > opt_.max_depth) return Status(Errc::LimitExceeded, "SR content tree deeper than the limit");
        if (doc_.nodes.size() >= opt_.max_nodes) return Status(Errc::LimitExceeded, "SR content tree has too many nodes");
        SrNode n;
        n.parent = parent;
        n.depth = depth;
        n.position = position;
        if (parent >= 0) {
            n.relationship_text = get_text(item, tags::RelationshipType).value_or("");
            n.relationship = SrRelationship::Unknown;
            for (const auto& r : kRelationships)
                if (r.name == n.relationship_text) n.relationship = r.rel;
            if (n.relationship == SrRelationship::Unknown)
                if (Status s = warn_or_fail("unknown or missing Relationship Type"); !s) return s;
        }
        if (auto ref = get_numbers(item, tags::ReferencedContentItemIdentifier, 1024); ref && !ref->empty() && !item.contains(tags::ValueType)) {
            n.value_type = SrValueType::Reference;
            n.value_type_text = "";
            for (double x : *ref) n.referenced_path.push_back(static_cast<std::uint32_t>(x));
        } else {
            n.value_type_text = get_text(item, tags::ValueType).value_or("");
            for (const auto& v : kValueTypes)
                if (v.name == n.value_type_text) n.value_type = v.vt;
            if (n.value_type == SrValueType::Unknown)
                if (Status s = warn_or_fail("unknown or missing Value Type"); !s) return s;
            n.concept_name = read_code(item, tags::ConceptNameCodeSequence);
            n.observation_datetime = get_text(item, tags::ObservationDateTime).value_or("");
            n.observation_uid = get_text(item, tags::ObservationUid).value_or("");
            if (Status s = read_value(item, n); !s) return s;
        }
        const auto self = static_cast<std::int32_t>(doc_.nodes.size());
        doc_.nodes.push_back(std::move(n));
        if (parent >= 0) doc_.nodes[static_cast<std::size_t>(parent)].children.push_back(static_cast<std::uint32_t>(self));
        const auto& kids = items_of(item, tags::ContentSequence);
        if (!kids.empty() && doc_.nodes[static_cast<std::size_t>(self)].value_type == SrValueType::Reference)
            if (Status s = warn_or_fail("by-reference content item has children"); !s) return s;
        for (std::size_t k = 0; k < kids.size(); ++k) {
            std::vector<std::uint32_t> p = position;
            p.push_back(static_cast<std::uint32_t>(k + 1));
            if (Status s = walk(kids[k], self, depth + 1, std::move(p)); !s) return s;
        }
        return Status::ok();
    }

private:
    const SrReadOptions& opt_;
    SrDocument& doc_;
    const charset::CharacterSet& cs_;
};

std::string code_text(const Code& c) {
    return "(" + c.value + ", " + c.scheme + ", \"" + c.meaning + "\")";
}

std::string position_text(const std::vector<std::uint32_t>& p) {
    std::string s;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (i) s.push_back('.');
        s += std::to_string(p[i]);
    }
    return s;
}

}  // namespace

std::string_view value_type_name(SrValueType v) noexcept {
    for (const auto& e : kValueTypes)
        if (e.vt == v) return e.name;
    return v == SrValueType::Reference ? "REFERENCE" : "UNKNOWN";
}

std::string_view relationship_name(SrRelationship r) noexcept {
    for (const auto& e : kRelationships)
        if (e.rel == r) return e.name;
    return r == SrRelationship::None ? "" : "UNKNOWN";
}

Result<SrDocument> read_sr(const Dataset& ds, const SrReadOptions& opt) {
    SrDocument doc;
    doc.sop_class_uid = get_text(ds, siaara::tags::SOPClassUID).value_or("");
    for (const auto& k : kKinds)
        if (k.uid == doc.sop_class_uid) doc.kind = k.kind;
    if (get_text(ds, tags::ValueType).value_or("") != "CONTAINER")
        return Status(Errc::NotFound, "no SR content tree (top-level Value Type is not CONTAINER)");
    doc.completion_flag = get_text(ds, tags::CompletionFlag).value_or("");
    doc.verification_flag = get_text(ds, tags::VerificationFlag).value_or("");
    const charset::CharacterSet cs = charset::character_set_of(ds);
    Reader rd(opt, doc, cs);
    if (Status s = rd.walk(ds, -1, 0, {1}); !s) return s;
    // Resolve by-reference relationships (C.17.3.2.4).
    std::map<std::vector<std::uint32_t>, std::int32_t> by_path;
    for (std::size_t i = 0; i < doc.nodes.size(); ++i) by_path[doc.nodes[i].position] = static_cast<std::int32_t>(i);
    for (auto& n : doc.nodes) {
        if (n.value_type != SrValueType::Reference) continue;
        auto it = by_path.find(n.referenced_path);
        if (it == by_path.end()) {
            if (Status s = rd.warn_or_fail("unresolved by-reference relationship"); !s) return s;
            continue;
        }
        n.referenced_node = it->second;
    }
    return doc;
}

std::string sr_to_text(const SrDocument& doc, std::size_t max_bytes) {
    std::string out;
    // Pre-order walk with an explicit stack (the tree depth is already bounded by the reader).
    std::vector<std::uint32_t> stack;
    if (!doc.nodes.empty()) stack.push_back(0);
    while (!stack.empty()) {
        const std::uint32_t i = stack.back();
        stack.pop_back();
        const SrNode& n = doc.nodes[i];
        std::string line(static_cast<std::size_t>(n.depth) * 2, ' ');
        line += position_text(n.position);
        if (!n.relationship_text.empty()) line += " " + n.relationship_text;
        if (n.value_type == SrValueType::Reference) {
            line += " -> ";
            line += n.referenced_node >= 0 ? position_text(doc.nodes[static_cast<std::size_t>(n.referenced_node)].position)
                                           : position_text(n.referenced_path) + " (unresolved)";
        } else {
            line += " " + std::string(n.value_type_text.empty() ? value_type_name(n.value_type) : n.value_type_text);
            if (n.concept_name) line += " " + code_text(*n.concept_name);
            switch (n.value_type) {
                case SrValueType::Container:
                    if (!n.continuity.empty()) line += " [" + n.continuity + "]";
                    if (!n.template_id.empty()) line += " TID " + n.template_id;
                    break;
                case SrValueType::Text:
                case SrValueType::DateTime:
                case SrValueType::Date:
                case SrValueType::Time:
                case SrValueType::UidRef:
                case SrValueType::PName: line += " = \"" + n.text + "\""; break;
                case SrValueType::Code:
                    if (n.code) line += " = " + code_text(*n.code);
                    break;
                case SrValueType::Num:
                    line += " = ";
                    line += !n.numeric_text.empty() ? n.numeric_text : (n.numeric ? format_ds(*n.numeric) : std::string("(no value)"));
                    if (n.units) line += " " + (n.units->scheme == "UCUM" ? n.units->value : n.units->meaning);
                    if (n.qualifier) line += " " + code_text(*n.qualifier);
                    break;
                case SrValueType::Composite:
                case SrValueType::Image:
                case SrValueType::Waveform:
                    for (const auto& r : n.references) {
                        line += " = " + r.sop_class_uid + " " + r.sop_instance_uid;
                        if (!r.frames.empty()) line += " frames " + std::to_string(r.frames.size());
                    }
                    break;
                case SrValueType::Scoord:
                case SrValueType::Scoord3d:
                    line += " = " + n.graphic_type + " " + std::to_string(n.graphic_data.size() / (n.value_type == SrValueType::Scoord ? 2 : 3)) +
                            " points";
                    break;
                case SrValueType::Tcoord: line += " = " + n.temporal_range_type; break;
                case SrValueType::Table:
                    line += " = " + std::to_string(n.table_rows) + " x " + std::to_string(n.table_columns) + " table, " +
                            std::to_string(n.cells.size()) + " cell items";
                    break;
                default: break;
            }
        }
        line.push_back('\n');
        if (out.size() + line.size() > max_bytes) {
            out += "...\n";
            break;
        }
        out += line;
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
    }
    return out;
}

// ---------------------------------------------------------------- Key Object Selection writer
Result<DicomFile> write_kos(const KosWriteRequest& req) {
    if (req.references.empty()) return Status(Errc::InvalidArgument, "KOS needs at least one referenced instance");
    for (const auto& r : req.references)
        if (!uid_valid(r.study_uid) || !uid_valid(r.series_uid) || !uid_valid(r.sop_class_uid) || !uid_valid(r.sop_instance_uid))
            return Status(Errc::InvalidArgument, "KOS reference needs valid study, series, SOP class and instance UIDs");
    Code title = req.title ? *req.title : codes::kCid7010[0].code();
    if (!cid_code(7010, title.value, title.scheme))
        return Status(Errc::InvalidArgument, "KOS title is not a code of CID 7010");
    title = *cid_code(7010, title.value, title.scheme);  // meaning exactly as in PS3.16
    if (!req.series_uid.empty() && !uid_valid(req.series_uid)) return Status(Errc::InvalidArgument, "invalid series UID");
    if (!req.sop_instance_uid.empty() && !uid_valid(req.sop_instance_uid)) return Status(Errc::InvalidArgument, "invalid SOP instance UID");

    DicomFile file;
    file.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = file.dataset;
    bool non_ascii = false;
    const std::string date = today_da(), time = now_tm();
    (void)ds.set_string(siaara::tags::SOPClassUID, VR::UI, kKosSopClass);
    (void)ds.set_string(siaara::tags::SOPInstanceUID, VR::UI, req.sop_instance_uid.empty() ? new_uid() : req.sop_instance_uid);
    (void)ds.set_string(tags::InstanceCreationDate, VR::DA, date);
    (void)ds.set_string(tags::InstanceCreationTime, VR::TM, time);
    copy_patient_study(req.source, ds, non_ascii);
    if (!ds.contains(siaara::tags::StudyInstanceUID)) return Status(Errc::InvalidArgument, "source has no Study Instance UID");
    // Key Object Document Series
    (void)ds.set_string(siaara::tags::Modality, VR::CS, "KO");
    (void)ds.set_string(siaara::tags::SeriesInstanceUID, VR::UI, req.series_uid.empty() ? new_uid() : req.series_uid);
    (void)ds.set_string(tags::SeriesNumber, VR::IS, format_is(req.series_number));
    ds.put(make_sequence(tags::ReferencedPerformedProcedureStepSequence, {}));
    // General Equipment
    put_text(ds, tags::Manufacturer, VR::LO, req.manufacturer, non_ascii);
    // Key Object Document
    (void)ds.set_string(tags::InstanceNumber, VR::IS, format_is(req.instance_number));
    (void)ds.set_string(tags::ContentDate, VR::DA, date);
    (void)ds.set_string(tags::ContentTime, VR::TM, time);
    {
        // Current Requested Procedure Evidence: study -> series -> instances, in first-seen order.
        std::vector<std::string> studies;
        for (const auto& r : req.references)
            if (std::find(studies.begin(), studies.end(), r.study_uid) == studies.end()) studies.push_back(r.study_uid);
        std::vector<Dataset> st_items;
        for (const auto& st : studies) {
            std::vector<std::string> series;
            for (const auto& r : req.references)
                if (r.study_uid == st && std::find(series.begin(), series.end(), r.series_uid) == series.end())
                    series.push_back(r.series_uid);
            std::vector<Dataset> se_items;
            for (const auto& se : series) {
                std::vector<Dataset> sops;
                std::vector<std::string> seen;
                for (const auto& r : req.references) {
                    if (r.study_uid != st || r.series_uid != se) continue;
                    if (std::find(seen.begin(), seen.end(), r.sop_instance_uid) != seen.end()) continue;
                    seen.push_back(r.sop_instance_uid);
                    Dataset d;
                    (void)d.set_string(tags::ReferencedSopClassUid, VR::UI, r.sop_class_uid);
                    (void)d.set_string(tags::ReferencedSopInstanceUid, VR::UI, r.sop_instance_uid);
                    sops.push_back(std::move(d));
                }
                Dataset item;
                item.put(make_sequence(tags::ReferencedSopSequence, std::move(sops)));
                (void)item.set_string(siaara::tags::SeriesInstanceUID, VR::UI, se);
                se_items.push_back(std::move(item));
            }
            Dataset item;
            item.put(make_sequence(tags::ReferencedSeriesSequence, std::move(se_items)));
            (void)item.set_string(kStudyInstanceUid, VR::UI, st);
            st_items.push_back(std::move(item));
        }
        ds.put(make_sequence(tags::CurrentRequestedProcedureEvidenceSequence, std::move(st_items)));
    }
    // SR Document Content: TID 2010 root container.
    (void)ds.set_string(tags::ValueType, VR::CS, "CONTAINER");
    {
        std::vector<Dataset> c;
        c.push_back(code_item(title));
        ds.put(make_sequence(tags::ConceptNameCodeSequence, std::move(c)));
    }
    (void)ds.set_string(tags::ContinuityOfContent, VR::CS, "SEPARATE");
    {
        Dataset t;
        (void)t.set_string(tags::MappingResource, VR::CS, "DCMR");
        (void)t.set_string(tags::TemplateIdentifier, VR::CS, "2010");
        std::vector<Dataset> v;
        v.push_back(std::move(t));
        ds.put(make_sequence(tags::ContentTemplateSequence, std::move(v)));
    }
    std::vector<Dataset> content;
    if (!req.description.empty()) {
        Dataset d;
        (void)d.set_string(tags::RelationshipType, VR::CS, "CONTAINS");
        (void)d.set_string(tags::ValueType, VR::CS, "TEXT");
        std::vector<Dataset> c;
        c.push_back(code_item(codes::kKeyObjectDescription.code()));
        d.put(make_sequence(tags::ConceptNameCodeSequence, std::move(c)));
        put_text(d, tags::TextValue, VR::UT, req.description, non_ascii);
        content.push_back(std::move(d));
    }
    for (const auto& r : req.references) {
        Dataset d;
        (void)d.set_string(tags::RelationshipType, VR::CS, "CONTAINS");
        (void)d.set_string(tags::ValueType, VR::CS, r.composite ? "COMPOSITE" : "IMAGE");
        Dataset ref;
        (void)ref.set_string(tags::ReferencedSopClassUid, VR::UI, r.sop_class_uid);
        (void)ref.set_string(tags::ReferencedSopInstanceUid, VR::UI, r.sop_instance_uid);
        if (!r.frames.empty() && !r.composite) {
            std::string f;
            for (std::size_t i = 0; i < r.frames.size(); ++i) {
                if (r.frames[i] < 1) return Status(Errc::InvalidArgument, "frame numbers are 1-based");
                if (i) f.push_back('\\');
                f += std::to_string(r.frames[i]);
            }
            (void)ref.set_string(tags::ReferencedFrameNumber, VR::IS, f);
        }
        std::vector<Dataset> v;
        v.push_back(std::move(ref));
        d.put(make_sequence(tags::ReferencedSopSequence, std::move(v)));
        content.push_back(std::move(d));
    }
    ds.put(make_sequence(tags::ContentSequence, std::move(content)));
    finish_charset(ds, non_ascii);
    return file;
}

}  // namespace siaara::clinical
