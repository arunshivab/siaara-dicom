// Fuzz target: value parsers - DS / IS (clinical::parse_decimal, Dataset::get_double/get_int,
// parse_decimal_c), DA / TM / DT (clinical::parse_da/tm/dt, store::date_key and date ranges,
// deid::shift_date). Properties: format_ds output is at most 16 characters and parses back.
#include <cmath>

#include "clinical/common.hpp"
#include "core/dataset.hpp"
#include "core/number.hpp"
#include "deid/deid.hpp"
#include "fuzz.hpp"
#include "store/text.hpp"

using namespace siaara;

SIAARA_FUZZ_TARGET(values) {
    const std::string_view s(reinterpret_cast<const char*>(data), size);
    if (auto d = clinical::parse_decimal(s)) {
        if (std::isfinite(*d)) {
            const std::string f = clinical::format_ds(*d);
            FUZZ_REQUIRE(f.size() <= 16);
            FUZZ_REQUIRE(clinical::parse_decimal(f).has_value());
        }
    }
    (void)parse_decimal_c(s);
    Dataset ds;
    if (ds.set_string(0x00281050, VR::DS, s)) {
        const std::size_t vm = ds.value_multiplicity(0x00281050);
        for (std::size_t i = 0; i < vm && i < 8; ++i) (void)ds.get_double(0x00281050, i);
    }
    if (ds.set_string(0x00200013, VR::IS, s)) (void)ds.get_int(0x00200013, 0);
    (void)clinical::get_numbers(ds, 0x00281050, 1024);
    (void)clinical::parse_da(s);
    (void)clinical::parse_tm(s);
    std::optional<int> off;
    (void)clinical::parse_dt(s, &off);
    const std::size_t half = size / 2;
    (void)clinical::combine_da_tm(s.substr(0, half), s.substr(half));
    (void)store::date_key(s);
    std::uint32_t lo = 0, hi = 0;
    (void)store::parse_date_range(s, lo, hi);
    std::string out;
    const std::int32_t days = size > 0 ? -static_cast<std::int32_t>(data[0]) * 37 : -1;
    // A shifted single date (not a range) must still be a valid date.
    if (deid::shift_date(s, VR::DA, days, out) && s.find('-') == std::string_view::npos)
        FUZZ_REQUIRE(clinical::parse_da(out).has_value());
    (void)deid::shift_date(s, VR::DT, days, out);
    return 0;
}
