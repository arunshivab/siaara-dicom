#include "repair/study.hpp"

#include <algorithm>
#include <map>
#include <optional>

#include "clinical/geometry.hpp"
#include "dict/dictionary.hpp"

namespace siaara::repair {

namespace {

struct LevelAttr {
    Tag tag;
    VR vr;
};
// PS3.3 C.7.1.1 Patient, C.7.2.1 General Study, C.7.3.1 General Series (identifying attributes).
constexpr LevelAttr kPatientAttrs[] = {
    {0x00100010, VR::PN}, {0x00100020, VR::LO}, {0x00100021, VR::LO}, {0x00100030, VR::DA}, {0x00100040, VR::CS},
};
constexpr LevelAttr kStudyAttrs[] = {
    {0x00080020, VR::DA}, {0x00080030, VR::TM}, {0x00080050, VR::SH}, {0x00080090, VR::PN},
    {0x00200010, VR::SH}, {0x00081030, VR::LO},
};
constexpr LevelAttr kSeriesAttrs[] = {
    {0x00080060, VR::CS}, {0x00200011, VR::IS}, {0x0008103E, VR::LO}, {0x00180015, VR::CS},
    {0x0020000D, VR::UI},  // a series belongs to exactly one study
};

std::string list_inputs(const std::vector<std::size_t>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += i + 1 == v.size() ? " and " : ", ";
        if (i == 8 && v.size() > 9) {
            s += std::to_string(v.size() - 8) + " more";
            break;
        }
        s += std::to_string(v[i]);
    }
    return s;
}

std::string keyword(Tag t) {
    if (const dict::Entry* e = dict::lookup(t)) return std::string(e->keyword);
    return tag_string(t);
}

Finding make(Severity s, const char* code, Tag tag, std::string msg) {
    Finding f;
    f.severity = s;
    f.code = code;
    f.category = kStudy;
    if (tag) f.path.push_back({tag, -1});
    f.message = std::move(msg);
    return f;
}

std::optional<std::string> raw_value(const Dataset& ds, Tag t) {
    const Element* e = ds.find(t);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    return std::string(trim_value(e->value.as_chars(), e->vr));
}

void check_level(const std::vector<std::size_t>& members, std::span<const Dataset* const> ds, std::span<const LevelAttr> attrs,
                 const char* level, const StudyOptions& opt, StudyPlan& plan) {
    if (members.size() < 2) return;
    for (const LevelAttr& a : attrs) {
        std::map<std::string, std::vector<std::size_t>> by_value;
        std::vector<std::size_t> absent;
        for (std::size_t i : members) {
            auto v = raw_value(*ds[i], a.tag);
            if (!v) absent.push_back(i);
            else by_value[*v].push_back(i);
        }
        if (by_value.size() <= 1) continue;
        // Majority value (strict: more files than any other value).
        const std::vector<std::size_t>* best = nullptr;
        const std::string* best_value = nullptr;
        bool tie = false;
        for (const auto& [v, idx] : by_value) {
            if (!best || idx.size() > best->size()) {
                best = &idx;
                best_value = &v;
                tie = false;
            } else if (idx.size() == best->size()) {
                tie = true;
            }
        }
        std::vector<std::size_t> minority;
        for (const auto& [v, idx] : by_value)
            if (&idx != best) minority.insert(minority.end(), idx.begin(), idx.end());
        std::sort(minority.begin(), minority.end());
        std::string msg = std::string(level) + "-level attribute " + keyword(a.tag) + " has " +
                          std::to_string(by_value.size()) + " different values across " + std::to_string(members.size()) +
                          " instances of the same " + level;
        if (tie) {
            msg += "; no majority value, so repair leaves it (inputs " + list_inputs(minority) + " and " +
                   list_inputs(*best) + ")";
            plan.findings.push_back(make(Severity::Error, "level-conflict", a.tag, msg));
            continue;
        }
        msg += "; inputs " + list_inputs(minority) + " differ from the majority (" + std::to_string(best->size()) + " instances)";
        plan.findings.push_back(make(Severity::Error, "level-conflict", a.tag, msg));
        if (!opt.harmonise) continue;
        for (std::size_t i : minority)
            plan.per_file[i].push_back({a.tag, a.vr, *best_value,
                                        std::string(level) + "-level value harmonised to the majority of " +
                                            std::to_string(members.size()) + " instances"});
    }
}

}  // namespace

StudyPlan plan_study(std::span<const Dataset* const> ds, std::span<const std::string> hashes, const StudyOptions& opt,
                     const std::function<std::string(std::string_view)>& new_uid) {
    StudyPlan plan;
    plan.per_file.resize(ds.size());
    std::vector<std::size_t> valid;
    for (std::size_t i = 0; i < ds.size(); ++i)
        if (ds[i]) valid.push_back(i);
    if (valid.size() < 2) return plan;

    // ---- Missing Study / Series Instance UIDs
    std::map<std::string, std::string> series_to_study;  // from files that have both (majority not needed here)
    for (std::size_t i : valid) {
        auto st = raw_value(*ds[i], tags::StudyInstanceUID);
        auto se = raw_value(*ds[i], tags::SeriesInstanceUID);
        if (st && se && !st->empty() && !se->empty()) series_to_study.emplace(*se, *st);
    }
    std::vector<std::string> study_of(ds.size()), series_of(ds.size());
    for (std::size_t i : valid) {
        study_of[i] = raw_value(*ds[i], tags::StudyInstanceUID).value_or("");
        series_of[i] = raw_value(*ds[i], tags::SeriesInstanceUID).value_or("");
    }
    for (std::size_t i : valid) {
        if (!study_of[i].empty() || series_of[i].empty()) continue;
        auto it = series_to_study.find(series_of[i]);
        if (it == series_to_study.end()) continue;
        plan.findings.push_back(make(Severity::Error, "study-uid-missing", tags::StudyInstanceUID,
                                     "input " + std::to_string(i) +
                                         " has no Study Instance UID; other instances of its series have one"));
        if (opt.fill_uids) {
            plan.per_file[i].push_back({tags::StudyInstanceUID, VR::UI, it->second,
                                        "Study Instance UID taken from the other instances of the same series"});
            study_of[i] = it->second;
        }
    }
    for (std::size_t i : valid) {
        if (!series_of[i].empty() || study_of[i].empty()) continue;
        // Same study, same Modality and Series Number: exactly one candidate series.
        auto mod = raw_value(*ds[i], tags::Modality);
        auto num = raw_value(*ds[i], 0x00200011);
        std::vector<std::string> cands;
        for (std::size_t k : valid) {
            if (k == i || series_of[k].empty() || study_of[k] != study_of[i]) continue;
            if (raw_value(*ds[k], tags::Modality) != mod || raw_value(*ds[k], 0x00200011) != num || !num) continue;
            if (std::find(cands.begin(), cands.end(), series_of[k]) == cands.end()) cands.push_back(series_of[k]);
        }
        std::string msg = "input " + std::to_string(i) + " has no Series Instance UID";
        if (cands.size() == 1) {
            msg += "; one series of the same study has its Modality and Series Number";
            if (opt.fill_uids) {
                plan.per_file[i].push_back({tags::SeriesInstanceUID, VR::UI, cands[0],
                                            "Series Instance UID taken from the series with the same Modality and "
                                            "Series Number in the same study"});
                series_of[i] = cands[0];
            }
        } else {
            msg += "; no single matching series (attribute repair can generate a new UID)";
        }
        plan.findings.push_back(make(Severity::Error, "series-uid-missing", tags::SeriesInstanceUID, msg));
    }

    // ---- Level consistency
    std::map<std::string, std::vector<std::size_t>> studies, series;
    for (std::size_t i : valid) {
        if (!study_of[i].empty()) studies[study_of[i]].push_back(i);
        if (!series_of[i].empty()) series[series_of[i]].push_back(i);
    }
    for (const auto& [uid, members] : studies) {
        check_level(members, ds, kPatientAttrs, "patient", opt, plan);
        check_level(members, ds, kStudyAttrs, "study", opt, plan);
    }
    for (const auto& [uid, members] : series) check_level(members, ds, kSeriesAttrs, "series", opt, plan);

    // ---- Duplicate SOP Instance UIDs
    std::map<std::string, std::vector<std::size_t>> by_sop;
    for (std::size_t i : valid)
        if (auto s = raw_value(*ds[i], tags::SOPInstanceUID); s && !s->empty()) by_sop[*s].push_back(i);
    for (const auto& [uid, members] : by_sop) {
        if (members.size() < 2) continue;
        bool identical = true;
        for (std::size_t i : members)
            if (i >= hashes.size() || hashes[i] != hashes[members[0]]) identical = false;
        if (identical) {
            plan.findings.push_back(make(Severity::Warning, "duplicate-copy", tags::SOPInstanceUID,
                                         "inputs " + list_inputs(members) +
                                             " are identical copies of one instance (same SOP Instance UID and content)"));
            continue;
        }
        plan.findings.push_back(make(Severity::Error, "duplicate-sop-instance-uid", tags::SOPInstanceUID,
                                     "inputs " + list_inputs(members) +
                                         " are different instances with the same SOP Instance UID (PS3.5 9.1)"));
        if (!opt.new_uids) continue;
        for (std::size_t k = 1; k < members.size(); ++k) {
            const std::size_t i = members[k];
            plan.per_file[i].push_back({tags::SOPInstanceUID, VR::UI,
                                        new_uid("sop-instance|" + uid + "|" + (i < hashes.size() ? hashes[i] : std::string())),
                                        "new SOP Instance UID: another input (" + std::to_string(members[0]) +
                                            ") is a different instance with the same UID"});
        }
    }

    // ---- Instance Numbers within a series
    for (const auto& [uid, members] : series) {
        if (members.size() < 2) continue;
        std::map<std::int64_t, std::vector<std::size_t>> by_num;
        std::vector<std::size_t> missing;
        for (std::size_t i : members) {
            auto n = ds[i]->get_int(0x00200013);
            if (!n) missing.push_back(i);
            else by_num[*n].push_back(i);
        }
        bool dup = false, gap = false;
        for (const auto& [n, idx] : by_num)
            if (idx.size() > 1) dup = true;
        if (!by_num.empty()) {
            const std::int64_t lo = by_num.begin()->first, hi = by_num.rbegin()->first;
            if (static_cast<std::uint64_t>(hi - lo + 1) != by_num.size()) gap = true;
        }
        if (!dup && !gap && missing.empty()) continue;
        std::string what;
        if (dup) what += "duplicated";
        if (gap) what += std::string(what.empty() ? "" : ", ") + "not consecutive";
        if (!missing.empty()) what += std::string(what.empty() ? "" : ", ") + "missing in inputs " + list_inputs(missing);
        plan.findings.push_back(make(dup || !missing.empty() ? Severity::Warning : Severity::Info, "instance-number",
                                     0x00200013,
                                     "Instance Numbers in a series of " + std::to_string(members.size()) +
                                         " instances are " + what));
        if (!opt.renumber) continue;
        std::vector<const Dataset*> list;
        for (std::size_t i : members) list.push_back(ds[i]);
        const clinical::SeriesSort sorted = clinical::sort_series(list);
        std::vector<std::size_t> order;
        for (std::uint32_t item : sorted.order) {
            const std::size_t inst = members[sorted.items[item].instance];
            if (std::find(order.begin(), order.end(), inst) == order.end()) order.push_back(inst);
        }
        for (std::size_t i : members)
            if (std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
        for (std::size_t k = 0; k < order.size(); ++k) {
            const std::string v = std::to_string(k + 1);
            auto cur = raw_value(*ds[order[k]], 0x00200013);
            if (cur && *cur == v) continue;
            plan.per_file[order[k]].push_back({0x00200013, VR::IS, v,
                                               "Instance Number renumbered by position in the series (" +
                                                   std::to_string(k + 1) + " of " + std::to_string(order.size()) + ")"});
        }
    }
    return plan;
}

}  // namespace siaara::repair
