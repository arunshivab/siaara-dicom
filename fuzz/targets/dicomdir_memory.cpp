// Fuzz target: the DICOMDIR reader from memory (siaara_dicomdir_read_memory, PS3.10 8, PS3.3 F):
// untrusted read limits, record links (offsets, cycles, shared children, depth), the record limit
// and the File ID check. Never touches the disk.
#include <cstring>
#include <string>

#include "fuzz.hpp"
#include "siaara/siaara.h"

namespace {

struct Seen {
    std::size_t records = 0;
};

int32_t SIAARA_CALL on_record(void* user, const siaara_dicomdir_entry* e, const siaara_index_record* keys, const char* strings) {
    auto* s = static_cast<Seen*>(user);
    FUZZ_REQUIRE(e && keys && strings);
    FUZZ_REQUIRE(e->depth < 16);
    FUZZ_REQUIRE(e->file_status == SIAARA_OK || e->file_status == SIAARA_ERR_REJECTED);
    const std::string id(strings + keys->str_offset[SIAARA_IDX_FILE_PATH], keys->str_length[SIAARA_IDX_FILE_PATH]);
    if (e->file_status == SIAARA_OK && !id.empty()) {
        // A well-formed File ID: 1 to 8 components of A-Z 0-9 _ joined with '/', never a path.
        std::size_t parts = 1;
        FUZZ_REQUIRE(id.front() != '/' && id.back() != '/');
        for (char c : id) {
            if (c == '/') ++parts;
            else FUZZ_REQUIRE((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
        }
        FUZZ_REQUIRE(parts <= 8);
        FUZZ_REQUIRE(id.find("//") == std::string::npos);
    }
    ++s->records;
    return 0;
}

}  // namespace

SIAARA_FUZZ_TARGET(dicomdir_memory) {
    for (int pass = 0; pass < 2; ++pass) {
        siaara_dicomdir_read_options o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.include_inactive = pass;
        o.max_records = pass == 0 ? 0 : 64;  // the default, then a small limit
        Seen seen;
        siaara_dicomdir_summary sum;
        std::memset(&sum, 0, sizeof sum);
        sum.struct_size = sizeof sum;
        const siaara_status st = siaara_dicomdir_read_memory(data, size, &o, on_record, &seen, &sum);
        if (st == SIAARA_OK) {
            FUZZ_REQUIRE(sum.reported == seen.records);
            FUZZ_REQUIRE(sum.missing_files == 0);
            FUZZ_REQUIRE(pass == 0 || sum.records <= 64);
        }
    }
    return 0;
}
