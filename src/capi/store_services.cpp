// SIAARA DICOM Toolkit - network services answered from a local store (capi/store_services.hpp).
#include "capi/store_services.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "charset/charset.hpp"
#include "core/io.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"
#include "net/dimse.hpp"

namespace siaara::capi {

namespace {

using store::IndexRecord;
using store::Str;

// Query/Retrieve levels.
constexpr int kPatient = 0, kStudy = 1, kSeries = 2, kImage = 3;

constexpr Tag kQueryRetrieveLevel = 0x00080052;
constexpr Tag kRetrieveAeTitle = 0x00080054;
constexpr Tag kInstanceAvailability = 0x00080056;

enum class Src : std::uint8_t {
    Field,
    PatientStudies, PatientSeries, PatientInstances,
    StudySeries, StudyInstances, SeriesInstances,
    SeriesNumber, InstanceNumber, Rows, Columns, Frames,
    RetrieveAe, Availability,
};

struct KeyDef {
    Tag tag;
    VR vr;
    int level;        // lowest Q/R level the key belongs to
    Src src;
    Str field;        // for Src::Field
    bool return_key;  // one of the level's usual return keys (identifier builder, QIDO defaults)
};

// The attributes the store can match on and return, per level (PS3.4 C.6.1.1 / C.6.2.1).
const std::vector<KeyDef>& key_table() {
    static const std::vector<KeyDef> t = {
        {0x00100010, VR::PN, kPatient, Src::Field, Str::PatientName, true},
        {0x00100020, VR::LO, kPatient, Src::Field, Str::PatientId, true},
        {0x00100021, VR::LO, kPatient, Src::Field, Str::IssuerOfPatientId, false},
        {0x00100030, VR::DA, kPatient, Src::Field, Str::PatientBirthDate, true},
        {0x00100040, VR::CS, kPatient, Src::Field, Str::PatientSex, true},
        {0x00201200, VR::IS, kPatient, Src::PatientStudies, Str::Count, false},
        {0x00201202, VR::IS, kPatient, Src::PatientSeries, Str::Count, false},
        {0x00201204, VR::IS, kPatient, Src::PatientInstances, Str::Count, false},
        {0x0020000D, VR::UI, kStudy, Src::Field, Str::StudyInstanceUid, true},
        {0x00080020, VR::DA, kStudy, Src::Field, Str::StudyDate, true},
        {0x00080030, VR::TM, kStudy, Src::Field, Str::StudyTime, true},
        {0x00080050, VR::SH, kStudy, Src::Field, Str::AccessionNumber, true},
        {0x00200010, VR::SH, kStudy, Src::Field, Str::StudyId, true},
        {0x00081030, VR::LO, kStudy, Src::Field, Str::StudyDescription, true},
        {0x00080090, VR::PN, kStudy, Src::Field, Str::ReferringPhysician, true},
        {0x00080061, VR::CS, kStudy, Src::Field, Str::ModalitiesInStudy, true},
        {0x00201206, VR::IS, kStudy, Src::StudySeries, Str::Count, true},
        {0x00201208, VR::IS, kStudy, Src::StudyInstances, Str::Count, true},
        {0x0020000E, VR::UI, kSeries, Src::Field, Str::SeriesInstanceUid, true},
        {0x00080060, VR::CS, kSeries, Src::Field, Str::Modality, true},
        {0x0008103E, VR::LO, kSeries, Src::Field, Str::SeriesDescription, true},
        {0x00200011, VR::IS, kSeries, Src::SeriesNumber, Str::Count, true},
        {0x00180015, VR::CS, kSeries, Src::Field, Str::BodyPartExamined, false},
        {0x00080021, VR::DA, kSeries, Src::Field, Str::SeriesDate, false},
        {0x00080031, VR::TM, kSeries, Src::Field, Str::SeriesTime, false},
        {0x00200052, VR::UI, kSeries, Src::Field, Str::FrameOfReferenceUid, false},
        {0x00201209, VR::IS, kSeries, Src::SeriesInstances, Str::Count, true},
        {0x00080018, VR::UI, kImage, Src::Field, Str::SopInstanceUid, true},
        {0x00080016, VR::UI, kImage, Src::Field, Str::SopClassUid, true},
        {0x00200013, VR::IS, kImage, Src::InstanceNumber, Str::Count, true},
        {0x00080023, VR::DA, kImage, Src::Field, Str::ContentDate, false},
        {0x00080033, VR::TM, kImage, Src::Field, Str::ContentTime, false},
        {0x00080008, VR::CS, kImage, Src::Field, Str::ImageType, false},
        {0x00280010, VR::US, kImage, Src::Rows, Str::Count, false},
        {0x00280011, VR::US, kImage, Src::Columns, Str::Count, false},
        {0x00280008, VR::IS, kImage, Src::Frames, Str::Count, false},
        {kRetrieveAeTitle, VR::AE, kPatient, Src::RetrieveAe, Str::Count, false},
        {kInstanceAvailability, VR::CS, kPatient, Src::Availability, Str::Count, false},
    };
    return t;
}

const KeyDef* key_def(Tag t) {
    for (const KeyDef& k : key_table())
        if (k.tag == t) return &k;
    return nullptr;
}

std::optional<int> level_of(const Dataset& id) {
    const auto v = id.get_string(kQueryRetrieveLevel);
    if (!v) return std::nullopt;
    if (*v == "PATIENT") return kPatient;
    if (*v == "STUDY") return kStudy;
    if (*v == "SERIES") return kSeries;
    if (*v == "IMAGE") return kImage;
    return std::nullopt;
}

bool patient_root(std::string_view cls) {
    return cls == net::sop::PatientRootFind || cls == net::sop::PatientRootMove || cls == net::sop::PatientRootGet;
}

store::Level store_level(int l) {
    switch (l) {
        case kPatient: return store::Level::Patient;
        case kStudy: return store::Level::Study;
        case kSeries: return store::Level::Series;
        default: return store::Level::Instance;
    }
}

bool has_wildcard_or_list(std::string_view v) {
    return v.find_first_of("*?\\") != std::string_view::npos;
}

std::vector<std::string> split_values(std::string_view v) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= v.size()) {
        const std::size_t k = v.find('\\', start);
        std::string one(v.substr(start, k == std::string_view::npos ? std::string_view::npos : k - start));
        while (!one.empty() && (one.back() == ' ' || one.back() == '\0')) one.pop_back();
        while (!one.empty() && one.front() == ' ') one.erase(one.begin());
        if (!one.empty()) out.push_back(std::move(one));
        if (k == std::string_view::npos) break;
        start = k + 1;
    }
    return out;
}

std::string value_of(const KeyDef& k, const IndexRecord& r, int level, const StoreQrOptions& opt) {
    switch (k.src) {
        case Src::Field: return r[k.field];
        case Src::PatientStudies: return level == kPatient ? std::to_string(r.num_studies) : std::string();
        case Src::PatientSeries: return level == kPatient ? std::to_string(r.num_series) : std::string();
        case Src::PatientInstances: return level == kPatient ? std::to_string(r.num_instances) : std::string();
        case Src::StudySeries: return level == kStudy ? std::to_string(r.num_series) : std::string();
        case Src::StudyInstances: return level == kStudy ? std::to_string(r.num_instances) : std::string();
        case Src::SeriesInstances: return level == kSeries ? std::to_string(r.num_instances) : std::string();
        case Src::SeriesNumber:
            return (r.flags & store::kHasSeriesNumber) ? std::to_string(r.series_number) : std::string();
        case Src::InstanceNumber:
            return (r.flags & store::kHasInstanceNumber) ? std::to_string(r.instance_number) : std::string();
        case Src::Rows: return r.rows ? std::to_string(r.rows) : std::string();
        case Src::Columns: return r.columns ? std::to_string(r.columns) : std::string();
        case Src::Frames: return r.number_of_frames ? std::to_string(r.number_of_frames) : std::string();
        case Src::RetrieveAe: return opt.retrieve_ae;
        case Src::Availability: return "ONLINE";
    }
    return {};
}

// The attributes of an index row at `level`, as a data set to match against.
Dataset candidate(const IndexRecord& r, int level, const StoreQrOptions& opt) {
    Dataset c;
    for (const KeyDef& k : key_table()) {
        if (k.level > level) continue;
        const std::string v = value_of(k, r, level, opt);
        if (v.empty()) continue;
        if (k.vr == VR::US) {
            const std::uint32_t n = k.src == Src::Rows ? r.rows : r.columns;
            (void)c.set_uint16(k.tag, static_cast<std::uint16_t>(std::min<std::uint32_t>(n, 0xFFFF)));
        } else {
            (void)c.set_string(k.tag, k.vr, v);
        }
    }
    return c;
}

// The identifier keys matched against candidates: keys the store cannot match on are left out
// (treated as universal, PS3.4 C.2.2.1.1: optional keys need not be supported for matching).
Dataset matching_keys(const Dataset& id, int level) {
    Dataset m;
    for (const Element& e : id.elements()) {
        const KeyDef* k = key_def(e.tag);
        if (!k || k->level > level || k->src == Src::RetrieveAe || k->src == Src::Availability) continue;
        m.put(e);
    }
    return m;
}

// Index rows at `level` that may match (a superset; query_matches decides), with exact keys
// pushed down to the store's indexes. One store query per value of a UID list.
Result<std::vector<IndexRecord>> candidates(const store::Store& s, const Dataset& id, int level) {
    store::QueryFilter base;
    base.level = store_level(level);
    const auto exact = [&](Tag t, int at) -> std::optional<std::string> {
        if (at > level) return std::nullopt;
        auto v = id.get_string(t);
        if (!v || v->empty() || has_wildcard_or_list(*v)) return std::nullopt;
        return v;
    };
    if (auto v = exact(0x00100020, kPatient)) base.patient_id = *v;
    if (auto v = exact(0x00080050, kStudy)) base.accession_number = *v;
    // The lowest-level UID key drives the lookup; a list of UIDs gives one query per UID.
    struct UidKey {
        Tag tag;
        int at;
        std::string store::QueryFilter::*field;
    };
    const UidKey uid_keys[] = {{0x00080018, kImage, &store::QueryFilter::sop_instance_uid},
                               {0x0020000E, kSeries, &store::QueryFilter::series_uid},
                               {0x0020000D, kStudy, &store::QueryFilter::study_uid}};
    std::vector<store::QueryFilter> filters;
    for (const UidKey& k : uid_keys) {
        if (k.at > level || !filters.empty()) continue;
        const auto raw = id.get_raw_string(k.tag);
        if (!raw) continue;
        const std::vector<std::string> uids = split_values(*raw);
        if (uids.empty()) continue;
        for (const std::string& u : uids) {
            if (u.find_first_of("*?") != std::string::npos) {
                filters.clear();
                break;
            }
            store::QueryFilter f = base;
            f.*(k.field) = u;
            filters.push_back(std::move(f));
        }
    }
    if (filters.empty()) filters.push_back(base);
    std::vector<IndexRecord> out;
    std::set<std::string> seen;
    for (const store::QueryFilter& f : filters) {
        auto r = s.query(f);
        if (!r) return r.take_error();
        for (IndexRecord& row : r->rows) {
            // The same row through two UIDs of a list: once.
            std::string key;
            switch (level) {
                case kPatient: key = store::Store::patient_key(row); break;
                case kStudy: key = row[Str::StudyInstanceUid]; break;
                case kSeries: key = row[Str::SeriesInstanceUid]; break;
                default: key = row[Str::SopInstanceUid]; break;
            }
            if (filters.size() > 1 && !seen.insert(key).second) continue;
            out.push_back(std::move(row));
        }
    }
    return out;
}

void decode_text(Dataset& ds, const charset::CharacterSet& inherited) {
    const charset::CharacterSet cs = charset::character_set_of(ds, &inherited);
    const bool plain = cs.scheme == charset::Scheme::Utf8 ||
                       (cs.scheme == charset::Scheme::Iso2022 && !cs.extensions && cs.terms.size() == 1 &&
                        cs.terms[0] == charset::Term::IR6);
    for (Element& e : ds.elements()) {
        if (e.is_sequence()) {
            for (Dataset& item : e.items) decode_text(item, cs);
            continue;
        }
        if (plain || !charset::vr_uses_character_set(e.vr)) continue;
        const std::string_view raw(reinterpret_cast<const char*>(e.value.data()), e.value.size());
        const std::string utf8 = charset::decode(raw, cs, charset::text_kind(e.vr));
        std::string padded = utf8;
        if (padded.size() % 2) padded.push_back(' ');
        e.value = Bytes::copy_of(reinterpret_cast<const std::uint8_t*>(padded.data()), padded.size());
    }
    if (!plain && ds.contains(tags::SpecificCharacterSet))
        (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
}

}  // namespace

Dataset identifier_utf8(const Dataset& ds) {
    Dataset out = ds;
    decode_text(out, charset::CharacterSet{});
    return out;
}

std::uint16_t store_find(const store::Store& s, std::string_view sop_class, const Dataset& identifier, bool relational,
                         const net::MatchOptions& mo, const StoreQrOptions& opt,
                         const std::function<bool(const Dataset&)>& emit, std::uint32_t& sent) {
    sent = 0;
    const std::optional<int> level = level_of(identifier);
    if (!level || (!patient_root(sop_class) && *level == kPatient)) return net::dimse_status::IdentifierDoesNotMatch;
    if (const std::uint16_t c = net::check_query_identifier(sop_class, identifier, relational, false);
        c != net::dimse_status::Success)
        return c;
    const Dataset id = identifier_utf8(identifier);
    const Dataset keys = matching_keys(id, *level);
    auto rows = candidates(s, id, *level);
    if (!rows) return net::dimse_status::ProcessingFailure;
    const std::string level_text = identifier.get_string(kQueryRetrieveLevel).value_or("");
    for (const IndexRecord& r : *rows) {
        const Dataset cand = candidate(r, *level, opt);
        if (!net::query_matches(keys, cand, mo)) continue;
        if (sent >= opt.max_find_results) return net::dimse_status::OutOfResources;
        Dataset rsp = net::query_response(id, cand);
        (void)rsp.set_string(kQueryRetrieveLevel, VR::CS, level_text);
        (void)rsp.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
        if (!emit(rsp)) return net::dimse_status::Success;
        ++sent;
    }
    return net::dimse_status::Success;
}

std::uint16_t store_retrieve(const store::Store& s, std::string_view sop_class, const Dataset& identifier,
                             bool relational, const StoreQrOptions& opt, std::vector<net::RetrieveItem>& items) {
    items.clear();
    const std::optional<int> level = level_of(identifier);
    const bool composite = sop_class == net::sop::CompositeInstanceRootMove || sop_class == net::sop::CompositeInstanceRootGet ||
                           sop_class == net::sop::CompositeInstanceRetrieveWithoutBulkDataGet;
    if (!level || (!patient_root(sop_class) && *level == kPatient)) return net::dimse_status::IdentifierDoesNotMatch;
    if (!composite)
        if (const std::uint16_t c = net::check_query_identifier(sop_class, identifier, relational, true);
            c != net::dimse_status::Success)
            return c;
    const Dataset id = identifier_utf8(identifier);
    const Dataset keys = matching_keys(id, *level);
    auto rows = candidates(s, id, *level);
    if (!rows) return net::dimse_status::ProcessingFailure;
    std::set<std::string> seen;
    const auto add = [&](const IndexRecord& inst) -> bool {
        if (!seen.insert(inst[Str::SopInstanceUid]).second) return true;
        if (items.size() >= opt.max_retrieve_instances) return false;
        net::RetrieveItem it;
        it.path = inst[Str::FilePath];
        items.push_back(std::move(it));
        return true;
    };
    for (const IndexRecord& r : *rows) {
        if (!net::query_matches(keys, candidate(r, *level, opt), net::MatchOptions{})) continue;
        std::vector<IndexRecord> insts;
        if (*level == kImage) {
            insts.push_back(r);
        } else {
            store::QueryFilter f;
            f.level = store::Level::Instance;
            if (*level == kSeries) f.series_uid = r[Str::SeriesInstanceUid];
            else if (*level == kStudy) f.study_uid = r[Str::StudyInstanceUid];
            else f.patient_id = r[Str::PatientId];
            if (*level == kPatient && f.patient_id.empty()) f.patient_name = r[Str::PatientName];
            auto q = s.query(f);
            if (!q) return net::dimse_status::ProcessingFailure;
            const std::string pkey = *level == kPatient ? store::Store::patient_key(r) : std::string();
            for (IndexRecord& x : q->rows)
                if (*level != kPatient || store::Store::patient_key(x) == pkey) insts.push_back(std::move(x));
        }
        for (const IndexRecord& x : insts)
            if (!add(x)) {
                items.clear();
                return net::dimse_status::OutOfResourcesMatches;
            }
    }
    return net::dimse_status::Success;
}

Result<Dataset> make_qr_identifier(int level, const QrKeyValues& keys) {
    static const char* const names[] = {"PATIENT", "STUDY", "SERIES", "IMAGE"};
    if (level < kPatient || level > kImage) return Status(Errc::InvalidArgument, "unknown level");
    Dataset ds;
    if (Status s = ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192"); !s) return s;
    if (Status s = ds.set_string(kQueryRetrieveLevel, VR::CS, names[level]); !s) return s;
    if (keys.return_keys)
        for (const KeyDef& k : key_table()) {
            if (!k.return_key || k.level > level) continue;
            const bool count = k.src == Src::StudySeries || k.src == Src::StudyInstances || k.src == Src::SeriesInstances;
            if (count && k.level != level) continue;  // "number of related" keys belong to their own level
            if (Status s = ds.set_string(k.tag, k.vr, ""); !s) return s;
        }
    for (const auto& [tag, value] : keys.values) {
        const KeyDef* k = key_def(tag);
        const dict::Entry* e = k ? nullptr : dict::lookup(tag);
        const VR vr = k ? k->vr : (e ? e->vr : VR::LO);
        if (Status s = ds.set_string(tag, vr, value); !s) return s;
    }
    return ds;
}

web::Store dicomweb_store(store::Store& s, store::DuplicatePolicy policy, bool read_only, const StoreQrOptions& opt) {
    web::Store w;
    store::Store* sp = &s;
    w.search = [sp, opt](const web::SearchQuery& q, std::vector<Dataset>& out) -> Status {
        const int level = q.level == web::QueryLevel::Study ? kStudy : q.level == web::QueryLevel::Series ? kSeries : kImage;
        QrKeyValues kv;
        kv.return_keys = true;
        Result<Dataset> id = make_qr_identifier(level, kv);
        if (!id) return id.take_error();
        if (!q.study_uid.empty()) (void)id->set_string(0x0020000D, VR::UI, q.study_uid);
        if (!q.series_uid.empty()) (void)id->set_string(0x0020000E, VR::UI, q.series_uid);
        for (const auto& [tag, value] : q.match) {
            const KeyDef* k = key_def(tag);
            const dict::Entry* e = k ? nullptr : dict::lookup(tag);
            if (!k && !e) return Status(Errc::Unsupported, "unknown attribute");
            if (Status st = id->set_string(tag, k ? k->vr : e->vr, value); !st) return Status(Errc::Unsupported, "bad value");
        }
        const auto include = [&](Tag t) {
            if (id->contains(t)) return;
            const KeyDef* k = key_def(t);
            const dict::Entry* e = k ? nullptr : dict::lookup(t);
            if (k || e) (void)id->set_string(t, k ? k->vr : e->vr, "");
        };
        for (Tag t : q.include_fields) include(t);
        if (q.include_all)
            for (const KeyDef& k : key_table())
                if (k.level <= level) include(k.tag);
        StoreQrOptions o = opt;
        if (q.limit) o.max_find_results = std::min<std::uint32_t>(o.max_find_results, q.offset + *q.limit);
        net::MatchOptions mo;
        mo.fuzzy_person_names = q.fuzzy_matching;
        std::uint32_t sent = 0;
        const std::uint16_t st = store_find(*sp, net::sop::StudyRootFind, *id, true, mo, o,
                                            [&](const Dataset& d) {
                                                Dataset r = d;
                                                (void)r.remove(kQueryRetrieveLevel);
                                                out.push_back(std::move(r));
                                                return true;
                                            },
                                            sent);
        if (st == net::dimse_status::IdentifierDoesNotMatch) return Status(Errc::Unsupported, "query not supported");
        if (st != net::dimse_status::Success && st != net::dimse_status::OutOfResources)
            return Status(Errc::IoError, "search failed");
        return Status::ok();
    };
    w.retrieve = [sp](const web::InstanceKey& k, std::vector<Bytes>& out) -> Status {
        store::QueryFilter f;
        f.level = store::Level::Instance;
        f.study_uid = k.study;
        f.series_uid = k.series;
        f.sop_instance_uid = k.instance;
        auto r = sp->query(f);
        if (!r) return r.take_error();
        if (r->rows.empty()) return Status(Errc::NotFound, "not found");
        for (const IndexRecord& row : r->rows) {
            if (row[Str::StudyInstanceUid] != k.study) continue;
            auto b = io::read_file(row[Str::FilePath]);
            if (!b) return b.take_error();
            out.push_back(std::move(b.value()));
        }
        if (out.empty()) return Status(Errc::NotFound, "not found");
        return Status::ok();
    };
    if (!read_only)
        w.store = [sp, policy](const DicomFile&, const Bytes& part10) -> std::uint16_t {
            auto r = sp->add_buffer(part10, policy);
            if (!r) return r.error().code == Errc::LimitExceeded ? net::dimse_status::OutOfResources : 0x0110;
            return 0;
        };
    return w;
}

}  // namespace siaara::capi
