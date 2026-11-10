// SIAARA DICOM Toolkit - network services answered from a local store (capi/store_services.hpp).
#include "capi/store_services.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
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

// R-128: the most values a list-of-UIDs key may hold.
constexpr std::size_t kMaxUidListValues = 10000;
// R-115: index rows are fetched from the store in pages of this size.
constexpr std::size_t kCandidatePage = 5000;

enum class Visit { Done, Stopped, Failed, TooManyUids };

// Visits the index rows at `level` that may match (a superset; query_matches decides), without
// ever holding more than one page of them (R-115): single-value keys - wild cards and date ranges
// included, where the store matches them itself - are pushed down to the store's indexes, and the
// rows are read page by page until `visit` returns false. A list of UIDs gives one indexed
// lookup per UID (at most kMaxUidListValues, R-128); `uid_key` then receives that key's tag: the
// rows are exactly the listed ones, so the caller need not match that key again.
Visit visit_candidates(const store::Store& s, const Dataset& id, int level, Tag* uid_key,
                       const std::function<bool(IndexRecord&)>& visit) {
    if (uid_key) *uid_key = 0;
    store::QueryFilter base;
    base.level = store_level(level);
    base.want_total = false;
    const auto single = [&](Tag t, int at) -> std::optional<std::string> {
        if (at > level) return std::nullopt;
        auto v = id.get_string(t);
        if (!v || v->empty() || v->find('\\') != std::string::npos) return std::nullopt;
        return v;
    };
    // Exact values use the store's indexes; a wild card narrows the walk (the store's own
    // matching of these two is not stricter than PS3.4 C.2.2.2.4, and query_matches has the last word).
    if (auto v = single(0x00100020, kPatient)) base.patient_id = *v;
    if (auto v = single(0x00080050, kStudy)) base.accession_number = *v;
    // Study Date: a single date or a range (C.2.2.2.5), as the store's own filter reads it.
    std::string study_date;
    if (auto v = single(0x00080020, kStudy))
        if (v->find_first_not_of("0123456789-") == std::string::npos) study_date = *v;
    // The lowest-level UID key drives the lookup; a list of UIDs gives one query per UID.
    struct UidKey {
        Tag tag;
        int at;
        std::string store::QueryFilter::*field;
    };
    const UidKey uid_keys[] = {{0x00080018, kImage, &store::QueryFilter::sop_instance_uid},
                               {0x0020000E, kSeries, &store::QueryFilter::series_uid},
                               {0x0020000D, kStudy, &store::QueryFilter::study_uid}};
    std::vector<std::string> uids;
    std::string store::QueryFilter::*uid_field = nullptr;
    for (const UidKey& k : uid_keys) {
        if (k.at > level || uid_field) continue;
        const auto raw = id.get_raw_string(k.tag);
        if (!raw) continue;
        std::vector<std::string> list = split_values(*raw);
        if (list.empty()) continue;
        if (list.size() > kMaxUidListValues) return Visit::TooManyUids;
        bool wild = false;
        for (const std::string& u : list) wild = wild || u.find_first_of("*?") != std::string::npos;
        if (wild) continue;
        uids = std::move(list);
        uid_field = k.field;
        if (uid_key) *uid_key = k.tag;
    }
    std::set<std::string> seen;  // the same row through two UIDs of a list: once
    const auto run = [&](store::QueryFilter f, bool with_date) -> Visit {
        if (with_date) f.study_date = study_date;
        f.limit = kCandidatePage;
        for (f.offset = 0;; f.offset += kCandidatePage) {
            auto r = s.query(f);
            if (!r) return Visit::Failed;
            for (IndexRecord& row : r->rows) {
                if (uids.size() > 1) {
                    std::string key;
                    switch (level) {
                        case kPatient: key = store::Store::patient_key(row); break;
                        case kStudy: key = row[Str::StudyInstanceUid]; break;
                        case kSeries: key = row[Str::SeriesInstanceUid]; break;
                        default: key = row[Str::SopInstanceUid]; break;
                    }
                    if (!seen.insert(std::move(key)).second) continue;
                }
                if (!visit(row)) return Visit::Stopped;
            }
            if (r->rows.size() < kCandidatePage) return Visit::Done;
        }
    };
    // A date the store's filter does not accept is simply not pushed down.
    const auto run_checked = [&](const store::QueryFilter& f) -> Visit {
        if (!study_date.empty()) {
            store::QueryFilter probe = f;
            probe.study_date = study_date;
            probe.limit = 1;
            if (s.query(probe)) return run(f, true);
        }
        return run(f, false);
    };
    if (!uid_field) return run_checked(base);
    for (const std::string& u : uids) {
        store::QueryFilter f = base;
        f.*uid_field = u;
        const Visit v = run_checked(f);
        if (v != Visit::Done) return v;
    }
    return Visit::Done;
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
    Dataset keys = matching_keys(id, *level);
    const std::string level_text = identifier.get_string(kQueryRetrieveLevel).value_or("");
    std::uint16_t status = net::dimse_status::Success;
    Tag uid_key = 0;
    bool first = true;
    // R-115: the index is walked page by page and the walk ends with the response that passes
    // max_find_results; the whole table is never copied.
    const Visit v = visit_candidates(s, id, *level, &uid_key, [&](IndexRecord& r) -> bool {
        if (first) {
            first = false;
            if (uid_key != 0) (void)keys.remove(uid_key);  // R-128: the rows are exactly the listed UIDs
        }
        const Dataset cand = candidate(r, *level, opt);
        if (!net::query_matches(keys, cand, mo)) return true;
        if (sent >= opt.max_find_results) {
            status = net::dimse_status::OutOfResources;
            return false;
        }
        Dataset rsp = net::query_response(id, cand);
        (void)rsp.set_string(kQueryRetrieveLevel, VR::CS, level_text);
        (void)rsp.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
        // T-P3-2 (PS3.4 C.4.1.1.3.2): a response carries Retrieve AE Title (0008,0054) or the
        // Storage Media File-Set ID / UID; this SCP has no file-sets, so every response names the
        // AE the instances can be retrieved from, whether or not the key was requested.
        if (!opt.retrieve_ae.empty()) (void)rsp.set_string(kRetrieveAeTitle, VR::AE, opt.retrieve_ae);
        if (!emit(rsp)) return false;
        ++sent;
        return true;
    });
    if (v == Visit::Failed) return net::dimse_status::ProcessingFailure;
    if (v == Visit::TooManyUids) return net::dimse_status::OutOfResources;
    return status;
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
    Dataset keys = matching_keys(id, *level);
    std::set<std::string> seen;
    std::uint16_t status = net::dimse_status::Success;
    const auto add = [&](const IndexRecord& inst) -> bool {
        if (!seen.insert(inst[Str::SopInstanceUid]).second) return true;
        if (items.size() >= opt.max_retrieve_instances) return false;
        net::RetrieveItem it;
        it.path = inst[Str::FilePath];
        items.push_back(std::move(it));
        return true;
    };
    Tag uid_key = 0;
    bool first = true;
    // R-115: rows page by page; the instances of each match are read with a limit of what the
    // retrieve may still take, so no more than max_retrieve_instances + 1 rows are ever copied.
    const Visit v = visit_candidates(s, id, *level, &uid_key, [&](IndexRecord& r) -> bool {
        if (first) {
            first = false;
            if (uid_key != 0) (void)keys.remove(uid_key);
        }
        if (!net::query_matches(keys, candidate(r, *level, opt), net::MatchOptions{})) return true;
        if (*level == kImage) {
            if (!add(r)) status = net::dimse_status::OutOfResourcesMatches;
            return status == net::dimse_status::Success;
        }
        store::QueryFilter f;
        f.level = store::Level::Instance;
        f.want_total = false;
        if (*level == kSeries) f.series_uid = r[Str::SeriesInstanceUid];
        else if (*level == kStudy) f.study_uid = r[Str::StudyInstanceUid];
        else f.patient_id = r[Str::PatientId];
        if (*level == kPatient && f.patient_id.empty()) f.patient_name = r[Str::PatientName];
        const std::string pkey = *level == kPatient ? store::Store::patient_key(r) : std::string();
        f.limit = kCandidatePage;
        for (f.offset = 0;; f.offset += kCandidatePage) {
            auto q = s.query(f);
            if (!q) {
                status = net::dimse_status::ProcessingFailure;
                return false;
            }
            for (const IndexRecord& x : q->rows)
                if ((*level != kPatient || store::Store::patient_key(x) == pkey) && !add(x)) {
                    status = net::dimse_status::OutOfResourcesMatches;
                    return false;
                }
            if (q->rows.size() < kCandidatePage) break;
        }
        return true;
    });
    if (v == Visit::Failed) status = net::dimse_status::ProcessingFailure;
    if (v == Visit::TooManyUids) status = net::dimse_status::OutOfResourcesMatches;
    if (status != net::dimse_status::Success) items.clear();
    return status;
}

Status make_qr_identifier(int level, const QrKeyValues& keys, Dataset& ds) {
    static const char* const names[] = {"PATIENT", "STUDY", "SERIES", "IMAGE"};
    if (level < kPatient || level > kImage) return Status(Errc::InvalidArgument, "unknown level");
    ds = Dataset{};
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
    return Status::ok();
}

web::Store dicomweb_store(store::Store& s, store::DuplicatePolicy policy, bool read_only, const StoreQrOptions& opt) {
    web::Store w;
    store::Store* sp = &s;
    w.search = [sp, opt](const web::SearchQuery& q, std::vector<Dataset>& out) -> Status {
        const int level = q.level == web::QueryLevel::Study ? kStudy : q.level == web::QueryLevel::Series ? kSeries : kImage;
        QrKeyValues kv;
        kv.return_keys = true;
        Dataset ident;
        if (Status ms = make_qr_identifier(level, kv, ident); !ms) return ms;
        Dataset* id = &ident;
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
    // R-110: the instances under a key from the index - path, size and transfer syntax - without
    // opening a file. The server answers 413 from the sizes, then maps one file at a time.
    w.locate = [sp](const web::InstanceKey& k, std::vector<web::StoredInstance>& out) -> Status {
        store::QueryFilter f;
        f.level = store::Level::Instance;
        f.study_uid = k.study;
        f.series_uid = k.series;
        f.sop_instance_uid = k.instance;
        f.want_total = false;
        f.limit = kCandidatePage;
        for (f.offset = 0;; f.offset += kCandidatePage) {
            auto r = sp->query(f);
            if (!r) return r.take_error();
            for (const IndexRecord& row : r->rows) {
                if (row[Str::StudyInstanceUid] != k.study) continue;
                if (out.size() >= 65535) return Status(Errc::LimitExceeded, "too many instances");
                web::StoredInstance inst;
                inst.path = row[Str::FilePath];
                inst.size = row.file_size;
                inst.transfer_syntax = row[Str::TransferSyntaxUid];
                out.push_back(std::move(inst));
            }
            if (r->rows.size() < kCandidatePage) break;
        }
        if (out.empty()) return Status(Errc::NotFound, "not found");
        return Status::ok();
    };
    if (!read_only)
        w.store = [sp, policy](const DicomFile&, const Bytes& part10) -> std::uint16_t {
            auto r = sp->add_buffer(part10, policy, 0, siaara::store::network_extract_options());
            if (!r) return r.error().code == Errc::LimitExceeded ? net::dimse_status::OutOfResources : 0x0110;
            return 0;
        };
    return w;
}

}  // namespace siaara::capi
