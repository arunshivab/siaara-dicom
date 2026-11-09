// SIAARA DICOM Toolkit - Native DICOM Model XML (PS3.19 Annex A): data set <-> XML.
#pragma once

#include <string>
#include <string_view>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "web/dicom_model.hpp"

namespace siaara::web {

// Appends a complete XML document (declaration + <NativeDicomModel>) for `ds`.
Status dataset_to_xml(const Dataset& ds, std::string& out, const ModelWriteOptions& opt = {});

// Parses a <NativeDicomModel> document. The root may carry the PS3.19 namespace or none.
// A privateCreator attribute whose Private Creator element is missing from the data set adds it.
Result<Dataset> dataset_from_xml(std::string_view text, const ModelReadOptions& opt = {});

}  // namespace siaara::web
