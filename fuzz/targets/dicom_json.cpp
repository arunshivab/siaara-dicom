// Fuzz target: DICOM JSON model reader (PS3.18 F). Accepted documents are written back and must
// read again.
#include "fuzz.hpp"
#include "web/dicom_json.hpp"

using namespace siaara;
using namespace siaara::web;

SIAARA_FUZZ_TARGET(dicom_json) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    ModelReadOptions ro;
    ro.max_value_bytes = 4u << 20;
    ro.max_elements = 200'000;
    std::vector<BulkReference> unresolved;
    ro.unresolved = &unresolved;
    auto sets = datasets_from_json(text, ro);
    if (sets) {
        std::string out;
        if (datasets_to_json(*sets, out)) FUZZ_REQUIRE(datasets_from_json(out, ro).is_ok());
    }
    auto one = dataset_from_json(text, ro);
    if (one) {
        std::string out;
        if (dataset_to_json(*one, out)) FUZZ_REQUIRE(dataset_from_json(out, ro).is_ok());
    }
    return 0;
}
