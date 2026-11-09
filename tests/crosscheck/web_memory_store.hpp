// Test-only in-memory DICOMweb Store (used by tests/test_web_loopback.cpp and the crosscheck tool).
// Matching follows PS3.4 C.2.2.2 in a simplified way: universal, single value, UID list,
// wildcard (* and ?), and date/time range matching on top-level attributes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/reader.hpp"
#include "dict/dictionary.hpp"
#include "web/server.hpp"

namespace siaara_test_web {

using namespace siaara;
using namespace siaara::web;

class MemoryStore {
public:
    struct Instance {
        std::string study, series, sop;
        Bytes raw;
        DicomFile file;
    };

    std::uint16_t add(const DicomFile& f, const Bytes& raw) {
        Instance in;
        in.study = f.dataset.get_string(tags::StudyInstanceUID).value_or("");
        in.series = f.dataset.get_string(tags::SeriesInstanceUID).value_or("");
        in.sop = f.dataset.get_string(tags::SOPInstanceUID).value_or("");
        if (in.study.empty() || in.series.empty() || in.sop.empty()) return failure::CannotUnderstand;
        in.raw = raw;
        in.file = f;
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& x : items_)
            if (x.sop == in.sop) { x = std::move(in); return 0; }
        items_.push_back(std::move(in));
        return 0;
    }

    Status add_part10(const Bytes& raw) {
        auto f = read_part10(raw);
        if (!f) return f.error();
        if (add(*f, raw) != 0) return Status(Errc::Malformed, "instance without UIDs");
        return Status::ok();
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lk(mu_);
        return items_.size();
    }

    Store make_store() {
        Store s;
        s.search = [this](const SearchQuery& q, std::vector<Dataset>& out) { return search(q, out); };
        s.retrieve = [this](const InstanceKey& k, std::vector<Bytes>& out) { return retrieve(k, out); };
        s.store = [this](const DicomFile& f, const Bytes& raw) { return add(f, raw); };
        return s;
    }

    Status retrieve(const InstanceKey& k, std::vector<Bytes>& out) {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& in : items_) {
            if (in.study != k.study) continue;
            if (!k.series.empty() && in.series != k.series) continue;
            if (!k.instance.empty() && in.sop != k.instance) continue;
            out.push_back(in.raw);
        }
        return out.empty() ? Status(Errc::NotFound, "no match") : Status::ok();
    }

    Status search(const SearchQuery& q, std::vector<Dataset>& out) {
        std::lock_guard<std::mutex> lk(mu_);
        // Group the candidate instances by the query level's key.
        std::map<std::string, std::vector<const Instance*>> groups;
        for (const auto& in : items_) {
            if (!q.study_uid.empty() && in.study != q.study_uid) continue;
            if (!q.series_uid.empty() && in.series != q.series_uid) continue;
            const std::string& key = q.level == QueryLevel::Study ? in.study : q.level == QueryLevel::Series ? in.series : in.sop;
            groups[key].push_back(&in);
        }
        for (const auto& [key, members] : groups) {
            // The server lets the store stop once it has offset + limit matches.
            if (q.limit && out.size() >= static_cast<std::uint64_t>(q.offset) + *q.limit) break;
            const Dataset& rep = members.front()->file.dataset;
            // Study-level aggregates.
            std::set<std::string> modalities, series_set;
            for (const auto* m : members) {
                if (auto mod = m->file.dataset.get_string(tags::Modality)) modalities.insert(*mod);
                series_set.insert(m->series);
            }
            bool ok = true;
            for (const auto& [tag, value] : q.match) {
                if (tag == kModalitiesInStudy) {
                    bool any = value.empty();
                    for (const auto& mod : modalities)
                        if (match_value(mod, value, VR::CS, false)) any = true;
                    if (!any) { ok = false; break; }
                    continue;
                }
                const Element* e = rep.find(tag);
                VR vr = e ? e->vr : VR::None;
                if (vr == VR::None) {
                    if (const auto* d = dict::lookup(tag)) vr = d->vr;
                }
                if (vr == VR::SQ) return Status(Errc::Unsupported, "sequence matching not supported");
                if (value.empty()) continue;  // universal matching
                std::string have;
                if (e) {
                    auto raw = rep.get_raw_string(tag);
                    if (raw && vr_is_string(vr)) have = std::string(trim_value(*raw, vr));
                    else if (auto s = rep.get_string(tag)) have = *s;
                }
                if (!e || !match_value(have, value, vr, q.fuzzy_matching)) { ok = false; break; }
            }
            if (!ok) continue;
            Dataset r;
            auto copy = [&](Tag t) {
                if (const Element* e = rep.find(t)) r.put(*e);
            };
            static const Tag kStudy[] = {tags::SpecificCharacterSet, 0x00080020, 0x00080030, 0x00080050, 0x00080090,
                                         tags::PatientName, tags::PatientID, 0x00100030, 0x00100040,
                                         tags::StudyInstanceUID, 0x00200010, 0x00081030};
            static const Tag kSeries[] = {tags::SpecificCharacterSet, tags::Modality, 0x0008103E, tags::SeriesInstanceUID,
                                          0x00200011, tags::StudyInstanceUID};
            static const Tag kInstance[] = {tags::SpecificCharacterSet, tags::SOPClassUID, tags::SOPInstanceUID,
                                            0x00200013, tags::Rows, tags::Columns, 0x00280100, tags::NumberOfFrames,
                                            tags::StudyInstanceUID, tags::SeriesInstanceUID};
            if (q.level == QueryLevel::Study) {
                for (Tag t : kStudy) copy(t);
                std::string mods;
                for (const auto& m : modalities) mods += (mods.empty() ? "" : "\\") + m;
                (void)r.set_string(kModalitiesInStudy, VR::CS, mods);
                (void)r.set_string(0x00201206, VR::IS, std::to_string(series_set.size()));
                (void)r.set_string(0x00201208, VR::IS, std::to_string(members.size()));
            } else if (q.level == QueryLevel::Series) {
                for (Tag t : kSeries) copy(t);
                (void)r.set_string(0x00201209, VR::IS, std::to_string(members.size()));
            } else {
                for (Tag t : kInstance) copy(t);
            }
            for (const auto& m : q.match) copy(m.first);
            for (Tag t : q.include_fields) copy(t);
            if (q.include_all)
                for (const auto& e : rep.elements())
                    if (e.tag != tags::PixelData && e.value.size() <= 1024 && !e.is_encapsulated()) r.put(e);
            out.push_back(std::move(r));
        }
        return Status::ok();
    }

private:
    static constexpr Tag kModalitiesInStudy = 0x00080061;
    std::mutex mu_;
    std::vector<Instance> items_;

    static char fold(char c, bool ci) { return (ci && c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

    static bool wildcard(std::string_view s, std::string_view p, bool ci) {
        std::size_t si = 0, pi = 0, star = std::string_view::npos, mark = 0;
        while (si < s.size()) {
            if (pi < p.size() && (p[pi] == '?' || fold(p[pi], ci) == fold(s[si], ci))) { ++si; ++pi; }
            else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = si; }
            else if (star != std::string_view::npos) { pi = star + 1; si = ++mark; }
            else return false;
        }
        while (pi < p.size() && p[pi] == '*') ++pi;
        return pi == p.size();
    }

    static bool match_value(const std::string& have, const std::string& want, VR vr, bool fuzzy) {
        if (vr == VR::UI) {
            std::size_t s = 0;
            while (s <= want.size()) {
                auto e = want.find_first_of(",\\", s);
                if (e == std::string::npos) e = want.size();
                if (want.substr(s, e - s) == have) return true;
                s = e + 1;
            }
            return false;
        }
        if ((vr == VR::DA || vr == VR::TM || vr == VR::DT) && want.find('-') != std::string::npos) {
            const auto d = want.find('-');
            const std::string lo = want.substr(0, d), hi = want.substr(d + 1);
            if (have.empty()) return false;
            return (lo.empty() || have >= lo) && (hi.empty() || have.substr(0, hi.size()) <= hi);
        }
        const bool ci = fuzzy && vr == VR::PN;
        if (want.find_first_of("*?") != std::string::npos) return wildcard(have, want, ci);
        if (ci) {
            // Fuzzy: case-insensitive prefix match on the whole value.
            return wildcard(have, want + "*", true);
        }
        return have == want;
    }
};

}  // namespace siaara_test_web
