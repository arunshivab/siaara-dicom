// Fuzz target: DICOM Native XML model reader (PS3.19 A.1). Accepted documents are written back and
// must read again.
#include "fuzz.hpp"
#include "web/dicom_xml.hpp"

using namespace siaara;
using namespace siaara::web;

SIAARA_FUZZ_TARGET(dicom_xml) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    ModelReadOptions ro;
    ro.max_value_bytes = 4u << 20;
    ro.max_elements = 200'000;
    std::vector<BulkReference> unresolved;
    ro.unresolved = &unresolved;
    auto ds = dataset_from_xml(text, ro);
    if (!ds) return 0;
    std::string out;
    if (dataset_to_xml(*ds, out)) FUZZ_REQUIRE(dataset_from_xml(out, ro).is_ok());
    return 0;
}
