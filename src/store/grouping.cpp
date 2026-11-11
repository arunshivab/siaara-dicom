#include "store/grouping.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "core/display_safe.hpp"
#include "core/reader.hpp"
#include "store/dicomdir.hpp"

namespace siaara::store {

namespace {

namespace gt {
constexpr Tag SopClassUid = 0x00080016, SopInstanceUid = 0x00080018, StudyDate = 0x00080020, Modality = 0x00080060,
              SeriesDescription = 0x0008103E, PatientName = 0x00100010, PatientId = 0x00100020,
              StudyUid = 0x0020000D, SeriesUid = 0x0020000E, SeriesNumber = 0x00200011,
              InstanceNumber = 0x00200013, ImagePosition = 0x00200032, ImageOrientation = 0x00200037,
              SliceLocation = 0x00201041, NumberOfFrames = 0x00280008, Rows = 0x00280010, Columns = 0x00280011,
              IssuerOfPatientId = 0x00100021, AcquisitionTime = 0x00080032, ContentTime = 0x00080033;
}  // namespace gt

constexpr std::size_t idx(TextField f) noexcept { return static_cast<std::size_t>(f); }

bool is_uid_field(TextField f) noexcept {
    return f == TextField::SopClassUid || f == TextField::SopInstanceUid || f == TextField::StudyUid ||
           f == TextField::SeriesUid;
}

// The stored element is usable as a value: present, not a sequence, not encapsulated.
const Element* value_element(const Dataset& ds, Tag tag) {
    const Element* e = ds.find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return nullptr;
    return e;
}

// ASCII-only lower case (the matching of names and File IDs).
std::string fold(std::string_view s, bool on) {
    std::string out(s);
    if (on)
        for (char& c : out)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// A caller's name split on '/' and '\', '.' components dropped (for matching only).
std::vector<std::string_view> name_components(std::string_view name) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/' || name[i] == '\\') {
            const std::string_view part = name.substr(start, i - start);
            if (part != ".") out.push_back(part);
            start = i + 1;
        }
    }
    return out;
}

std::string join_key(const std::vector<std::string_view>& parts, std::size_t from, bool fold_case) {
    std::string k;
    for (std::size_t i = from; i < parts.size(); ++i) {
        if (i > from) k.push_back('/');
        k += fold(parts[i], fold_case);
    }
    return k;
}

bool all_finite(const double* v, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// The unit normal (row x column) of an orientation, when it has one.
std::optional<std::array<double, 3>> normal_of(const std::array<double, 6>& o) {
    if (!all_finite(o.data(), 6)) return std::nullopt;
    const std::array<double, 3> n{o[1] * o[5] - o[2] * o[4], o[2] * o[3] - o[0] * o[5], o[0] * o[4] - o[1] * o[3]};
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!std::isfinite(len) || len < kGroupingMinNormalLength) return std::nullopt;
    return std::array<double, 3>{n[0] / len, n[1] / len, n[2] / len};
}

bool same_text(const GroupText& a, const GroupText& b) noexcept { return a.status == b.status && a.stored == b.stored; }

// A TM value (HH, HHMM, HHMMSS, HHMMSS.F to .FFFFFF; the ACR-NEMA form HH:MM:SS too) as seconds
// since midnight; padding and a second value are ignored. nullopt when it is not a time.
std::optional<double> parse_time(std::string_view v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.remove_suffix(1);
    while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
    if (const auto bs = v.find('\\'); bs != std::string_view::npos) v = v.substr(0, bs);
    std::string digits;
    std::size_t i = 0;
    for (; i < v.size() && v[i] != '.'; ++i) {
        if (v[i] == ':') continue;
        if (v[i] < '0' || v[i] > '9') return std::nullopt;
        digits.push_back(v[i]);
    }
    if (digits.empty() || digits.size() % 2 != 0 || digits.size() > 6) return std::nullopt;
    auto two = [&](std::size_t at) { return at + 2 <= digits.size() ? (digits[at] - '0') * 10 + (digits[at + 1] - '0') : 0; };
    const int hh = two(0), mm = two(2), ss = two(4);
    if (hh > 23 || mm > 59 || ss > 60) return std::nullopt;
    double frac = 0;
    if (i < v.size()) {
        if (digits.size() != 6 || i + 1 >= v.size() || v.size() - i - 1 > 6) return std::nullopt;
        long micro = 0;
        int places = 0;
        for (std::size_t k = i + 1; k < v.size(); ++k, ++places) {
            if (v[k] < '0' || v[k] > '9') return std::nullopt;
            micro = micro * 10 + (v[k] - '0');
        }
        for (; places < 6; ++places) micro *= 10;
        frac = static_cast<double>(micro) / 1e6;
    }
    return hh * 3600.0 + mm * 60.0 + ss + frac;
}

}  // namespace

bool grouping_uid_valid(std::string_view uid, UidValidation rule) noexcept {
    if (uid.empty() || uid.size() > 64) return false;
    for (char c : uid)
        if (c != '.' && (c < '0' || c > '9')) return false;
    if (rule == UidValidation::Lenient) return true;
    // PS3.5 9.1: components of digits separated by single dots; no empty component; no leading
    // zero unless the component is "0".
    std::size_t start = 0;
    for (std::size_t i = 0; i <= uid.size(); ++i) {
        if (i < uid.size() && uid[i] != '.') continue;
        const std::size_t n = i - start;
        if (n == 0 || (n > 1 && uid[start] == '0')) return false;
        start = i + 1;
    }
    return true;
}

bool grouping_name_unsafe(std::string_view name) noexcept {
    if (name.empty()) return false;
    if (name.front() == '/' || name.front() == '\\') return true;  // absolute (or UNC)
    if (name.size() >= 2 && name[1] == ':' && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')))
        return true;  // a drive letter
    std::size_t start = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i < name.size() && name[i] != '/' && name[i] != '\\') continue;
        if (name.substr(start, i - start) == "..") return true;
        start = i + 1;
    }
    return false;
}

Grouping::Grouping(const GroupingOptions& opt) : opt_(opt) {}

bool Grouping::keep_text(GroupText& t, std::string_view stored, std::uint64_t& cost) const {
    t.stored.assign(stored.data(), stored.size());
    t.display = text::display_safe(stored);
    cost += t.stored.size() + t.display.size();
    return true;
}

// Marks a total budget reached (sticky: every later file is skipped for it).
void Grouping::budget(Limit l) {
    if (summary_.budget_reached == Limit::None) summary_.budget_reached = l;
}

void Grouping::skip(File& f, SkipReason r, Limit l, Errc status) {
    f.skip = r;
    f.limit = l;
    f.status = status;
    ++summary_.skipped;
}

void Grouping::add_file(std::string_view name, const std::uint8_t* data, std::size_t size) {
    built_ = false;
    files_.emplace_back();
    const auto file_index = static_cast<std::uint32_t>(files_.size() - 1);
    ++summary_.files;

    // The name: kept (stored + display) while the result budget allows.
    {
        File& f = files_.back();
        if (name.empty()) {
            f.name.status = TextStatus::Absent;
        } else if (name.size() > opt_.max_text_bytes) {
            f.name.status = TextStatus::TooLong;
        } else if (summary_.budget_reached == Limit::ResultBytes) {
            f.name.status = TextStatus::NotKept;
        } else {
            const std::uint64_t cost = name.size() + text::display_safe_size(name);
            if (summary_.result_bytes + cost > opt_.max_result_bytes) {
                f.name.status = TextStatus::NotKept;
                budget(Limit::ResultBytes);
            } else {
                std::uint64_t c = 0;
                keep_text(f.name, name, c);
                f.name.status = TextStatus::Ok;
                summary_.result_bytes += c;
            }
        }
    }
    File& f = files_.back();
    if (summary_.budget_reached != Limit::None) return skip(f, SkipReason::TotalBudget, summary_.budget_reached);
    if (summary_.files_read >= opt_.max_files) {
        budget(Limit::MaxFiles);
        return skip(f, SkipReason::TotalBudget, Limit::MaxFiles);
    }
    if (size > opt_.max_file_bytes) return skip(f, SkipReason::FileLimit, Limit::FileBytes);
    if (summary_.total_bytes + size > opt_.max_total_bytes) {
        budget(Limit::TotalBytes);
        return skip(f, SkipReason::TotalBudget, Limit::TotalBytes);
    }
    summary_.total_bytes += size;
    ++summary_.files_read;
    if (!data || !has_part10_header(data, size)) return skip(f, SkipReason::NotDicom, Limit::None, Errc::NotDicom);

    // Header only, with the untrusted limits; the inflate cap is the smaller of the per-file
    // limit and what is left of the total. The bytes are viewed, not copied, and not kept.
    const std::uint64_t left = opt_.max_total_inflated - summary_.total_inflated;
    const bool total_caps = left < opt_.max_inflated_size;
    ReadOptions ro = untrusted_read_options(opt_.max_depth, opt_.max_elements, opt_.max_inflated_size, true);
    ro.max_inflated_size = std::max<std::uint64_t>(1, std::min(left, opt_.max_inflated_size));
    const Bytes bytes(std::shared_ptr<const void>{}, data, size);
    auto file = read_part10(bytes, ro);
    if (!file) {
        const Status& e = file.error();
        if (e.code == Errc::NotDicom) return skip(f, SkipReason::NotDicom, Limit::None, e.code);
        if (e.code == Errc::LimitExceeded) {
            // The inflater's own message names it (codec/deflate.cpp); any other limit is the
            // nesting depth or the element / item count of this file.
            if (e.message.rfind("deflate:", 0) == 0) {
                if (total_caps) {
                    budget(Limit::TotalInflated);
                    return skip(f, SkipReason::TotalBudget, Limit::TotalInflated, e.code);
                }
                return skip(f, SkipReason::FileLimit, Limit::InflatedSize, e.code);
            }
            return skip(f, SkipReason::FileLimit, Limit::ReadLimits, e.code);
        }
        return skip(f, SkipReason::Unreadable, Limit::None, e.code);
    }
    summary_.total_inflated += file->inflated_size;
    const Dataset& ds = file->dataset;
    // A Part 10 header and nothing after it (or nothing that could be read): skipped, not ungrouped.
    if (ds.empty())
        return skip(f, SkipReason::Unreadable, Limit::None,
                    (file->read_loss & DicomFile::kReadInputTruncated) ? Errc::Truncated : Errc::Malformed);

    GroupedInstance in;
    in.file = file_index;
    in.read_loss = file->read_loss;
    std::uint64_t cost = 0;
    auto take_text = [&](TextField field, Tag tag) {
        GroupText& t = in.text[idx(field)];
        const Element* e = value_element(ds, tag);
        if (!e || e->value.empty()) return;
        if (e->value.size() > opt_.max_value_bytes) {
            t.status = TextStatus::TooLong;
            return;
        }
        auto v = charset::get_utf8_all(ds, tag);
        if (!v || v->empty()) return;
        const std::uint64_t cap = is_uid_field(field) ? opt_.max_uid_chars : opt_.max_text_bytes;
        if (v->size() > cap) {
            t.status = TextStatus::TooLong;
            return;
        }
        keep_text(t, *v, cost);
        t.status = is_uid_field(field) && !grouping_uid_valid(t.stored, opt_.uid_validation) ? TextStatus::NotAUid : TextStatus::Ok;
    };
    take_text(TextField::SopClassUid, gt::SopClassUid);
    take_text(TextField::SopInstanceUid, gt::SopInstanceUid);
    take_text(TextField::StudyUid, gt::StudyUid);
    take_text(TextField::SeriesUid, gt::SeriesUid);
    take_text(TextField::Modality, gt::Modality);
    take_text(TextField::StudyDate, gt::StudyDate);
    take_text(TextField::SeriesDescription, gt::SeriesDescription);
    // The patient key: Patient ID plus Issuer of Patient ID, both exactly as stored (the bytes).
    // Files without a Patient ID (absent, empty, or longer than max_value_bytes and so not
    // comparable) form one "no Patient ID" patient per study, never merged across studies. The
    // key is kept internally; it is returned only as the fields below, with include_patient_fields.
    std::string patient_key;
    {
        auto raw = [&](Tag tag) -> std::optional<std::string_view> {
            const Element* e = value_element(ds, tag);
            if (!e || e->value.size() > opt_.max_value_bytes) return std::nullopt;
            const std::string_view v = e->value.as_chars();
            if (v.find_first_not_of(std::string_view(" \0", 2)) == std::string_view::npos) return std::nullopt;
            return v;
        };
        const auto pid = raw(gt::PatientId);
        if (pid) {
            const auto issuer = raw(gt::IssuerOfPatientId);
            patient_key = "I" + std::to_string(pid->size()) + ":" + std::string(*pid) + "|" +
                          (issuer ? "1" + std::string(*issuer) : std::string("0"));
        } else {
            const GroupText& study = in.text[idx(TextField::StudyUid)];
            patient_key = "N" + std::to_string(static_cast<int>(study.status)) + study.stored;
        }
        cost += patient_key.size();  // kept (internally) whether or not it is returned
        if (opt_.include_patient_fields) {
            take_text(TextField::PatientId, gt::PatientId);
            take_text(TextField::PatientName, gt::PatientName);
            take_text(TextField::IssuerOfPatientId, gt::IssuerOfPatientId);
        } else {
            for (TextField t : {TextField::PatientId, TextField::PatientName, TextField::IssuerOfPatientId})
                in.text[idx(t)].status = TextStatus::NotRequested;
        }
    }

    // Numbers. A stored value longer than max_value_bytes is not parsed (as for text).
    auto numeric = [&](Tag tag) -> const Element* {
        const Element* e = value_element(ds, tag);
        return e && e->value.size() <= opt_.max_value_bytes ? e : nullptr;
    };
    if (numeric(gt::SeriesNumber))
        if (auto v = ds.get_int(gt::SeriesNumber)) {
            in.series_number = *v;
            in.flags |= kGiSeriesNumber;
        }
    if (numeric(gt::InstanceNumber))
        if (auto v = ds.get_int(gt::InstanceNumber)) {
            in.instance_number = *v;
            in.flags |= kGiInstanceNumber;
        }
    if (numeric(gt::NumberOfFrames))
        if (auto v = ds.get_int(gt::NumberOfFrames)) {
            in.number_of_frames = *v;
            in.flags |= kGiFrames;
        }
    if (numeric(gt::Rows))
        if (auto v = ds.get_int(gt::Rows); v && *v >= 0 && *v <= UINT32_MAX) {
            in.rows = static_cast<std::uint32_t>(*v);
            in.flags |= kGiRows;
        }
    if (numeric(gt::Columns))
        if (auto v = ds.get_int(gt::Columns); v && *v >= 0 && *v <= UINT32_MAX) {
            in.columns = static_cast<std::uint32_t>(*v);
            in.flags |= kGiColumns;
        }
    auto doubles = [&](Tag tag, double* out, std::size_t n) {
        if (!numeric(tag)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            auto v = ds.get_double(tag, i);
            if (!v) return false;
            out[i] = *v;
        }
        return true;
    };
    if (doubles(gt::ImagePosition, in.image_position.data(), 3)) in.flags |= kGiPosition;
    else in.image_position = {};
    if (doubles(gt::ImageOrientation, in.image_orientation.data(), 6)) in.flags |= kGiOrientation;
    else in.image_orientation = {};
    if (doubles(gt::SliceLocation, &in.slice_location, 1)) in.flags |= kGiSliceLocation;
    else in.slice_location = 0;
    for (Tag t : {gt::AcquisitionTime, gt::ContentTime}) {
        const Element* e = numeric(t);
        if (!e) continue;
        if (auto v = parse_time(e->value.as_chars())) {
            in.acquisition_time = *v;
            in.flags |= kGiAcquisitionTime;
        }
        break;  // Content Time only when there is no Acquisition Time
    }

    if (summary_.result_bytes + cost > opt_.max_result_bytes) {
        budget(Limit::ResultBytes);
        return skip(f, SkipReason::TotalBudget, Limit::ResultBytes);
    }

    // Duplicates (a valid SOP Instance UID only).
    const GroupText& sop = in.text[idx(TextField::SopInstanceUid)];
    if (sop.status == TextStatus::Ok) {
        auto [it, fresh] = sop_index_.try_emplace(sop.stored, static_cast<std::uint32_t>(instances_.size()));
        if (!fresh) {
            if (opt_.duplicates == GroupDuplicates::KeepFirst) {
                f.duplicate_of = static_cast<std::int32_t>(it->second);
                return;
            }
            in.flags |= kGiDuplicate;
            in.duplicate_of = static_cast<std::int32_t>(it->second);  // internal index until build()
        }
    }
    summary_.result_bytes += cost;
    f.instance = static_cast<std::int32_t>(instances_.size());
    instances_.push_back(std::move(in));
    patient_keys_.push_back(std::move(patient_key));
}

void Grouping::add_dicomdir(const std::uint8_t* data, std::size_t size) {
    built_ = false;
    dicomdirs_.emplace_back();
    const auto dd = static_cast<std::uint32_t>(dicomdirs_.size() - 1);
    ++summary_.dicomdirs;
    auto fail = [&](SkipReason r, Limit l, Errc s) {
        Dicomdir& d = dicomdirs_[dd];
        d.skip = r;
        d.limit = l;
        d.status = s;
    };
    if (dd >= opt_.max_dicomdirs) return fail(SkipReason::TotalBudget, Limit::MaxDicomdirs, Errc::LimitExceeded);
    if (summary_.budget_reached != Limit::None) return fail(SkipReason::TotalBudget, summary_.budget_reached, Errc::Ok);
    if (size > opt_.max_file_bytes) return fail(SkipReason::FileLimit, Limit::FileBytes, Errc::LimitExceeded);
    if (summary_.total_bytes + size > opt_.max_total_bytes) {
        budget(Limit::TotalBytes);
        return fail(SkipReason::TotalBudget, Limit::TotalBytes, Errc::LimitExceeded);
    }
    summary_.total_bytes += size;
    if (!data || !has_part10_header(data, size)) return fail(SkipReason::NotDicom, Limit::None, Errc::NotDicom);
    std::vector<DicomdirEntry> got;
    std::uint64_t cost = 0;
    DicomdirReadOptions o;
    o.max_bytes = opt_.max_file_bytes;
    auto r = read_dicomdir_memory(data, size, [&](const DicomdirRecord& rec) {
        if (rec.file_id.empty()) return true;
        DicomdirEntry e;
        e.dicomdir = dd;
        e.record = static_cast<std::uint32_t>(rec.index);
        e.file_status = rec.file_status.code;
        e.components = rec.file_id.size();
        const std::string& text = rec.keys[Str::FilePath];
        if (text.size() > opt_.max_text_bytes) {
            e.file_id.status = TextStatus::TooLong;
        } else {
            keep_text(e.file_id, text, cost);
            e.file_id.status = TextStatus::Ok;
        }
        got.push_back(std::move(e));
        return summary_.result_bytes + cost <= opt_.max_result_bytes;
    }, o);
    if (summary_.result_bytes + cost > opt_.max_result_bytes) {
        budget(Limit::ResultBytes);
        return fail(SkipReason::TotalBudget, Limit::ResultBytes, Errc::LimitExceeded);
    }
    if (!r) {
        const Errc c = r.error().code;
        if (c == Errc::NotDicom) return fail(SkipReason::NotDicom, Limit::None, c);
        if (c == Errc::LimitExceeded) {
            const bool records = r.error().message.find("records") != std::string::npos;
            return fail(SkipReason::FileLimit, records ? Limit::DicomdirRecords : Limit::ReadLimits, c);
        }
        return fail(SkipReason::Unreadable, Limit::None, c);
    }
    summary_.result_bytes += cost;
    for (auto& e : got) entries_.push_back(std::move(e));
}

void Grouping::build() {
    patients_.clear();
    studies_.clear();
    series_.clear();
    order_.clear();
    issues_.clear();
    for (auto& f : files_) f.dicomdir_matches.clear();
    GroupingSummary& sum = summary_;
    sum.instances = instances_.size();
    sum.ungrouped = sum.inconsistencies = sum.listed_missing = sum.present_unlisted = sum.ambiguous = 0;

    // ---- tree: patients by key, studies by (patient, Study UID), series by (study, Series UID);
    // first-added order at every level.
    std::unordered_map<std::string, std::uint32_t> patient_at, study_at, series_at;
    std::vector<std::vector<std::uint32_t>> patient_studies, study_series, series_members;
    std::vector<std::uint32_t> ungrouped;
    for (std::uint32_t i = 0; i < instances_.size(); ++i) {
        GroupedInstance& in = instances_[i];
        in.series = -1;
        in.flags &= ~(kGiUngrouped | kGiPositionUsable | kGiInDicomdir | kGiUnsafeName);
        const bool study_ok = in.text[idx(TextField::StudyUid)].status == TextStatus::Ok;
        const bool series_ok = in.text[idx(TextField::SeriesUid)].status == TextStatus::Ok;
        if (!study_ok || !series_ok) {
            in.flags |= kGiUngrouped;
            ungrouped.push_back(i);
            continue;
        }
        const std::string& pkey = patient_keys_[i];
        auto [pit, pnew] = patient_at.try_emplace(pkey, static_cast<std::uint32_t>(patients_.size()));
        if (pnew) {
            GroupedPatient p;
            p.first_instance_ref = i;
            p.no_patient_id = !pkey.empty() && pkey.front() == 'N';
            patients_.push_back(p);
            patient_studies.emplace_back();
        }
        const std::uint32_t p = pit->second;
        std::string skey = std::to_string(p) + '|' + in.text[idx(TextField::StudyUid)].stored;
        auto [sit, snew] = study_at.try_emplace(std::move(skey), static_cast<std::uint32_t>(studies_.size()));
        if (snew) {
            GroupedStudy s;
            s.patient = p;
            s.first_instance_ref = i;
            studies_.push_back(s);
            study_series.emplace_back();
            patient_studies[p].push_back(sit->second);
        }
        const std::uint32_t s = sit->second;
        std::string ekey = std::to_string(s) + '|' + in.text[idx(TextField::SeriesUid)].stored;
        auto [eit, enew] = series_at.try_emplace(std::move(ekey), static_cast<std::uint32_t>(series_.size()));
        if (enew) {
            GroupedSeries e;
            e.study = s;
            e.first_instance_ref = i;
            series_.push_back(e);
            series_members.emplace_back();
            study_series[s].push_back(eit->second);
        }
        series_members[eit->second].push_back(i);
    }

    // ---- order within each series. NaN and infinity never reach a comparator: every tier holds
    // only finite keys (or integers), and the tiers are concatenated.
    for (std::uint32_t k = 0; k < series_.size(); ++k) {
        GroupedSeries& e = series_[k];
        std::vector<std::uint32_t>& m = series_members[k];
        // The series' reference orientation: its first file (as added) with a usable one.
        std::array<double, 6> ref{};
        for (std::uint32_t i : m) {
            const GroupedInstance& in = instances_[i];
            if (!(in.flags & kGiOrientation)) continue;
            if (auto n = normal_of(in.image_orientation)) {
                e.has_normal = true;
                e.normal = *n;
                ref = in.image_orientation;
                break;
            }
        }
        for (std::uint32_t i : m) {
            GroupedInstance& in = instances_[i];
            if (!e.has_normal || !(in.flags & kGiPosition) || !(in.flags & kGiOrientation)) continue;
            if (!all_finite(in.image_position.data(), 3) || !all_finite(in.image_orientation.data(), 6)) continue;
            bool same = true;
            for (std::size_t c = 0; c < 6; ++c)
                if (!(std::fabs(in.image_orientation[c] - ref[c]) <= opt_.orientation_tolerance)) same = false;
            if (!same) continue;
            const double pos = in.image_position[0] * e.normal[0] + in.image_position[1] * e.normal[1] +
                               in.image_position[2] * e.normal[2];
            if (!std::isfinite(pos)) continue;
            in.position = pos;
            in.flags |= kGiPositionUsable;
        }
        auto has_slice = [](const GroupedInstance& in) { return (in.flags & kGiSliceLocation) && std::isfinite(in.slice_location); };
        auto has_number = [](const GroupedInstance& in) { return (in.flags & kGiInstanceNumber) != 0; };
        bool all_pos = true, all_slice = true, all_number = true;
        for (std::uint32_t i : m) {
            const GroupedInstance& in = instances_[i];
            all_pos = all_pos && (in.flags & kGiPositionUsable);
            all_slice = all_slice && has_slice(in);
            all_number = all_number && has_number(in);
        }
        // PerSeries: one rule for the whole series when any position is unusable.
        const bool per_series = opt_.order == GroupOrder::Position && opt_.position_fallback == PositionFallback::PerSeries && !all_pos;
        const SortKey series_key = all_slice ? SortKey::SliceLocation : all_number ? SortKey::InstanceNumber : SortKey::Added;
        std::array<std::vector<std::uint32_t>, 4> tiers;
        for (std::uint32_t i : m) {
            GroupedInstance& in = instances_[i];
            const bool usable = (in.flags & kGiPositionUsable) != 0;
            if (opt_.order == GroupOrder::AsAdded) in.sort_key = SortKey::Added;
            else if (opt_.order == GroupOrder::InstanceNumber) in.sort_key = has_number(in) ? SortKey::InstanceNumber : SortKey::Added;
            else if (per_series) in.sort_key = series_key;
            else in.sort_key = usable ? SortKey::Position : has_slice(in) ? SortKey::SliceLocation
                                                         : has_number(in) ? SortKey::InstanceNumber : SortKey::Added;
            tiers[static_cast<std::size_t>(in.sort_key)].push_back(i);
        }
        // Ties: tie_break, then the order the files were added in (stable sorts). Only finite
        // doubles and integers are compared.
        auto tie_less = [&](const GroupedInstance& a, const GroupedInstance& b) {
            if (opt_.tie_break == TieBreak::InstanceNumber) {
                if (has_number(a) != has_number(b)) return has_number(a);
                return has_number(a) && a.instance_number < b.instance_number;
            }
            if (opt_.tie_break == TieBreak::AcquisitionTime) {
                const bool ta = (a.flags & kGiAcquisitionTime) != 0, tb = (b.flags & kGiAcquisitionTime) != 0;
                if (ta != tb) return ta;
                return ta && a.acquisition_time < b.acquisition_time;
            }
            return false;
        };
        auto sort_tier = [&](std::vector<std::uint32_t>& t, auto key) {
            std::stable_sort(t.begin(), t.end(), [&](std::uint32_t x, std::uint32_t y) {
                const GroupedInstance& a = instances_[x];
                const GroupedInstance& b = instances_[y];
                const auto ka = key(a), kb = key(b);
                if (ka < kb) return true;
                if (kb < ka) return false;
                return tie_less(a, b);
            });
        };
        sort_tier(tiers[0], [](const GroupedInstance& in) { return in.position; });
        sort_tier(tiers[1], [](const GroupedInstance& in) { return in.slice_location; });
        sort_tier(tiers[2], [](const GroupedInstance& in) { return in.instance_number; });
        m.clear();
        std::size_t used = 0;
        for (std::size_t t = 0; t < tiers.size(); ++t) {
            if (!tiers[t].empty()) {
                ++used;
                e.order = static_cast<SeriesOrder>(t);
            }
            m.insert(m.end(), tiers[t].begin(), tiers[t].end());
        }
        if (used > 1) e.order = SeriesOrder::Mixed;
    }

    // ---- flatten: studies and series are numbered patient by patient, study by study.
    {
        std::vector<GroupedStudy> studies;
        std::vector<GroupedSeries> series;
        std::vector<std::uint32_t> study_new(studies_.size());
        for (std::uint32_t p = 0; p < patients_.size(); ++p) {
            GroupedPatient& gp = patients_[p];
            gp.first_study = static_cast<std::uint32_t>(studies.size());
            gp.study_count = static_cast<std::uint32_t>(patient_studies[p].size());
            for (std::uint32_t s : patient_studies[p]) {
                study_new[s] = static_cast<std::uint32_t>(studies.size());
                GroupedStudy gs = studies_[s];
                gs.first_series = static_cast<std::uint32_t>(series.size());
                gs.series_count = static_cast<std::uint32_t>(study_series[s].size());
                for (std::uint32_t k : study_series[s]) {
                    GroupedSeries ge = series_[k];
                    ge.study = study_new[s];
                    ge.first_instance = static_cast<std::uint32_t>(order_.size());
                    ge.instance_count = static_cast<std::uint32_t>(series_members[k].size());
                    for (std::uint32_t i : series_members[k]) {
                        instances_[i].series = static_cast<std::int32_t>(series.size());
                        order_.push_back(i);
                    }
                    gs.instance_count += ge.instance_count;
                    series.push_back(ge);
                }
                gp.instance_count += gs.instance_count;
                studies.push_back(gs);
            }
        }
        studies_ = std::move(studies);
        series_ = std::move(series);
    }
    for (std::uint32_t i : ungrouped) order_.push_back(i);
    result_of_.assign(instances_.size(), -1);
    for (std::uint32_t r = 0; r < order_.size(); ++r) result_of_[order_[r]] = static_cast<std::int32_t>(r);
    const std::vector<std::int32_t>& result_of = result_of_;

    auto issue = [&](IssueKind kind) -> GroupingIssue& {
        issues_.emplace_back();
        issues_.back().kind = kind;
        return issues_.back();
    };
    auto instance_issue = [&](IssueKind kind, std::uint32_t internal) -> GroupingIssue& {
        GroupingIssue& g = issue(kind);
        const GroupedInstance& in = instances_[internal];
        g.instance = result_of[internal];
        g.file = static_cast<std::int32_t>(in.file);
        g.name = &files_[in.file].name;
        if (in.series >= 0) {
            g.series = in.series;
            g.study = static_cast<std::int32_t>(series_[static_cast<std::size_t>(in.series)].study);
        }
        return g;
    };

    // ---- skipped files
    for (std::uint32_t fi = 0; fi < files_.size(); ++fi) {
        const File& f = files_[fi];
        if (f.skip == SkipReason::None) continue;
        GroupingIssue& g = issue(IssueKind::Skipped);
        g.reason = static_cast<std::int32_t>(f.skip);
        g.limit = f.limit;
        g.status = f.status;
        g.file = static_cast<std::int32_t>(fi);
        g.name = &f.name;
    }
    for (std::uint32_t d = 0; d < dicomdirs_.size(); ++d) {
        const Dicomdir& dd = dicomdirs_[d];
        if (dd.skip == SkipReason::None) continue;
        GroupingIssue& g = issue(IssueKind::DicomdirSkipped);
        g.reason = static_cast<std::int32_t>(dd.skip);
        g.limit = dd.limit;
        g.status = dd.status;
        g.dicomdir = static_cast<std::int32_t>(d);
    }
    // ---- ungrouped
    for (std::uint32_t i : ungrouped) {
        GroupingIssue& g = instance_issue(IssueKind::Ungrouped, i);
        const bool study_ok = instances_[i].text[idx(TextField::StudyUid)].status == TextStatus::Ok;
        const bool series_ok = instances_[i].text[idx(TextField::SeriesUid)].status == TextStatus::Ok;
        g.reason = static_cast<std::int32_t>(!study_ok && !series_ok ? UngroupedReason::Both
                                             : !study_ok             ? UngroupedReason::StudyUid
                                                                     : UngroupedReason::SeriesUid);
        ++sum.ungrouped;
    }
    // ---- duplicates
    for (std::uint32_t r = 0; r < order_.size(); ++r) {
        const GroupedInstance& in = instances_[order_[r]];
        if (!(in.flags & kGiDuplicate)) continue;
        GroupingIssue& g = instance_issue(IssueKind::Duplicate, order_[r]);
        g.other_instance = result_of[static_cast<std::size_t>(in.duplicate_of)];
        g.subject = &in.text[idx(TextField::SopInstanceUid)];
    }
    for (std::uint32_t fi = 0; fi < files_.size(); ++fi) {
        const File& f = files_[fi];
        if (f.duplicate_of < 0) continue;
        GroupingIssue& g = issue(IssueKind::Duplicate);
        g.file = static_cast<std::int32_t>(fi);
        g.name = &f.name;
        g.other_instance = result_of[static_cast<std::size_t>(f.duplicate_of)];
        g.subject = &instances_[static_cast<std::size_t>(f.duplicate_of)].text[idx(TextField::SopInstanceUid)];
    }
    sum.duplicates = 0;
    for (const auto& g : issues_)
        if (g.kind == IssueKind::Duplicate) ++sum.duplicates;
    // ---- inconsistencies: a Study UID under more than one patient
    {
        std::map<std::string, std::vector<std::uint32_t>> by_uid;
        for (std::uint32_t s = 0; s < studies_.size(); ++s)
            by_uid[instances_[studies_[s].first_instance_ref].text[idx(TextField::StudyUid)].stored].push_back(s);
        for (std::uint32_t s = 0; s < studies_.size(); ++s) {
            const GroupedInstance& ref = instances_[studies_[s].first_instance_ref];
            if (by_uid[ref.text[idx(TextField::StudyUid)].stored].size() < 2) continue;
            GroupingIssue& g = issue(IssueKind::StudyPatientConflict);
            g.study = static_cast<std::int32_t>(s);
            g.instance = result_of[studies_[s].first_instance_ref];
            g.subject = &ref.text[idx(TextField::StudyUid)];
            ++sum.inconsistencies;
        }
    }
    // ---- a series whose instances disagree on Modality or Rows x Columns (against its first file)
    for (std::uint32_t k = 0; k < series_.size(); ++k) {
        const GroupedSeries& e = series_[k];
        const GroupedInstance& ref = instances_[e.first_instance_ref];
        const std::uint32_t size_bits = kGiRows | kGiColumns;
        std::int32_t modality = -1, size = -1;
        for (std::uint32_t r = e.first_instance; r < e.first_instance + e.instance_count; ++r) {
            const GroupedInstance& in = instances_[order_[r]];
            if (modality < 0 && !same_text(in.text[idx(TextField::Modality)], ref.text[idx(TextField::Modality)]))
                modality = static_cast<std::int32_t>(r);
            if (size < 0 && ((in.flags & size_bits) != (ref.flags & size_bits) || in.rows != ref.rows || in.columns != ref.columns))
                size = static_cast<std::int32_t>(r);
        }
        if (modality >= 0) {
            GroupingIssue& g = instance_issue(IssueKind::SeriesModalityConflict, order_[static_cast<std::size_t>(modality)]);
            g.other_instance = result_of[e.first_instance_ref];
            ++sum.inconsistencies;
        }
        if (size >= 0) {
            GroupingIssue& g = instance_issue(IssueKind::SeriesSizeConflict, order_[static_cast<std::size_t>(size)]);
            g.other_instance = result_of[e.first_instance_ref];
            ++sum.inconsistencies;
        }
    }

    // ---- names that are unsafe as paths: reported, never matched to a File ID
    sum.unsafe_names = 0;
    for (std::uint32_t fi = 0; fi < files_.size(); ++fi) {
        File& f = files_[fi];
        f.unsafe_name = f.name.status == TextStatus::Ok && grouping_name_unsafe(f.name.stored);
        if (!f.unsafe_name) continue;
        GroupingIssue& g = f.instance >= 0 ? instance_issue(IssueKind::UnsafeName, static_cast<std::uint32_t>(f.instance))
                                           : issue(IssueKind::UnsafeName);
        g.file = static_cast<std::int32_t>(fi);
        g.name = &f.name;
        if (f.instance >= 0) instances_[static_cast<std::size_t>(f.instance)].flags |= kGiUnsafeName;
        ++sum.unsafe_names;
    }

    // ---- DICOMDIR File IDs against the caller's names. A File ID listed more than once (by two
    // DICOMDIRs, or twice by one) is one entry with all its sources (dicomdir_mask).
    bool any_dicomdir = false;
    for (const auto& d : dicomdirs_)
        if (d.skip == SkipReason::None) any_dicomdir = true;
    if (any_dicomdir) {
        const bool fold_case = opt_.match_case_insensitive;
        struct Listed {
            std::uint32_t first = 0;   // entries_ index of its first listing
            std::uint32_t mask = 0;
            bool matchable = false;
            std::vector<std::uint32_t> matched_files;
        };
        std::vector<Listed> listed;
        std::unordered_map<std::string, std::uint32_t> listed_at;
        std::unordered_map<std::string, std::uint32_t> by_key;  // matchable File IDs
        std::vector<std::size_t> lengths;
        for (std::uint32_t k = 0; k < entries_.size(); ++k) {
            const DicomdirEntry& e = entries_[k];
            const bool matchable = e.file_status == Errc::Ok && e.file_id.status == TextStatus::Ok;
            const std::string key = matchable ? "M" + fold(e.file_id.stored, fold_case)  // well formed: no '\' or '.' inside
                                              : "R" + std::to_string(static_cast<int>(e.file_id.status)) + e.file_id.stored;
            auto [it, fresh] = listed_at.try_emplace(key, static_cast<std::uint32_t>(listed.size()));
            if (fresh) {
                Listed l;
                l.first = k;
                l.matchable = matchable;
                listed.push_back(l);
                if (matchable) {
                    by_key.emplace(key.substr(1), it->second);
                    if (std::find(lengths.begin(), lengths.end(), e.components) == lengths.end()) lengths.push_back(e.components);
                }
            }
            if (e.dicomdir < 32) listed[it->second].mask |= 1u << e.dicomdir;
        }
        for (std::uint32_t fi = 0; fi < files_.size(); ++fi) {
            File& f = files_[fi];
            if (f.name.status != TextStatus::Ok || f.unsafe_name) continue;
            const auto parts = name_components(f.name.stored);
            auto match = [&](const std::string& key, std::size_t want) {
                auto it = by_key.find(key);
                if (it == by_key.end() || entries_[listed[it->second].first].components != want) return;
                listed[it->second].matched_files.push_back(fi);
                f.dicomdir_matches.push_back(it->second);
            };
            if (opt_.match_by_trailing_components) {
                for (std::size_t n : lengths)
                    if (parts.size() >= n) match(join_key(parts, parts.size() - n, fold_case), n);
            } else {
                match(join_key(parts, 0, fold_case), parts.size());
            }
            if (!f.dicomdir_matches.empty() && f.instance >= 0)
                instances_[static_cast<std::size_t>(f.instance)].flags |= kGiInDicomdir;
        }
        for (const Listed& l : listed) {
            if (!l.matched_files.empty()) continue;
            const DicomdirEntry& e = entries_[l.first];
            GroupingIssue& g = issue(IssueKind::ListedMissing);
            g.dicomdir = static_cast<std::int32_t>(e.dicomdir);
            g.record = static_cast<std::int32_t>(e.record);
            g.dicomdir_mask = l.mask;
            g.status = e.file_status;
            g.subject = &e.file_id;
            ++sum.listed_missing;
        }
        for (const Listed& l : listed) {
            if (l.matched_files.size() < 2) continue;
            const DicomdirEntry& e = entries_[l.first];
            for (std::uint32_t fi : l.matched_files) {
                const File& f = files_[fi];
                GroupingIssue& g = f.instance >= 0 ? instance_issue(IssueKind::AmbiguousMatch, static_cast<std::uint32_t>(f.instance))
                                                   : issue(IssueKind::AmbiguousMatch);
                g.file = static_cast<std::int32_t>(fi);
                g.name = &f.name;
                g.dicomdir = static_cast<std::int32_t>(e.dicomdir);
                g.record = static_cast<std::int32_t>(e.record);
                g.dicomdir_mask = l.mask;
                g.subject = &e.file_id;
                ++sum.ambiguous;
            }
        }
        for (std::uint32_t r = 0; r < order_.size(); ++r) {
            const GroupedInstance& in = instances_[order_[r]];
            if (in.flags & kGiInDicomdir) continue;
            instance_issue(IssueKind::PresentUnlisted, order_[r]);
            ++sum.present_unlisted;
        }
    }

    sum.patients = patients_.size();
    sum.studies = studies_.size();
    sum.series = series_.size();
    sum.issues = issues_.size();
    built_ = true;
}

}  // namespace siaara::store
