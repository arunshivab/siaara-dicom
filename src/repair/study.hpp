// SIAARA DICOM Toolkit - study / series consistency across files (PS3.3 Annex A IE levels:
// Patient, Study, Series; C.7.1.1, C.7.2.1, C.7.3.1, C.12.1 SOP Instance UID uniqueness, PS3.5 9.1).
//
// Findings name inputs by their index in the input list and attributes by tag/keyword only; they
// never contain values.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "repair/report.hpp"

namespace siaara::repair {

struct StudyOptions {
    bool harmonise = true;        // set conflicting Patient/Study/Series attributes to the majority value
    bool new_uids = true;         // give duplicate SOP Instance UIDs (different content) new UIDs
    bool renumber = true;         // renumber Instance Number by position when duplicated / gapped / missing
    bool fill_uids = true;        // fill missing Study / Series Instance UIDs from the other files
};

// One planned change to a top-level element of one file.
struct PlannedSet {
    Tag tag = 0;
    VR vr = VR::None;
    std::string value;            // new value (string VR)
    std::string detail;           // no values
};

struct StudyPlan {
    std::vector<Finding> findings;
    std::vector<std::vector<PlannedSet>> per_file;  // same order as the inputs
};

// `datasets[i]` may be null (unreadable input). `hashes[i]`: content hash, used to tell identical
// copies from different instances sharing a SOP Instance UID. `new_uid(seed)` makes a UID.
StudyPlan plan_study(std::span<const Dataset* const> datasets, std::span<const std::string> hashes,
                     const StudyOptions& opt, const std::function<std::string(std::string_view)>& new_uid);

}  // namespace siaara::repair
