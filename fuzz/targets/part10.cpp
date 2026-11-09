// Fuzz target: Part 10 / raw data set reader in every native transfer syntax, lenient and strict,
// header-only and full; a successful parse is written back and must parse again.
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "fuzz.hpp"
#include "fuzz_util.hpp"

using namespace siaara;

namespace {

ReadOptions limits(bool lenient, bool header_only) {
    ReadOptions o;
    o.lenient = lenient;
    o.stop_before_pixel_data = header_only;
    o.max_elements = 200'000;
    o.max_items_per_sequence = 50'000;
    o.max_fragments = 50'000;
    o.max_inflated_size = 64u << 20;
    return o;
}

void round_trip(const DicomFile& f) {
    std::vector<std::uint8_t> out;
    if (!write_part10(f, out)) return;  // the writer may refuse what the lenient reader accepted
    auto again = read_part10(Bytes::copy_of(out.data(), out.size()), limits(true, false));
    FUZZ_REQUIRE(again.is_ok());
}

constexpr std::string_view kSyntaxes[] = {
    ts::ImplicitVRLittleEndian, ts::ExplicitVRLittleEndian, ts::ExplicitVRBigEndian,
    ts::DeflatedExplicitVRLittleEndian, "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.5",
};

}  // namespace

SIAARA_FUZZ_TARGET(part10) {
    const Bytes in = siaara_fuzz::to_bytes(data, size);
    for (int pass = 0; pass < 3; ++pass) {
        auto r = read_part10(in, limits(pass != 1, pass == 2));
        if (r && pass == 0) round_trip(*r);
    }
    // The same bytes as a raw data set body in a syntax chosen by the first byte.
    if (size > 0) {
        const std::string_view tsuid = kSyntaxes[data[0] % std::size(kSyntaxes)];
        const Bytes body = siaara_fuzz::to_bytes(data + 1, size - 1);
        auto ds = read_dataset(body, tsuid, limits(true, false));
        if (ds) {
            std::vector<std::uint8_t> out;
            (void)write_dataset(*ds, tsuid, out);
        }
        (void)read_dataset(body, tsuid, limits(false, false));
    }
    return 0;
}
