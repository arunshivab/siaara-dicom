// SIAARA DICOM Toolkit - network services answered from a local store (store::Store): Q/R C-FIND
// and the instance lists for C-MOVE / C-GET (PS3.4 C, Patient and Study Root, every level), the
// Q/R identifier builder, and the store behind the DICOMweb origin server (PS3.18). Internal to
// the C API (capi_net2.cpp); not installed.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "net/scp.hpp"
#include "net/services.hpp"
#include "store/store.hpp"
#include "web/server.hpp"

namespace siaara::capi {

struct StoreQrOptions {
    bool relational = false;            // SCP supports relational queries / retrieval
    bool fuzzy_person_names = false;
    bool combined_datetime = false;
    std::int32_t commitment_mode = 0;   // SIAARA_COMMIT_REPORT_* (kept with the other service settings)
    std::uint32_t max_find_results = 10000;
    std::uint32_t max_retrieve_instances = 65535;
    std::string retrieve_ae;            // Retrieve AE Title (0008,0054) in responses
};

// A copy of `ds` whose text values (and those of its sequence items) are UTF-8, with Specific
// Character Set ISO_IR 192 when it had another one: identifiers are matched against the store's
// UTF-8 index and worklist items.
Dataset identifier_utf8(const Dataset& ds);

// C-FIND (Patient / Study Root) answered from the store. Each match is passed to `emit` (false:
// stop) as the response identifier; `sent` counts them. Returns the final status.
std::uint16_t store_find(const store::Store& s, std::string_view sop_class, const Dataset& identifier, bool relational,
                         const net::MatchOptions& mo, const StoreQrOptions& opt,
                         const std::function<bool(const Dataset&)>& emit, std::uint32_t& sent);

// The instances a C-MOVE / C-GET identifier selects. Returns Success or a refusal status.
std::uint16_t store_retrieve(const store::Store& s, std::string_view sop_class, const Dataset& identifier,
                             bool relational, const StoreQrOptions& opt, std::vector<net::RetrieveItem>& items);

struct QrKeyValues {
    bool return_keys = false;
    std::vector<std::pair<Tag, std::string>> values;
};
// Q/R identifier at `level` (0 patient .. 3 image), written to `out`.
Status make_qr_identifier(int level, const QrKeyValues& keys, Dataset& out);

// The DICOMweb server's store over a local store.
web::Store dicomweb_store(store::Store& s, store::DuplicatePolicy policy, bool read_only, const StoreQrOptions& opt);

}  // namespace siaara::capi
