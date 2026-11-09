#include "store/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/dataset.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "store/common.hpp"
#include "store/path.hpp"
#include "store/text.hpp"

namespace siaara::store {

namespace fs = std::filesystem;

std::string instance_relative_path(std::string_view uid) {
    const auto h = sha256(uid.data(), uid.size());
    const std::string hex = to_hex(h.data(), 16);
    return "data/" + hex.substr(0, 2) + "/" + hex.substr(2, 2) + "/" + hex + ".dcm";
}

std::string Store::patient_key(const IndexRecord& r) {
    if (!r[Str::PatientId].empty()) return "I" + r[Str::PatientId] + '\x1F' + r[Str::IssuerOfPatientId];
    return "N" + name_forms(r[Str::PatientName])[0] + '\x1F' + r[Str::PatientBirthDate];
}

namespace {

constexpr char kJournalMagic[8] = {'S', 'I', 'A', 'A', 'R', 'A', 'J', '1'};
constexpr char kSnapshotMagic[8] = {'S', 'I', 'A', 'A', 'R', 'A', 'S', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kFrameHeader = 9;
constexpr std::uint32_t kMaxRecord = 64u << 20;

std::uint64_t now_seconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------- record codec
void encode_record(ByteWriter& w, const IndexRecord& r) {
    w.varint(kStrCount);
    for (const auto& s : r.s) w.str(s);
    w.u32(r.flags);
    w.i32(r.series_number);
    w.i32(r.instance_number);
    w.u32(r.number_of_frames);
    w.u32(r.rows);
    w.u32(r.columns);
    w.u16(r.bits_allocated);
    w.u16(r.bits_stored);
    w.u16(r.high_bit);
    w.u16(r.samples_per_pixel);
    w.u16(r.pixel_representation);
    w.u64(r.file_size);
    w.u64(r.pixel_data_offset);
    w.u64(r.pixel_data_length);
    for (double d : r.position) w.f64(d);
    for (double d : r.orientation) w.f64(d);
    for (double d : r.pixel_spacing) w.f64(d);
    w.f64(r.slice_thickness);
    w.u64(r.added_time);
}

bool decode_record(ByteReader& rd, IndexRecord& r) {
    const std::uint64_t n = rd.varint();
    if (!rd.ok() || n > 1024) return false;
    for (std::uint64_t i = 0; i < n; ++i) {
        std::string s = rd.str();
        if (i < kStrCount) r.s[static_cast<std::size_t>(i)] = std::move(s);
    }
    r.flags = rd.u32();
    r.series_number = rd.i32();
    r.instance_number = rd.i32();
    r.number_of_frames = rd.u32();
    r.rows = rd.u32();
    r.columns = rd.u32();
    r.bits_allocated = rd.u16();
    r.bits_stored = rd.u16();
    r.high_bit = rd.u16();
    r.samples_per_pixel = rd.u16();
    r.pixel_representation = rd.u16();
    r.file_size = rd.u64();
    r.pixel_data_offset = rd.u64();
    r.pixel_data_length = rd.u64();
    for (double& d : r.position) d = rd.f64();
    for (double& d : r.orientation) d = rd.f64();
    for (double& d : r.pixel_spacing) d = rd.f64();
    r.slice_thickness = rd.f64();
    r.added_time = rd.u64();
    return rd.ok();
}

void frame(std::vector<std::uint8_t>& out, char type, const std::vector<std::uint8_t>& payload) {
    std::uint8_t h[kFrameHeader];
    store_le32(h, static_cast<std::uint32_t>(payload.size()));
    const auto t = static_cast<std::uint8_t>(type);
    std::uint32_t c = crc32(&t, 1);
    c = crc32(payload.data(), payload.size(), c);
    store_le32(h + 4, c);
    h[8] = t;
    out.insert(out.end(), h, h + kFrameHeader);
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> header(const char magic[8], std::uint32_t generation) {
    std::vector<std::uint8_t> h(kHeaderSize);
    std::memcpy(h.data(), magic, 8);
    store_le32(h.data() + 8, kFormatVersion);
    store_le32(h.data() + 12, generation);
    return h;
}

// Iterates framed records. next() returns false at the end or at the first bad frame;
// good_end() is the offset just past the last good record.
class FrameReader {
public:
    FrameReader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n), pos_(kHeaderSize) {}
    bool next(char& type, const std::uint8_t*& payload, std::size_t& len) {
        if (n_ - pos_ < kFrameHeader) return false;
        const std::uint32_t l = load_le32(p_ + pos_);
        if (l > kMaxRecord || n_ - pos_ - kFrameHeader < l) return false;
        const std::uint8_t t = p_[pos_ + 8];
        std::uint32_t c = crc32(&t, 1);
        c = crc32(p_ + pos_ + kFrameHeader, l, c);
        if (c != load_le32(p_ + pos_ + 4)) return false;
        type = static_cast<char>(t);
        payload = p_ + pos_ + kFrameHeader;
        len = l;
        pos_ += kFrameHeader + l;
        return true;
    }
    std::size_t good_end() const noexcept { return pos_; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t pos_;
};

// ---------------------------------------------------------------- in-memory model
struct Patient;
struct Study;
struct Series;

constexpr Str kPatientFields[] = {Str::PatientId, Str::IssuerOfPatientId, Str::PatientName,
                                  Str::PatientBirthDate, Str::PatientSex};
constexpr Str kStudyFields[] = {Str::StudyInstanceUid, Str::StudyDate, Str::StudyTime, Str::AccessionNumber,
                                Str::StudyId, Str::StudyDescription, Str::ReferringPhysician};
constexpr Str kSeriesFields[] = {Str::SeriesInstanceUid, Str::Modality, Str::SeriesDescription,
                                 Str::BodyPartExamined, Str::SeriesDate, Str::SeriesTime,
                                 Str::FrameOfReferenceUid};

struct Patient {
    std::string key;
    std::array<std::string, 5> f;  // kPatientFields
    std::array<std::string, 3> forms;
    std::uint32_t birth_key = 0;
    std::vector<Study*> studies;
};

struct Study {
    std::array<std::string, 7> f;  // kStudyFields
    std::string desc_f, ref_f;
    std::uint32_t date_key = 0;
    Patient* patient = nullptr;
    std::vector<Series*> series;
    std::size_t instances = 0;
    const std::string& uid() const { return f[0]; }
};

struct Series {
    std::array<std::string, 7> f;  // kSeriesFields
    std::string desc_f;
    std::int32_t number = 0;
    bool has_number = false;
    Study* study = nullptr;
    std::vector<struct Instance*> instances;
    const std::string& uid() const { return f[0]; }
};

struct Instance {
    std::string uid, content_date, content_time, path;  // path empty = instance_relative_path(uid)
    const std::string* sop_class = nullptr;
    const std::string* ts = nullptr;
    const std::string* photometric = nullptr;
    const std::string* image_type = nullptr;
    const std::string* charset = nullptr;
    Series* series = nullptr;
    std::uint32_t flags = 0;
    std::int32_t instance_number = 0;
    std::uint32_t frames = 0, rows = 0, columns = 0;
    std::uint16_t bits_allocated = 0, bits_stored = 0, high_bit = 0, samples = 0, pixel_rep = 0;
    std::uint64_t file_size = 0, pd_offset = 0, pd_length = 0, added = 0;
    double position[3] = {0, 0, 0};
    double orientation[6] = {0, 0, 0, 0, 0, 0};
    double spacing[2] = {0, 0};
    double thickness = 0;

    std::string stored_path() const { return path.empty() ? instance_relative_path(uid) : path; }
    double slice_pos() const {
        if ((flags & (kHasPosition | kHasOrientation)) != (kHasPosition | kHasOrientation)) return 0;
        const double* r = orientation;
        const double* c = orientation + 3;
        const double n[3] = {r[1] * c[2] - r[2] * c[1], r[2] * c[0] - r[0] * c[2], r[0] * c[1] - r[1] * c[0]};
        return n[0] * position[0] + n[1] * position[1] + n[2] * position[2];
    }
};

template <class T>
void erase_ptr(std::vector<T*>& v, T* p) {
    auto it = std::find(v.begin(), v.end(), p);
    if (it != v.end()) {
        *it = v.back();
        v.pop_back();
    }
}

template <class M, class K, class V>
void multimap_erase(M& m, const K& k, V* p) {
    auto range = m.equal_range(k);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == p) {
            m.erase(it);
            return;
        }
}

// ---------------------------------------------------------------- ordering
bool patient_less(const Patient* a, const Patient* b) {
    if (a->forms[0] != b->forms[0]) return a->forms[0] < b->forms[0];
    return a->key < b->key;
}
bool study_less(const Study* a, const Study* b) {  // newest first
    if (a->date_key != b->date_key) return a->date_key > b->date_key;
    if (a->f[2] != b->f[2]) return a->f[2] > b->f[2];
    return a->uid() < b->uid();
}
bool series_less(const Series* a, const Series* b) {
    if (a->has_number != b->has_number) return a->has_number;
    if (a->number != b->number) return a->number < b->number;
    if (a->f[4] != b->f[4]) return a->f[4] < b->f[4];
    if (a->f[5] != b->f[5]) return a->f[5] < b->f[5];
    return a->uid() < b->uid();
}
bool instance_less_in_series(const Instance* a, const Instance* b) {
    const bool ha = (a->flags & kHasInstanceNumber) != 0, hb = (b->flags & kHasInstanceNumber) != 0;
    if (ha != hb) return ha;
    if (a->instance_number != b->instance_number) return a->instance_number < b->instance_number;
    const double pa = a->slice_pos(), pb = b->slice_pos();
    if (pa != pb) return pa < pb;
    return a->uid < b->uid;
}
bool instance_less(const Instance* a, const Instance* b) {
    if (a->series != b->series) {
        if (a->series->study != b->series->study) return study_less(a->series->study, b->series->study);
        return series_less(a->series, b->series);
    }
    return instance_less_in_series(a, b);
}

struct StudyOrder {
    using is_transparent = void;
    bool operator()(const Study* a, const Study* b) const { return study_less(a, b); }
    bool operator()(const Study* a, std::uint32_t d) const { return a->date_key > d; }
    bool operator()(std::uint32_t d, const Study* b) const { return d > b->date_key; }
};
struct PatientOrder {
    bool operator()(const Patient* a, const Patient* b) const { return patient_less(a, b); }
};

struct State {
    std::unordered_map<std::string_view, std::unique_ptr<Instance>> instances;
    std::unordered_map<std::string_view, std::unique_ptr<Series>> series;
    std::unordered_map<std::string_view, std::unique_ptr<Study>> studies;
    std::unordered_map<std::string_view, std::unique_ptr<Patient>> patients;
    // secondary indexes; keys view strings owned by the nodes (re-indexed whenever they change)
    std::unordered_multimap<std::string_view, Patient*> by_patient_id;
    std::multimap<std::string_view, Patient*> by_name_form;
    std::unordered_multimap<std::string_view, Study*> by_accession;
    std::set<Study*, StudyOrder> ordered_studies;     // result order: newest first
    std::set<Patient*, PatientOrder> ordered_patients; // result order: by folded name
    std::unordered_set<std::string> pool;
    std::uint64_t data_bytes = 0;

    const std::string* intern(const std::string& s) { return &*pool.insert(s).first; }

    // While bulk loading (at open) the ordered indexes are left empty and built once, sorted,
    // by end_bulk(): inserting a million nodes one by one is dominated by cache misses.
    bool bulk = false;

    void index_patient(Patient* p) {
        by_patient_id.emplace(std::string_view(p->f[0]), p);
        if (bulk) return;
        for (const auto& form : p->forms)
            if (!form.empty()) by_name_form.emplace(std::string_view(form), p);
        ordered_patients.insert(p);
    }
    void unindex_patient(Patient* p) {
        multimap_erase(by_patient_id, std::string_view(p->f[0]), p);
        if (bulk) return;
        for (const auto& form : p->forms)
            if (!form.empty()) multimap_erase(by_name_form, std::string_view(form), p);
        ordered_patients.erase(p);
    }
    void index_study(Study* s) {
        if (!s->f[3].empty()) by_accession.emplace(std::string_view(s->f[3]), s);
        if (!bulk) ordered_studies.insert(s);
    }
    void unindex_study(Study* s) {
        if (!s->f[3].empty()) multimap_erase(by_accession, std::string_view(s->f[3]), s);
        if (!bulk) ordered_studies.erase(s);
    }
    void begin_bulk() {
        bulk = true;
        by_name_form.clear();
        ordered_patients.clear();
        ordered_studies.clear();
    }
    void end_bulk() {
        bulk = false;
        std::vector<Patient*> ps;
        ps.reserve(patients.size());
        for (auto& kv : patients) ps.push_back(kv.second.get());
        std::sort(ps.begin(), ps.end(), patient_less);
        for (Patient* p : ps) ordered_patients.insert(ordered_patients.end(), p);
        std::vector<std::pair<std::string_view, Patient*>> forms;
        forms.reserve(ps.size());
        for (Patient* p : ps)
            for (const auto& form : p->forms)
                if (!form.empty()) forms.emplace_back(form, p);
        std::stable_sort(forms.begin(), forms.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& f : forms) by_name_form.emplace_hint(by_name_form.end(), f.first, f.second);
        std::vector<Study*> ss;
        ss.reserve(studies.size());
        for (auto& kv : studies) ss.push_back(kv.second.get());
        std::sort(ss.begin(), ss.end(), study_less);
        for (Study* s : ss) ordered_studies.insert(ordered_studies.end(), s);
    }

    static void set_patient_fields(Patient& p, const IndexRecord& r, bool only_nonempty) {
        for (std::size_t i = 0; i < 5; ++i)
            if (!only_nonempty || !r[kPatientFields[i]].empty()) p.f[i] = r[kPatientFields[i]];
        p.forms = name_forms(p.f[2]);
        p.birth_key = date_key(p.f[3]);
    }
    static void set_study_fields(Study& s, const IndexRecord& r, bool only_nonempty) {
        for (std::size_t i = 1; i < 7; ++i)
            if (!only_nonempty || !r[kStudyFields[i]].empty()) s.f[i] = r[kStudyFields[i]];
        s.desc_f = fold(s.f[5]);
        s.ref_f = fold_name_pattern(s.f[6]);
        s.date_key = date_key(s.f[1]);
    }
    static void set_series_fields(Series& e, const IndexRecord& r, bool only_nonempty) {
        for (std::size_t i = 1; i < 7; ++i)
            if (!only_nonempty || !r[kSeriesFields[i]].empty()) e.f[i] = r[kSeriesFields[i]];
        e.desc_f = fold(e.f[2]);
        if (r.flags & kHasSeriesNumber) {
            e.number = r.series_number;
            e.has_number = true;
        }
    }

    // Removes one instance and any parents left empty.
    void remove_instance(Instance* in) {
        Series* e = in->series;
        Study* s = e->study;
        Patient* p = s->patient;
        data_bytes -= std::min(data_bytes, in->file_size);
        erase_ptr(e->instances, in);
        --s->instances;
        instances.erase(in->uid);  // destroys `in`
        if (!e->instances.empty()) return;
        erase_ptr(s->series, e);
        series.erase(e->uid());
        if (!s->series.empty()) return;
        erase_ptr(p->studies, s);
        unindex_study(s);
        studies.erase(s->uid());
        if (!p->studies.empty()) return;
        unindex_patient(p);
        patients.erase(p->key);
    }

    // Adds or replaces. `update_parents`: latest non-empty values win for existing parents.
    // Returns true when an instance with this UID was replaced.
    bool apply_add(const IndexRecord& r, bool update_parents) {
        const std::string& uid = r[Str::SopInstanceUid];
        bool replaced = false;
        if (auto it = instances.find(uid); it != instances.end()) {
            remove_instance(it->second.get());
            replaced = true;
        }
        Series* e = nullptr;
        if (auto it = series.find(r[Str::SeriesInstanceUid]); it != series.end()) {
            e = it->second.get();
            if (update_parents) set_series_fields(*e, r, true);
        }
        Study* s = nullptr;
        if (e) {
            s = e->study;
        } else if (auto it = studies.find(r[Str::StudyInstanceUid]); it != studies.end()) {
            s = it->second.get();
        }
        if (s && update_parents) {
            unindex_study(s);
            set_study_fields(*s, r, true);
            index_study(s);
        }
        Patient* p = nullptr;
        if (s) {
            p = s->patient;
        } else {
            const std::string key = Store::patient_key(r);
            if (auto it = patients.find(key); it != patients.end()) p = it->second.get();
        }
        if (!p) {
            auto np = std::make_unique<Patient>();
            np->key = Store::patient_key(r);
            set_patient_fields(*np, r, false);
            p = np.get();
            patients.emplace(p->key, std::move(np));
            index_patient(p);
        } else if (update_parents && Store::patient_key(r) == p->key) {
            // Only a record of the same patient updates the patient's attributes. A record whose
            // Study UID belongs to another patient must not rewrite that patient's identifying
            // fields: they would no longer match its key, two patients could end up with the same
            // identity, and the next snapshot would not load.
            unindex_patient(p);
            set_patient_fields(*p, r, true);
            index_patient(p);
        }
        if (!s) {
            auto ns = std::make_unique<Study>();
            ns->f[0] = r[Str::StudyInstanceUid];
            set_study_fields(*ns, r, false);
            ns->patient = p;
            s = ns.get();
            studies.emplace(s->uid(), std::move(ns));
            p->studies.push_back(s);
            index_study(s);
        }
        if (!e) {
            auto ne = std::make_unique<Series>();
            ne->f[0] = r[Str::SeriesInstanceUid];
            set_series_fields(*ne, r, false);
            ne->study = s;
            e = ne.get();
            series.emplace(e->uid(), std::move(ne));
            s->series.push_back(e);
        }
        attach_instance(e, r);
        return replaced;
    }

    // Creates the instance node under `e`. FilePath: absolute path when referenced, otherwise
    // empty for the derived name (instance_relative_path) or the actual relative path.
    Instance* attach_instance(Series* e, const IndexRecord& r) {
        Study* s = e->study;
        auto in = std::make_unique<Instance>();
        in->uid = r[Str::SopInstanceUid];
        in->content_date = r[Str::ContentDate];
        in->content_time = r[Str::ContentTime];
        in->path = r[Str::FilePath];
        in->sop_class = intern(r[Str::SopClassUid]);
        in->ts = intern(r[Str::TransferSyntaxUid]);
        in->photometric = intern(r[Str::PhotometricInterpretation]);
        in->image_type = intern(r[Str::ImageType]);
        in->charset = intern(r[Str::SpecificCharacterSet]);
        in->series = e;
        in->flags = r.flags & ~(kHasSeriesNumber);
        in->instance_number = r.instance_number;
        in->frames = r.number_of_frames;
        in->rows = r.rows;
        in->columns = r.columns;
        in->bits_allocated = r.bits_allocated;
        in->bits_stored = r.bits_stored;
        in->high_bit = r.high_bit;
        in->samples = r.samples_per_pixel;
        in->pixel_rep = r.pixel_representation;
        in->file_size = r.file_size;
        in->pd_offset = r.pixel_data_offset;
        in->pd_length = r.pixel_data_length;
        in->added = r.added_time;
        std::memcpy(in->position, r.position, sizeof in->position);
        std::memcpy(in->orientation, r.orientation, sizeof in->orientation);
        std::memcpy(in->spacing, r.pixel_spacing, sizeof in->spacing);
        in->thickness = r.slice_thickness;
        e->instances.push_back(in.get());
        ++s->instances;
        data_bytes += in->file_size;
        Instance* raw = in.get();
        instances.emplace(raw->uid, std::move(in));
        return raw;
    }
};

// ---------------------------------------------------------------- records out
void fill_patient(const Patient& p, IndexRecord& r) {
    for (std::size_t i = 0; i < 5; ++i) r[kPatientFields[i]] = p.f[i];
}
std::string modalities_of(const Study& s) {
    std::vector<std::string_view> m;
    for (const Series* e : s.series)
        if (!e->f[1].empty() && std::find(m.begin(), m.end(), e->f[1]) == m.end()) m.push_back(e->f[1]);
    std::sort(m.begin(), m.end());
    std::string out;
    for (auto v : m) {
        if (!out.empty()) out.push_back('\\');
        out += v;
    }
    return out;
}
void fill_study(const Study& s, IndexRecord& r) {
    fill_patient(*s.patient, r);
    for (std::size_t i = 0; i < 7; ++i) r[kStudyFields[i]] = s.f[i];
    r[Str::ModalitiesInStudy] = modalities_of(s);
}
void fill_series(const Series& e, IndexRecord& r) {
    fill_study(*e.study, r);
    for (std::size_t i = 0; i < 7; ++i) r[kSeriesFields[i]] = e.f[i];
    if (e.has_number) {
        r.series_number = e.number;
        r.flags |= kHasSeriesNumber;
    }
}
void fill_instance_only(const Instance& in, IndexRecord& r) {
    r[Str::SopInstanceUid] = in.uid;
    r[Str::ContentDate] = in.content_date;
    r[Str::ContentTime] = in.content_time;
    r[Str::SopClassUid] = *in.sop_class;
    r[Str::TransferSyntaxUid] = *in.ts;
    r[Str::PhotometricInterpretation] = *in.photometric;
    r[Str::ImageType] = *in.image_type;
    r[Str::SpecificCharacterSet] = *in.charset;
    r.flags |= in.flags;
    r.instance_number = in.instance_number;
    r.number_of_frames = in.frames;
    r.rows = in.rows;
    r.columns = in.columns;
    r.bits_allocated = in.bits_allocated;
    r.bits_stored = in.bits_stored;
    r.high_bit = in.high_bit;
    r.samples_per_pixel = in.samples;
    r.pixel_representation = in.pixel_rep;
    r.file_size = in.file_size;
    r.pixel_data_offset = in.pd_offset;
    r.pixel_data_length = in.pd_length;
    r.added_time = in.added;
    std::memcpy(r.position, in.position, sizeof r.position);
    std::memcpy(r.orientation, in.orientation, sizeof r.orientation);
    std::memcpy(r.pixel_spacing, in.spacing, sizeof r.pixel_spacing);
    r.slice_thickness = in.thickness;
}

std::size_t series_count(const Patient& p) {
    std::size_t n = 0;
    for (const Study* s : p.studies) n += s->series.size();
    return n;
}
std::size_t instance_count(const Patient& p) {
    std::size_t n = 0;
    for (const Study* s : p.studies) n += s->instances;
    return n;
}

// ---------------------------------------------------------------- compiled filter
struct Compiled {
    std::string name, pid, sex, acc, sdesc, ref, edesc, body, study_uid, series_uid, sop_uid;
    std::vector<std::string> modalities;
    std::uint32_t birth_lo = 0, birth_hi = 0, date_lo = 0, date_hi = 0;
    bool birth = false, date = false;
    bool patient_f = false, study_f = false, series_f = false, instance_f = false;

    static bool active(const std::string& p) { return !p.empty() && p != "*"; }

    Status compile(const QueryFilter& q) {
        if (active(q.patient_name)) name = fold_name_pattern(q.patient_name);
        if (active(q.patient_id)) pid = q.patient_id;
        if (active(q.patient_sex)) sex = q.patient_sex;
        if (active(q.accession_number)) acc = q.accession_number;
        if (active(q.study_description)) sdesc = fold(q.study_description);
        if (active(q.referring_physician)) ref = fold_name_pattern(q.referring_physician);
        if (active(q.series_description)) edesc = fold(q.series_description);
        if (active(q.body_part)) body = q.body_part;
        study_uid = q.study_uid;
        series_uid = q.series_uid;
        sop_uid = q.sop_instance_uid;
        if (active(q.modality)) {
            std::size_t start = 0;
            const std::string& m = q.modality;
            for (std::size_t i = 0; i <= m.size(); ++i)
                if (i == m.size() || m[i] == '\\') {
                    if (i > start) modalities.push_back(m.substr(start, i - start));
                    start = i + 1;
                }
        }
        if (!q.patient_birth_date.empty()) {
            if (!parse_date_range(q.patient_birth_date, birth_lo, birth_hi))
                return Status(Errc::InvalidArgument, "malformed birth date range");
            birth = true;
        }
        if (!q.study_date.empty()) {
            if (!parse_date_range(q.study_date, date_lo, date_hi))
                return Status(Errc::InvalidArgument, "malformed study date range");
            date = true;
        }
        patient_f = !name.empty() || !pid.empty() || !sex.empty() || birth;
        study_f = !acc.empty() || !sdesc.empty() || !ref.empty() || !study_uid.empty() || date;
        series_f = !modalities.empty() || !edesc.empty() || !body.empty() || !series_uid.empty();
        instance_f = !sop_uid.empty();
        return Status::ok();
    }

    bool patient_ok(const Patient& p) const {
        if (!pid.empty() && !wildcard_match(pid, p.f[0])) return false;
        if (!sex.empty() && !wildcard_match(sex, p.f[4])) return false;
        if (birth && (p.birth_key < birth_lo || p.birth_key > birth_hi || p.birth_key == 0)) return false;
        if (!name.empty()) {
            bool any = false;
            for (const auto& form : p.forms)
                if (!form.empty() && wildcard_match(name, form)) {
                    any = true;
                    break;
                }
            if (!any) return false;
        }
        return true;
    }
    bool study_ok(const Study& s) const {
        if (!study_uid.empty() && s.uid() != study_uid) return false;
        if (date && (s.date_key == 0 || s.date_key < date_lo || s.date_key > date_hi)) return false;
        if (!acc.empty() && !wildcard_match(acc, s.f[3])) return false;
        if (!sdesc.empty() && !wildcard_match(sdesc, s.desc_f)) return false;
        if (!ref.empty() && !wildcard_match(ref, s.ref_f)) return false;
        return true;
    }
    bool series_ok(const Series& e) const {
        if (!series_uid.empty() && e.uid() != series_uid) return false;
        if (!modalities.empty()) {
            bool any = false;
            for (const auto& m : modalities)
                if (wildcard_match(m, e.f[1])) {
                    any = true;
                    break;
                }
            if (!any) return false;
        }
        if (!edesc.empty() && !wildcard_match(edesc, e.desc_f)) return false;
        if (!body.empty() && !wildcard_match(body, e.f[3])) return false;
        return true;
    }
};

// Path of p relative to root with '/' separators, as UTF-8.
std::string rel_generic(const fs::path& p, const fs::path& root) {
    const std::u8string g = p.lexically_relative(root).generic_u8string();
    std::string out(g.size(), '\0');
    for (std::size_t i = 0; i < g.size(); ++i) out[i] = static_cast<char>(g[i]);
    return out;
}

std::string literal_prefix(const std::string& p) {
    const std::size_t k = p.find_first_of("*?");
    return p.substr(0, k);
}

}  // namespace

// ---------------------------------------------------------------- Store::Impl
struct Store::Impl {
    std::string root_utf8;
    fs::path root, index_dir, data_dir, journal_path, snapshot_path;
    StoreOptions opt;
    LockFile lock;
    OsFile journal;
    State st;
    std::uint32_t generation = 0;
    StoreStats counters;  // journal/snapshot counters only
    mutable std::shared_mutex rw;
    mutable std::mutex writer;  // serialises changes; also guards `counters` and `generation`
    std::mutex cb_mutex;
    EventCallback callback;

    fs::path abs_path(const Instance& in) const {
        if (in.flags & kReferenced) return u8path(in.path);
        return root / u8path(in.stored_path());
    }

    void emit(const std::vector<Event>& events) {
        if (events.empty()) return;
        EventCallback cb;
        {
            std::lock_guard<std::mutex> g(cb_mutex);
            cb = callback;
        }
        if (!cb) return;
        for (const auto& e : events) cb(e);
    }

    // ---- journal
    Status journal_reset(std::uint32_t gen) {
        journal.close();
        const auto h = header(kJournalMagic, gen);
        if (Status s = write_replace(journal_path, h.data(), h.size(), true); !s) return s;
        auto f = OsFile::open(journal_path, OsFile::Mode::ReadWrite);
        if (!f) return f.take_error();
        journal = std::move(*f);
        counters.journal_bytes = kHeaderSize;
        counters.journal_records = 0;
        return Status::ok();
    }

    Status journal_append(const std::vector<std::uint8_t>& framed, std::uint64_t records) {
        if (Status s = journal.append(framed.data(), framed.size()); !s) return s;
        if (opt.durable)
            if (Status s = journal.sync(); !s) return s;
        counters.journal_bytes += framed.size();
        counters.journal_records += records;
        return Status::ok();
    }

    // ---- snapshot
    Status write_snapshot(std::uint32_t gen) {
        fs::path tmp = snapshot_path;
        tmp += u8path(".siaara-tmp");
        auto f = OsFile::open(tmp, OsFile::Mode::CreateTruncate);
        if (!f) return f.take_error();
        std::vector<std::uint8_t> buf = header(kSnapshotMagic, gen);
        std::uint64_t total = 0;
        auto flush = [&](bool force) -> Status {
            if (buf.size() < (1u << 20) && !force) return Status::ok();
            total += buf.size();
            Status s = f->append(buf.data(), buf.size());
            buf.clear();
            return s;
        };
        std::uint32_t pn = 0, sn = 0, en = 0, in_count = 0;
        Status s;
        for (const auto& [pk, p] : st.patients) {
            ByteWriter w;
            for (const auto& v : p->f) w.str(v);
            frame(buf, 'P', w.buf);
            const std::uint32_t pord = pn++;
            for (const Study* study : p->studies) {
                ByteWriter ws;
                ws.u32(pord);
                for (const auto& v : study->f) ws.str(v);
                frame(buf, 'S', ws.buf);
                const std::uint32_t sord = sn++;
                for (const Series* e : study->series) {
                    ByteWriter we;
                    we.u32(sord);
                    for (const auto& v : e->f) we.str(v);
                    we.u8(e->has_number ? 1 : 0);
                    we.i32(e->number);
                    frame(buf, 'E', we.buf);
                    const std::uint32_t eord = en++;
                    for (const Instance* in : e->instances) {
                        ByteWriter wi;
                        wi.u32(eord);
                        IndexRecord ri;
                        fill_instance_only(*in, ri);
                        ri[Str::FilePath] = in->path;
                        encode_record(wi, ri);
                        frame(buf, 'I', wi.buf);
                        ++in_count;
                    }
                    if (s = flush(false); !s) break;
                }
                if (!s) break;
            }
            if (!s) break;
        }
        if (s) {
            ByteWriter wz;
            wz.u32(pn);
            wz.u32(sn);
            wz.u32(en);
            wz.u32(in_count);
            frame(buf, 'Z', wz.buf);
            s = flush(true);
        }
        if (s) s = f->sync();
        f->close();
        std::error_code ec;
        if (!s) {
            fs::remove(tmp, ec);
            return s;
        }
        fs::rename(tmp, snapshot_path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return Status(Errc::IoError, "cannot replace snapshot");
        }
        sync_dir(index_dir);
        counters.snapshot_bytes = total;
        return Status::ok();
    }

    Status load_snapshot(State& out, std::uint32_t& gen) {
        std::error_code ec;
        if (!fs::exists(snapshot_path, ec)) {
            gen = 0;
            counters.snapshot_bytes = 0;
            return Status::ok();
        }
        auto bytes = io::map_file(to_utf8(snapshot_path));
        if (!bytes) return bytes.take_error();
        const std::uint8_t* p = bytes->data();
        const std::size_t n = bytes->size();
        if (n < kHeaderSize || std::memcmp(p, kSnapshotMagic, 8) != 0)
            return Status(Errc::Malformed, "index snapshot is not a SIAARA snapshot");
        if (load_le32(p + 8) != kFormatVersion) return Status(Errc::Unsupported, "unsupported snapshot version");
        gen = load_le32(p + 12);
        counters.snapshot_bytes = n;
        // Instance records take at least ~250 bytes: size the map once instead of rehashing.
        out.instances.reserve(n / 250);
        FrameReader fr(p, n);
        std::vector<Patient*> pv;
        std::vector<Study*> sv;
        std::vector<Series*> ev;
        char type = 0;
        const std::uint8_t* pl = nullptr;
        std::size_t len = 0;
        bool done = false;
        std::uint64_t ninst = 0;
        const Status bad(Errc::Malformed, "index snapshot record is malformed");
        while (fr.next(type, pl, len)) {
            ByteReader rd(pl, len);
            if (type == 'Z') {
                const std::uint32_t a = rd.u32(), b = rd.u32(), c = rd.u32(), d = rd.u32();
                if (!rd.ok() || a != pv.size() || b != sv.size() || c != ev.size() || d != ninst)
                    return Status(Errc::Malformed, "index snapshot counts do not match");
                done = true;
                break;
            }
            std::uint32_t parent = 0;
            if (type != 'P') parent = rd.u32();
            IndexRecord r;
            switch (type) {
                case 'P': {
                    for (Str k : kPatientFields) r[k] = rd.str();
                    if (!rd.ok() || !rd.at_end()) return bad;
                    auto np = std::make_unique<Patient>();
                    np->key = Store::patient_key(r);
                    State::set_patient_fields(*np, r, false);
                    Patient* q = np.get();
                    if (!out.patients.emplace(q->key, std::move(np)).second) return bad;
                    out.index_patient(q);
                    pv.push_back(q);
                    break;
                }
                case 'S': {
                    for (Str k : kStudyFields) r[k] = rd.str();
                    if (!rd.ok() || !rd.at_end() || parent >= pv.size()) return bad;
                    auto ns = std::make_unique<Study>();
                    ns->f[0] = r[Str::StudyInstanceUid];
                    State::set_study_fields(*ns, r, false);
                    ns->patient = pv[parent];
                    Study* q = ns.get();
                    if (!out.studies.emplace(q->uid(), std::move(ns)).second) return bad;
                    pv[parent]->studies.push_back(q);
                    out.index_study(q);
                    sv.push_back(q);
                    break;
                }
                case 'E': {
                    for (Str k : kSeriesFields) r[k] = rd.str();
                    if (rd.u8()) r.flags |= kHasSeriesNumber;
                    r.series_number = rd.i32();
                    if (!rd.ok() || !rd.at_end() || parent >= sv.size()) return bad;
                    auto ne = std::make_unique<Series>();
                    ne->f[0] = r[Str::SeriesInstanceUid];
                    State::set_series_fields(*ne, r, false);
                    ne->study = sv[parent];
                    Series* q = ne.get();
                    if (!out.series.emplace(q->uid(), std::move(ne)).second) return bad;
                    sv[parent]->series.push_back(q);
                    ev.push_back(q);
                    break;
                }
                case 'I': {
                    if (!decode_record(rd, r) || !rd.at_end() || parent >= ev.size()) return bad;
                    if (r[Str::SopInstanceUid].empty() || out.instances.count(r[Str::SopInstanceUid])) return bad;
                    out.attach_instance(ev[parent], r);
                    ++ninst;
                    break;
                }
                default: return Status(Errc::Malformed, "index snapshot has an unknown record");
            }
        }
        for (const Series* e : ev)
            if (e->instances.empty()) return Status(Errc::Malformed, "index snapshot has an empty series");
        if (!done || fr.good_end() != n) return Status(Errc::Malformed, "index snapshot is truncated or corrupt");
        return Status::ok();
    }

    Status replay_journal(State& out, std::uint32_t snapshot_gen) {
        std::error_code ec;
        const bool exists = fs::exists(journal_path, ec);
        std::uint64_t size = 0;
        if (exists) size = fs::file_size(journal_path, ec);
        if (!exists || ec || size < kHeaderSize) {
            // missing, or the header itself was torn while being created
            counters.recovered_bytes = exists ? size : 0;
            return journal_reset(snapshot_gen);
        }
        std::size_t good_end = 0;
        bool stale = false;
        {
            // Nothing may replace or truncate the journal while this mapping exists: on Windows a
            // mapped file cannot be renamed over (MoveFileEx fails), so act after the scope ends.
            auto bytes = io::map_file(to_utf8(journal_path));
            if (!bytes) return bytes.take_error();
            const std::uint8_t* p = bytes->data();
            const std::size_t n = bytes->size();
            if (std::memcmp(p, kJournalMagic, 8) != 0) return Status(Errc::Malformed, "index journal is not a SIAARA journal");
            if (load_le32(p + 8) != kFormatVersion) return Status(Errc::Unsupported, "unsupported journal version");
            const std::uint32_t jgen = load_le32(p + 12);
            if (jgen > snapshot_gen) return Status(Errc::Malformed, "index journal is newer than the snapshot");
            stale = jgen < snapshot_gen;  // already contained in the snapshot: do not replay
            FrameReader fr(p, stale ? kHeaderSize : n);  // a stale journal yields no records
            char type = 0;
            const std::uint8_t* pl = nullptr;
            std::size_t len = 0;
            std::uint64_t applied = 0;
            while (fr.next(type, pl, len)) {
                ByteReader rd(pl, len);
                if (type == 'A') {
                    IndexRecord r;
                    if (!decode_record(rd, r) || r[Str::SopInstanceUid].empty()) break;
                    out.apply_add(r, true);
                } else if (type == 'D') {
                    const std::string uid = rd.str();
                    if (!rd.ok()) break;
                    if (auto it = out.instances.find(uid); it != out.instances.end()) out.remove_instance(it->second.get());
                } else {
                    break;
                }
                ++applied;
                good_end = fr.good_end();
            }
            if (good_end == 0) good_end = kHeaderSize;
            counters.replayed_records = applied;
            counters.journal_records = applied;
            counters.recovered_bytes = n - good_end;
        }
        if (stale) {
            counters.replayed_records = counters.journal_records = counters.recovered_bytes = 0;
            return journal_reset(snapshot_gen);  // the mapping is gone now
        }
        auto f = OsFile::open(journal_path, OsFile::Mode::ReadWrite);
        if (!f) return f.take_error();
        if (counters.recovered_bytes) {
            if (Status s = f->truncate(good_end); !s) return s;
            if (Status s = f->sync(); !s) return s;
        }
        journal = std::move(*f);
        counters.journal_bytes = good_end;
        return Status::ok();
    }

    Status load() {
        State fresh;
        std::uint32_t gen = 0;
        fresh.begin_bulk();
        if (Status s = load_snapshot(fresh, gen); !s) return s;
        if (Status s = replay_journal(fresh, gen); !s) return s;
        fresh.end_bulk();
        st = std::move(fresh);
        generation = gen;
        return Status::ok();
    }

    // ---- writes
    Status compact_locked() {
        std::shared_lock<std::shared_mutex> g(rw);
        const std::uint32_t next = generation + 1;
        if (Status s = write_snapshot(next); !s) return s;
        g.unlock();
        generation = next;
        return journal_reset(next);
    }

    Status maybe_compact() {
        const double ratio_limit = opt.compact_ratio * static_cast<double>(st.instances.size());
        const auto limit = std::max<std::uint64_t>(opt.compact_min_records, static_cast<std::uint64_t>(ratio_limit));
        if (counters.journal_records > limit) return compact_locked();
        return Status::ok();
    }

    Result<AddOutcome> add_bytes(const Bytes& src, DuplicatePolicy policy, AddMode mode, const std::string& ref_path) {
        IndexRecord rec;
        if (Status s = extract_index(src, rec); !s) return s;
        Bytes data = src;
        if (mode == AddMode::Copy && !(rec.flags & kHadPreamble)) {
            // Raw data set: store a proper Part 10 file.
            auto f = read_part10(src);
            if (!f) return f.take_error();
            std::vector<std::uint8_t> out;
            if (Status s = write_part10(*f, out); !s) return s;
            auto holder = std::make_shared<std::vector<std::uint8_t>>(std::move(out));
            const std::uint8_t* q = holder->data();
            data = Bytes(std::shared_ptr<const void>(holder, holder.get()), q, holder->size());
            if (Status s = extract_index(data, rec); !s) return s;
        }
        const std::string uid = rec[Str::SopInstanceUid];
        if (uid.empty()) return Status(Errc::Malformed, "instance has no SOP Instance UID");
        if (uid.size() > 1024) return Status(Errc::Malformed, "SOP Instance UID is too long");
        if (rec[Str::SopClassUid] == kMediaStorageDirectoryUid)
            return Status(Errc::Rejected, "a DICOMDIR is not an instance");
        rec.added_time = now_seconds();
        if (mode == AddMode::Reference) {
            rec.flags |= kReferenced;
            rec[Str::FilePath] = ref_path;
        } else {
            rec[Str::FilePath].clear();  // derived from the UID
        }
        {
            std::shared_lock<std::shared_mutex> g(rw);
            if (policy == DuplicatePolicy::Keep && st.instances.count(uid)) return AddOutcome::KeptExisting;
        }
        fs::path final_path, tmp;
        std::error_code ec;
        if (mode == AddMode::Copy) {
            final_path = root / u8path(instance_relative_path(uid));
            fs::create_directories(final_path.parent_path(), ec);
            if (ec) return Status(Errc::IoError, "cannot create data folder");
            tmp = final_path;
            tmp += u8path("." + generate_uid().substr(5, 12) + ".siaara-tmp");
            auto f = OsFile::open(tmp, OsFile::Mode::CreateTruncate);
            if (!f) return f.take_error();
            Status s = f->append(data.data(), data.size());
            if (s && opt.durable) s = f->sync();
            f->close();
            if (!s) {
                fs::remove(tmp, ec);
                return s;
            }
        }
        std::vector<Event> events;
        AddOutcome outcome = AddOutcome::Added;
        {
            std::lock_guard<std::mutex> w(writer);
            bool exists = false;
            {
                std::shared_lock<std::shared_mutex> g(rw);
                exists = st.instances.count(uid) != 0;
            }
            if (exists && policy == DuplicatePolicy::Keep) {
                if (!tmp.empty()) fs::remove(tmp, ec);
                return AddOutcome::KeptExisting;
            }
            if (!tmp.empty()) {
                fs::rename(tmp, final_path, ec);
                if (ec) {
                    fs::remove(tmp, ec);
                    return Status(Errc::IoError, "cannot move instance into the store");
                }
                if (opt.durable) sync_dir(final_path.parent_path());
            }
            ByteWriter bw;
            encode_record(bw, rec);
            std::vector<std::uint8_t> framed;
            frame(framed, 'A', bw.buf);
            if (Status s = journal_append(framed, 1); !s) return s;
            {
                std::unique_lock<std::shared_mutex> g(rw);
                if (st.apply_add(rec, true)) outcome = AddOutcome::Replaced;
            }
            events.push_back({outcome == AddOutcome::Replaced ? EventKind::Replaced : EventKind::Added, uid,
                              rec[Str::SeriesInstanceUid], rec[Str::StudyInstanceUid]});
            if (Status s = maybe_compact(); !s) return s;
        }
        emit(events);
        return outcome;
    }

    std::vector<Instance*> collect(Level level, const std::string& key) const {
        std::vector<Instance*> out;
        auto add_series = [&](const Series* e) { out.insert(out.end(), e->instances.begin(), e->instances.end()); };
        auto add_study = [&](const Study* s) {
            for (const Series* e : s->series) add_series(e);
        };
        switch (level) {
            case Level::Patient:
                if (auto it = st.patients.find(key); it != st.patients.end())
                    for (const Study* s : it->second->studies) add_study(s);
                break;
            case Level::Study:
                if (auto it = st.studies.find(key); it != st.studies.end()) add_study(it->second.get());
                break;
            case Level::Series:
                if (auto it = st.series.find(key); it != st.series.end()) add_series(it->second.get());
                break;
            case Level::Instance:
                if (auto it = st.instances.find(key); it != st.instances.end()) out.push_back(it->second.get());
                break;
        }
        return out;
    }

    Result<std::size_t> remove_uids(const std::vector<std::string>& uids, std::vector<Event>& events) {
        // writer mutex held by the caller
        std::vector<std::string> todo;
        std::vector<fs::path> files;
        {
            std::shared_lock<std::shared_mutex> g(rw);
            for (const auto& u : uids) {
                auto it = st.instances.find(u);
                if (it == st.instances.end()) continue;
                const Instance& in = *it->second;
                todo.push_back(u);
                events.push_back({EventKind::Deleted, u, in.series->uid(), in.series->study->uid()});
                if (!(in.flags & kReferenced)) files.push_back(abs_path(in));
            }
        }
        if (todo.empty()) return std::size_t{0};
        std::vector<std::uint8_t> framed;
        for (const auto& u : todo) {
            ByteWriter bw;
            bw.str(u);
            frame(framed, 'D', bw.buf);
        }
        if (Status s = journal_append(framed, todo.size()); !s) return s;
        {
            std::unique_lock<std::shared_mutex> g(rw);
            for (const auto& u : todo)
                if (auto it = st.instances.find(u); it != st.instances.end()) st.remove_instance(it->second.get());
        }
        std::error_code ec;
        for (const auto& f : files) fs::remove(f, ec);
        if (Status s = maybe_compact(); !s) return s;
        return todo.size();
    }

    // Builds a state from every file under data/ plus the referenced files of `old`.
    State scan_state(const State& old) {
        State fresh;
        fresh.begin_bulk();
        std::error_code ec;
        const auto opts = fs::directory_options::skip_permission_denied;
        for (fs::recursive_directory_iterator it(data_dir, opts, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (!it->is_regular_file(ec2)) continue;
            const fs::path& p = it->path();
            if (p.extension() != ".dcm") continue;
            IndexRecord r;
            if (!extract_index_file(to_utf8(p), r) || r[Str::SopInstanceUid].empty()) continue;
            r[Str::FilePath] = rel_generic(p, root);
            if (r[Str::FilePath] == instance_relative_path(r[Str::SopInstanceUid])) r[Str::FilePath].clear();
            r.added_time = now_seconds();
            fresh.apply_add(r, true);
        }
        for (const auto& [uid, in] : old.instances) {
            if (!(in->flags & kReferenced) || fresh.instances.count(uid)) continue;
            IndexRecord r;
            if (!extract_index_file(in->path, r) || r[Str::SopInstanceUid] != in->uid) continue;
            r.flags |= kReferenced;
            r[Str::FilePath] = in->path;
            r.added_time = in->added;
            fresh.apply_add(r, true);
        }
        fresh.end_bulk();
        return fresh;
    }
};

// ---------------------------------------------------------------- Store
Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Store::~Store() = default;

const std::string& Store::root() const noexcept { return impl_->root_utf8; }

Result<std::unique_ptr<Store>> Store::open(const std::string& utf8_root, const StoreOptions& opt) {
    if (utf8_root.empty()) return Status(Errc::InvalidArgument, "store root is empty");
    auto impl = std::make_unique<Impl>();
    impl->opt = opt;
    std::error_code ec;
    fs::path root = u8path(utf8_root);
    if (!fs::exists(root, ec)) {
        if (!opt.create) return Status(Errc::NotFound, "store folder does not exist");
        fs::create_directories(root, ec);
        if (ec) return Status(Errc::IoError, "cannot create store folder");
    }
    fs::path abs = fs::absolute(root, ec);
    if (!ec) root = abs.lexically_normal();
    impl->root = root;
    impl->root_utf8 = to_utf8(root);
    impl->index_dir = root / "index";
    impl->data_dir = root / "data";
    impl->journal_path = impl->index_dir / "journal.sxj";
    impl->snapshot_path = impl->index_dir / "snapshot.sxs";
    auto lock = LockFile::acquire(root / "siaara-store.lock");
    if (!lock) return lock.take_error();
    impl->lock = std::move(*lock);
    fs::create_directories(impl->index_dir, ec);
    if (ec) return Status(Errc::IoError, "cannot create index folder");
    fs::create_directories(impl->data_dir, ec);
    if (ec) return Status(Errc::IoError, "cannot create data folder");
    Status s = impl->load();
    if (!s) {
        if (!opt.rebuild_if_corrupt || (s.code != Errc::Malformed && s.code != Errc::Unsupported)) return s;
        // Index unreadable: re-index the copied files (referenced entries cannot be recovered).
        State empty;
        impl->st = impl->scan_state(empty);
        impl->generation = 1;
        if (Status w = impl->write_snapshot(impl->generation); !w) return w;
        if (Status j = impl->journal_reset(impl->generation); !j) return j;
    }
    return std::unique_ptr<Store>(new Store(std::move(impl)));
}

Result<AddOutcome> Store::add_file(const std::string& utf8_path, DuplicatePolicy policy, AddMode mode) {
    auto bytes = io::map_file(utf8_path);
    if (!bytes) return bytes.take_error();
    std::string ref;
    if (mode == AddMode::Reference) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(u8path(utf8_path), ec);
        if (ec) return Status(Errc::IoError, "cannot resolve path");
        ref = to_utf8(p);
    }
    return impl_->add_bytes(*bytes, policy, mode, ref);
}

Result<AddOutcome> Store::add_buffer(const Bytes& data, DuplicatePolicy policy) {
    return impl_->add_bytes(data, policy, AddMode::Copy, {});
}

Result<std::size_t> Store::add_records(const std::vector<IndexRecord>& records) {
    std::vector<std::uint8_t> framed;
    std::vector<IndexRecord> recs;
    recs.reserve(records.size());
    const std::uint64_t now = now_seconds();
    for (const auto& r0 : records) {
        if (r0[Str::SopInstanceUid].empty() || r0[Str::SeriesInstanceUid].empty() ||
            r0[Str::StudyInstanceUid].empty() || r0[Str::FilePath].empty())
            return Status(Errc::InvalidArgument, "record lacks a UID or file path");
        IndexRecord r = r0;
        r.flags |= kReferenced;
        r[Str::ModalitiesInStudy].clear();
        r[Str::RecordType].clear();
        if (!r.added_time) r.added_time = now;
        ByteWriter bw;
        encode_record(bw, r);
        frame(framed, 'A', bw.buf);
        recs.push_back(std::move(r));
    }
    std::vector<Event> events;
    bool want_events = false;
    {
        std::lock_guard<std::mutex> g(impl_->cb_mutex);
        want_events = static_cast<bool>(impl_->callback);
    }
    {
        std::lock_guard<std::mutex> w(impl_->writer);
        if (Status s = impl_->journal_append(framed, recs.size()); !s) return s;
        {
            std::unique_lock<std::shared_mutex> g(impl_->rw);
            for (const auto& r : recs) {
                const bool replaced = impl_->st.apply_add(r, true);
                if (want_events)
                    events.push_back({replaced ? EventKind::Replaced : EventKind::Added, r[Str::SopInstanceUid],
                                      r[Str::SeriesInstanceUid], r[Str::StudyInstanceUid]});
            }
        }
        if (Status s = impl_->maybe_compact(); !s) return s;
    }
    impl_->emit(events);
    return recs.size();
}

Result<QueryResult> Store::query(const QueryFilter& q) const {
    Compiled c;
    if (Status s = c.compile(q); !s) return s;
    std::shared_lock<std::shared_mutex> g(impl_->rw);
    const State& st = impl_->st;
    QueryResult res;
    const std::size_t want = q.limit ? q.offset + q.limit : SIZE_MAX;  // rows needed before paging
    const bool any_filter = c.patient_f || c.study_f || c.series_f || c.instance_f;

    std::vector<const Patient*> out_p;
    std::vector<const Study*> out_s;
    std::vector<const Series*> out_e;
    std::vector<const Instance*> out_i;
    bool sorted = false;      // outputs already in result order
    bool stopped = false;     // stopped early: total is a lower bound

    // ---- patient level without study/series/instance filters: walk patients only
    if (q.level == Level::Patient && !c.study_f && !c.series_f && !c.instance_f) {
        std::vector<const Patient*> cands;
        bool ordered = false;
        if (!c.pid.empty() && !has_wildcards(c.pid)) {
            auto range = st.by_patient_id.equal_range(std::string_view(c.pid));
            for (auto it = range.first; it != range.second; ++it) cands.push_back(it->second);
        } else if (!c.name.empty() && !literal_prefix(c.name).empty()) {
            const std::string pre = literal_prefix(c.name);
            std::unordered_set<const Patient*> seen;
            for (auto it = st.by_name_form.lower_bound(std::string_view(pre));
                 it != st.by_name_form.end() && it->first.substr(0, pre.size()) == pre; ++it)
                if (seen.insert(it->second).second) cands.push_back(it->second);
        } else {
            ordered = true;
        }
        if (ordered) {
            for (const Patient* p : st.ordered_patients) {
                if (!c.patient_ok(*p)) continue;
                out_p.push_back(p);
                if (out_p.size() >= want && (!q.want_total || !c.patient_f)) {
                    stopped = c.patient_f;
                    break;
                }
            }
            sorted = true;
            res.total = c.patient_f ? out_p.size() : st.patients.size();
        } else {
            for (const Patient* p : cands)
                if (c.patient_ok(*p)) out_p.push_back(p);
            res.total = out_p.size();
        }
    } else {
        // ---- candidate studies from the most selective index
        std::vector<const Study*> cands;
        enum class From { List, All, DateRange } from = From::List;
        if (!c.sop_uid.empty()) {
            if (auto it = st.instances.find(c.sop_uid); it != st.instances.end()) cands.push_back(it->second->series->study);
        } else if (!c.series_uid.empty()) {
            if (auto it = st.series.find(c.series_uid); it != st.series.end()) cands.push_back(it->second->study);
        } else if (!c.study_uid.empty()) {
            if (auto it = st.studies.find(c.study_uid); it != st.studies.end()) cands.push_back(it->second.get());
        } else if (!c.acc.empty() && !has_wildcards(c.acc)) {
            auto range = st.by_accession.equal_range(std::string_view(c.acc));
            for (auto it = range.first; it != range.second; ++it) cands.push_back(it->second);
        } else if (!c.pid.empty() && !has_wildcards(c.pid)) {
            auto range = st.by_patient_id.equal_range(std::string_view(c.pid));
            for (auto it = range.first; it != range.second; ++it)
                cands.insert(cands.end(), it->second->studies.begin(), it->second->studies.end());
        } else if (c.date && (c.date_lo > 0 || c.date_hi < 99999999u)) {
            from = From::DateRange;
        } else if (!c.name.empty() && !literal_prefix(c.name).empty()) {
            const std::string pre = literal_prefix(c.name);
            std::unordered_set<const Patient*> seen;
            for (auto it = st.by_name_form.lower_bound(std::string_view(pre));
                 it != st.by_name_form.end() && it->first.substr(0, pre.size()) == pre; ++it)
                if (seen.insert(it->second).second)
                    cands.insert(cands.end(), it->second->studies.begin(), it->second->studies.end());
        } else {
            from = From::All;
        }

        std::unordered_set<const Patient*> seen_p;
        auto inst_ok = [&](const Instance* in) { return c.sop_uid.empty() || in->uid == c.sop_uid; };
        // Returns false to stop.
        auto visit = [&](const Study* s) -> bool {
            if (!c.patient_ok(*s->patient) || !c.study_ok(*s)) return true;
            bool any = false;
            if (q.level <= Level::Study && !c.series_f && !c.instance_f) {
                any = true;
            } else {
                for (const Series* e : s->series) {
                    if (!c.series_ok(*e)) continue;
                    if (q.level == Level::Instance) {
                        for (const Instance* in : e->instances)
                            if (inst_ok(in)) {
                                out_i.push_back(in);
                                any = true;
                            }
                        continue;
                    }
                    const bool hit = !c.instance_f || std::any_of(e->instances.begin(), e->instances.end(), inst_ok);
                    if (!hit) continue;
                    any = true;
                    if (q.level == Level::Series) out_e.push_back(e);
                    else break;
                }
            }
            if (!any) return true;
            if (q.level == Level::Study) {
                out_s.push_back(s);
                // In study order, so the first `want` rows are final; with no filter the total is known.
                if (from != From::List && out_s.size() >= want && (!q.want_total || !any_filter)) {
                    stopped = any_filter;
                    return false;
                }
            }
            if (q.level == Level::Patient && seen_p.insert(s->patient).second) out_p.push_back(s->patient);
            return true;
        };
        if (from == From::All) {
            for (const Study* s : st.ordered_studies)
                if (!visit(s)) break;
        } else if (from == From::DateRange) {
            for (auto it = st.ordered_studies.lower_bound(c.date_hi);
                 it != st.ordered_studies.end() && (*it)->date_key >= c.date_lo && (*it)->date_key != 0; ++it)
                if (!visit(*it)) break;
        } else {
            for (const Study* s : cands)
                if (!visit(s)) break;
        }
        sorted = from != From::List && q.level == Level::Study;
        if (q.level == Level::Study) res.total = (!any_filter && from == From::All) ? st.studies.size() : out_s.size();
        else if (q.level == Level::Series) res.total = out_e.size();
        else if (q.level == Level::Instance) res.total = out_i.size();
        else res.total = out_p.size();
    }
    res.total_is_exact = !stopped;

    auto page = [&](auto& v, auto less, auto fill) {
        const std::size_t begin = std::min(q.offset, v.size());
        std::size_t end = v.size();
        if (q.limit && q.limit < end - begin) end = begin + q.limit;
        if (!sorted) {
            if (end < v.size()) std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(end), v.end(), less);
            else std::sort(v.begin(), v.end(), less);
        }
        res.rows.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            IndexRecord r;
            fill(*v[i], r);
            res.rows.push_back(std::move(r));
        }
    };
    switch (q.level) {
        case Level::Patient:
            page(out_p, patient_less, [](const Patient& p, IndexRecord& r) {
                fill_patient(p, r);
                r.level = Level::Patient;
                r.num_studies = static_cast<std::uint32_t>(p.studies.size());
                r.num_series = static_cast<std::uint32_t>(series_count(p));
                r.num_instances = static_cast<std::uint32_t>(instance_count(p));
            });
            break;
        case Level::Study:
            page(out_s, study_less, [](const Study& s, IndexRecord& r) {
                fill_study(s, r);
                r.level = Level::Study;
                r.num_studies = 1;
                r.num_series = static_cast<std::uint32_t>(s.series.size());
                r.num_instances = static_cast<std::uint32_t>(s.instances);
            });
            break;
        case Level::Series:
            page(out_e, series_less, [](const Series& e, IndexRecord& r) {
                fill_series(e, r);
                r.level = Level::Series;
                r.num_studies = 1;
                r.num_series = 1;
                r.num_instances = static_cast<std::uint32_t>(e.instances.size());
            });
            break;
        case Level::Instance: {
            const Impl* impl = impl_.get();
            page(out_i, instance_less, [impl](const Instance& in, IndexRecord& r) {
                fill_series(*in.series, r);
                fill_instance_only(in, r);
                r.level = Level::Instance;
                r.num_studies = r.num_series = r.num_instances = 1;
                r[Str::FilePath] = to_utf8(impl->abs_path(in));
            });
            break;
        }
    }
    return res;
}

Result<std::vector<IndexRecord>> Store::series_of(const std::string& study_uid) const {
    QueryFilter q;
    q.level = Level::Series;
    q.study_uid = study_uid;
    auto r = query(q);
    if (!r) return r.take_error();
    return std::move(r->rows);
}

Result<std::vector<IndexRecord>> Store::instances_of(const std::string& series_uid) const {
    QueryFilter q;
    q.level = Level::Instance;
    q.series_uid = series_uid;
    auto r = query(q);
    if (!r) return r.take_error();
    return std::move(r->rows);
}

Result<IndexRecord> Store::instance(const std::string& sop_uid) const {
    if (sop_uid.empty()) return Status(Errc::InvalidArgument, "empty SOP Instance UID");
    QueryFilter q;
    q.level = Level::Instance;
    q.sop_instance_uid = sop_uid;
    auto r = query(q);
    if (!r) return r.take_error();
    if (r->rows.empty()) return Status(Errc::NotFound, "instance not in the store");
    return std::move(r->rows.front());
}

Result<std::size_t> Store::remove(Level level, const std::string& key) {
    std::vector<Event> events;
    Result<std::size_t> n = std::size_t{0};
    {
        std::lock_guard<std::mutex> w(impl_->writer);
        std::vector<std::string> uids;
        {
            std::shared_lock<std::shared_mutex> g(impl_->rw);
            for (const Instance* in : impl_->collect(level, key)) uids.push_back(in->uid);
        }
        n = impl_->remove_uids(uids, events);
    }
    impl_->emit(events);
    return n;
}

Result<std::size_t> Store::move_to(Store& dest, Level level, const std::string& key) {
    if (&dest == this) return Status(Errc::InvalidArgument, "source and destination are the same store");
    std::vector<std::pair<std::string, std::string>> items;  // uid, absolute path
    {
        std::shared_lock<std::shared_mutex> g(impl_->rw);
        for (const Instance* in : impl_->collect(level, key)) items.emplace_back(in->uid, to_utf8(impl_->abs_path(*in)));
    }
    std::vector<std::string> moved;
    Status err;
    for (const auto& [uid, path] : items) {
        auto r = dest.add_file(path, DuplicatePolicy::Replace, AddMode::Copy);
        if (!r) {
            err = r.take_error();
            break;
        }
        moved.push_back(uid);
    }
    std::vector<Event> events;
    Result<std::size_t> n = std::size_t{0};
    {
        std::lock_guard<std::mutex> w(impl_->writer);
        n = impl_->remove_uids(moved, events);
    }
    impl_->emit(events);
    if (!err) return err;
    return n;
}

Result<VerifyReport> Store::verify(bool deep, bool repair) {
    VerifyReport rep;
    struct Item {
        std::string uid;
        fs::path path;
        std::uint64_t size;
        bool referenced;
    };
    std::vector<Item> items;
    std::unordered_set<std::string> expected;  // generic relative paths of copied instances
    {
        std::shared_lock<std::shared_mutex> g(impl_->rw);
        items.reserve(impl_->st.instances.size());
        for (const auto& [uid, in] : impl_->st.instances) {
            items.push_back({in->uid, impl_->abs_path(*in), in->file_size, (in->flags & kReferenced) != 0});
            if (!(in->flags & kReferenced)) expected.insert(in->stored_path());
        }
    }
    std::vector<std::string> bad;      // remove from the index
    std::vector<fs::path> bad_files;   // copied files to delete with them
    std::vector<IndexRecord> reindex;  // size changed but the content is the same instance
    std::error_code ec;
    for (const auto& it : items) {
        ++rep.checked;
        const auto sz = fs::file_size(it.path, ec);
        if (ec) {
            ++rep.missing;
            rep.missing_uids.push_back(it.uid);
            bad.push_back(it.uid);
            continue;
        }
        bool size_bad = sz != it.size;
        if (size_bad) ++rep.size_mismatch;
        if (deep || size_bad) {
            IndexRecord r;
            const Status s = extract_index_file(to_utf8(it.path), r);
            if (!s || r[Str::SopInstanceUid] != it.uid) {
                if (deep && !size_bad) ++rep.content_mismatch;
                bad.push_back(it.uid);
                if (!it.referenced) bad_files.push_back(it.path);
            } else if (size_bad) {
                if (it.referenced) {
                    r.flags |= kReferenced;
                    r[Str::FilePath] = to_utf8(it.path);
                } else {
                    r[Str::FilePath].clear();
                }
                r.added_time = now_seconds();
                reindex.push_back(std::move(r));
            }
        }
    }
    // orphans under data/
    std::vector<fs::path> orphans;
    const auto opts = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator di(impl_->data_dir, opts, ec), end; !ec && di != end; di.increment(ec)) {
        std::error_code ec2;
        if (!di->is_regular_file(ec2)) continue;
        const std::string rel = rel_generic(di->path(), impl_->root);
        if (expected.count(rel)) continue;
        ++rep.orphans;
        rep.orphan_paths.push_back(rel);
        orphans.push_back(di->path());
    }
    if (!repair) return rep;

    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> w(impl_->writer);
        auto n = impl_->remove_uids(bad, events);
        if (!n) return n.take_error();
        rep.repaired += *n;
        for (const auto& f : bad_files)
            if (fs::remove(f, ec)) ++rep.repaired;
        for (auto& r : reindex) {
            ByteWriter bw;
            encode_record(bw, r);
            std::vector<std::uint8_t> framed;
            frame(framed, 'A', bw.buf);
            if (Status s = impl_->journal_append(framed, 1); !s) return s;
            std::unique_lock<std::shared_mutex> g(impl_->rw);
            impl_->st.apply_add(r, true);
            ++rep.repaired;
            events.push_back({EventKind::Replaced, r[Str::SopInstanceUid], r[Str::SeriesInstanceUid], r[Str::StudyInstanceUid]});
        }
        for (const auto& f : orphans)
            if (is_within(impl_->data_dir, f) && fs::remove(f, ec)) ++rep.repaired;
    }
    impl_->emit(events);
    return rep;
}

Status Store::rebuild() {
    {
        std::lock_guard<std::mutex> w(impl_->writer);
        State fresh;
        {
            std::shared_lock<std::shared_mutex> g(impl_->rw);
            fresh = impl_->scan_state(impl_->st);
        }
        {
            std::unique_lock<std::shared_mutex> g(impl_->rw);
            impl_->st = std::move(fresh);
        }
        if (Status s = impl_->compact_locked(); !s) return s;
    }
    impl_->emit({Event{EventKind::Rebuilt, {}, {}, {}}});
    return Status::ok();
}

Status Store::compact() {
    std::lock_guard<std::mutex> w(impl_->writer);
    return impl_->compact_locked();
}

void Store::set_callback(EventCallback cb) {
    std::lock_guard<std::mutex> g(impl_->cb_mutex);
    impl_->callback = std::move(cb);
}

StoreStats Store::stats() const {
    std::lock_guard<std::mutex> w(impl_->writer);
    std::shared_lock<std::shared_mutex> g(impl_->rw);
    StoreStats s = impl_->counters;
    s.patients = impl_->st.patients.size();
    s.studies = impl_->st.studies.size();
    s.series = impl_->st.series.size();
    s.instances = impl_->st.instances.size();
    s.data_bytes = impl_->st.data_bytes;
    s.generation = impl_->generation;
    return s;
}

}  // namespace siaara::store
