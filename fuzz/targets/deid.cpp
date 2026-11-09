// Fuzz target: de-identifier (PS3.15 E Basic Profile + options). The input is parsed as a Part 10
// file (an empty data set when it does not parse), PHI is planted in identifying attributes at
// the top level and inside sequence items, and the result is de-identified with option bits from
// the first input byte. Properties: no crash; when de-identification succeeds, the planted
// marker appears nowhere in the written output (data set and file meta).
#include <algorithm>

#include "clinical/common.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "deid/deid.hpp"
#include "fuzz.hpp"
#include "fuzz_util.hpp"

using namespace siaara;

namespace {

constexpr std::string_view kMarker = "QZXPHIMARK";

Element seq(Tag t, Dataset item) {
    Element e;
    e.tag = t;
    e.vr = VR::SQ;
    e.items.push_back(std::move(item));
    return e;
}

bool contains(const std::vector<std::uint8_t>& v, std::string_view s) {
    return std::search(v.begin(), v.end(), s.begin(), s.end()) != v.end();
}

}  // namespace

SIAARA_FUZZ_TARGET(deid) {
    const std::string_view whole(reinterpret_cast<const char*>(data), size);
    const bool marker_in_input = whole.find(kMarker) != std::string_view::npos;
    const std::uint32_t options = size > 0 ? (data[0] | (size > 1 ? (data[1] & 3u) << 8 : 0u)) & deid::kAllOptionBits : 0;
    ReadOptions ro;
    ro.max_elements = 200'000;
    ro.max_inflated_size = 32u << 20;
    auto parsed = read_part10(siaara_fuzz::to_bytes(data, size), ro);
    DicomFile f;
    if (parsed) f = std::move(*parsed);
    if (f.transfer_syntax.empty()) f.transfer_syntax = "1.2.840.10008.1.2.1";

    const std::string pn = std::string(kMarker) + "^PATIENT";
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::PatientName, VR::PN, pn);
    (void)ds.set_string(tags::PatientID, VR::LO, kMarker);
    (void)ds.set_string(0x00101001, VR::PN, pn);       // Other Patient Names
    (void)ds.set_string(0x00101040, VR::LO, kMarker);  // Patient's Address
    if (!(options & deid::RetainInstitutionIdentity)) (void)ds.set_string(0x00080080, VR::LO, kMarker);
    Dataset nested;
    (void)nested.set_string(tags::PatientName, VR::PN, pn);
    (void)nested.set_string(tags::PatientID, VR::LO, kMarker);
    ds.put(seq(0x00081120, nested));  // Referenced Patient Sequence (K with Retain UIDs)
    Dataset content;
    (void)content.set_string(0x0040A040, VR::CS, "PNAME");
    (void)content.set_string(0x0040A123, VR::PN, pn);  // Person Name
    ds.put(seq(0x0040A730, content));                  // Content Sequence

    static constexpr std::uint8_t kKey[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                              17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    deid::Options opt;
    opt.options = options;
    opt.key = kKey;
    if (size > 2 && (data[2] & 1u)) opt.combined = deid::CombinedPolicy::Remove;
    deid::Report rep;
    if (!deid::deidentify(f, opt, &rep)) return 0;
    std::vector<std::uint8_t> out;
    if (!write_part10(f, out)) {
        // A data set the writer cannot encode: check the body in another encoding.
        if (!write_dataset(f.dataset, "1.2.840.10008.1.2", out)) return 0;
    }
    if (!marker_in_input) FUZZ_REQUIRE(!contains(out, kMarker));
    return 0;
}
