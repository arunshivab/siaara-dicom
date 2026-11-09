// SIAARA DICOM Toolkit - DICOM JSON Model (PS3.18 Annex F): data set <-> JSON.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "web/dicom_model.hpp"
#include "web/json.hpp"

namespace siaara::web {

// Appends one DICOM JSON object (F.2) for `ds` to `out`. Group length elements are omitted
// (F.2.2). Text is converted to UTF-8 from the data set's Specific Character Set.
Status dataset_to_json(const Dataset& ds, std::string& out, const ModelWriteOptions& opt = {});
// Appends a JSON array of DICOM JSON objects (the QIDO-RS / metadata response form).
Status datasets_to_json(const std::vector<const Dataset*>& sets, std::string& out,
                        const ModelWriteOptions& opt = {});
Status datasets_to_json(const std::vector<Dataset>& sets, std::string& out, const ModelWriteOptions& opt = {});

// Parses one DICOM JSON object.
Result<Dataset> dataset_from_json(std::string_view text, const ModelReadOptions& opt = {});
// Parses a JSON array of DICOM JSON objects (a single object is accepted as a one-element array).
Result<std::vector<Dataset>> datasets_from_json(std::string_view text, const ModelReadOptions& opt = {});
// Converts an already parsed object.
Result<Dataset> dataset_from_json_value(const json::Value& v, const ModelReadOptions& opt = {});

}  // namespace siaara::web
