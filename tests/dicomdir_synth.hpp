// Test helpers for the DICOMDIR reader from memory and the grouping with a DICOMDIR
// (tests/test_dicomdir_memory.cpp, tests/test_grouping.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "crosscheck/store_synth.hpp"
#include "siaara/siaara.h"
#include "store/dicomdir.hpp"

namespace dicomdir_synth {

using store_synth::Spec;
namespace fs = std::filesystem;


struct DirRec {
    uint32_t index, depth;
    int32_t parent, in_use;
    siaara_status file_status;
    uint64_t offset;
    std::string type, file_text, patient_id, sop_uid;
};

inline int32_t SIAARA_CALL collect(void* user, const siaara_dicomdir_entry* e, const siaara_index_record* keys, const char* strings) {
    auto* v = static_cast<std::vector<DirRec>*>(user);
    auto str = [&](int id) { return std::string(strings + keys->str_offset[id], keys->str_length[id]); };
    v->push_back({e->index, e->depth, e->parent, e->in_use, e->file_status, e->offset, str(SIAARA_IDX_RECORD_TYPE),
                  str(SIAARA_IDX_FILE_PATH), str(SIAARA_IDX_PATIENT_ID), str(SIAARA_IDX_SOP_INSTANCE_UID)});
    return 0;
}

inline siaara_dicomdir_read_options dd_options() {
    siaara_dicomdir_read_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.resolve_files = 1;
    o.case_insensitive = 1;
    return o;
}

inline siaara_status read_mem(const std::vector<std::uint8_t>& b, std::vector<DirRec>& out, siaara_dicomdir_summary* sum = nullptr,
                       const siaara_dicomdir_read_options* o = nullptr) {
    out.clear();
    siaara_dicomdir_summary s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    const siaara_status st = siaara_dicomdir_read_memory(b.data(), b.size(), o, collect, &out, &s);
    if (sum) *sum = s;
    return st;
}

inline siaara_status read_path(const fs::path& p, std::vector<DirRec>& out, siaara_dicomdir_summary* sum = nullptr,
                        const siaara_dicomdir_read_options* o = nullptr) {
    out.clear();
    siaara_dicomdir_summary s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    const std::string ps = p.string();
    const siaara_status st = siaara_dicomdir_read(ps.data(), ps.size(), o, collect, &out, &s);
    if (sum) *sum = s;
    return st;
}

// A file set of `patients` patients x 2 studies x 2 series x 2 images, written as media with a DICOMDIR.
inline std::vector<std::string> make_media(const fs::path& src, const fs::path& media, int patients = 2) {
    std::vector<std::string> files;
    for (int p = 1; p <= patients; ++p)
        for (int s = 1; s <= 2; ++s)
            for (int e = 1; e <= 2; ++e)
                for (int i = 1; i <= 2; ++i) {
                    Spec sp;
                    sp.patient_id = "P" + std::to_string(p);
                    sp.study_uid = "1.2.99." + std::to_string(p) + "." + std::to_string(s);
                    sp.series_uid = sp.study_uid + "." + std::to_string(e);
                    sp.sop_uid = sp.series_uid + "." + std::to_string(i);
                    sp.instance_number = i;
                    const fs::path f = src / ("f" + std::to_string(files.size()) + ".dcm");
                    store_synth::write_spec(f, sp);
                    files.push_back(f.string());
                }
    siaara::store::DicomdirWriteOptions o;
    (void)siaara::store::write_dicomdir(media.string(), files, o);
    return files;
}

inline void patch32(std::vector<std::uint8_t>& b, std::uint64_t at, std::uint32_t v) { siaara::store_le32(b.data() + at, v); }


}  // namespace dicomdir_synth
