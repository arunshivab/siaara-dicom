// Test-only helper for tests/crosscheck/dicomdir_crosscheck.py.
//   dicomdir_tool make  <dir> <count> [first]  synthetic files <dir>/IMAGES/IMnnnnnn
//   dicomdir_tool write <root> <file>...       DICOMDIR (General Purpose CD) for the files
//   dicomdir_tool read  <DICOMDIR>             one JSON object per record, then a summary
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "store_synth.hpp"
#include "store/dicomdir.hpp"

using namespace siaara::store;

namespace {

std::string json(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o.push_back(static_cast<char>(c));
        }
    }
    return o + "\"";
}

int make(const char* dir, int first, int n) {
    for (int i = first; i < first + n; ++i) {
        store_synth::Spec s;
        const int series = i / 4, study = series / 2, patient = study / 2;
        s.charset = (patient % 2) ? "ISO_IR 100" : "";
        s.patient_id = "PX" + std::to_string(patient);
        s.patient_name = (patient % 2) ? "M\xC3\xBCller^Pat" + std::to_string(patient) : "Doe^Pat" + std::to_string(patient);
        s.study_uid = "1.2.826.0.1.88." + std::to_string(study);
        s.series_uid = s.study_uid + "." + std::to_string(series % 2);
        s.sop_uid = s.series_uid + "." + std::to_string(i % 4);
        s.series_number = series % 2 + 1;
        s.instance_number = i % 4 + 1;
        s.study_id = "S" + std::to_string(study);
        char name[32];
        std::snprintf(name, sizeof name, "IM%06d", i);
        if (!store_synth::write_spec(std::filesystem::path(dir) / "IMAGES" / name, s)) return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && !std::strcmp(argv[1], "make")) return make(argv[2], argc >= 5 ? std::atoi(argv[4]) : 0, std::atoi(argv[3]));
    if (argc >= 4 && !std::strcmp(argv[1], "write")) {
        std::vector<std::string> files(argv + 3, argv + argc);
        DicomdirWriteOptions o;
        o.file_set_id = "SIAARA";
        auto r = write_dicomdir(argv[2], files, o);
        if (!r) {
            std::fprintf(stderr, "write failed: %s\n", r.error().message.c_str());
            return 1;
        }
        std::printf("{\"files\":%zu,\"warnings\":%zu}\n", r->files, r->warnings.size());
        for (const auto& w : r->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        return 0;
    }
    if (argc >= 3 && !std::strcmp(argv[1], "read")) {
        auto r = read_dicomdir(argv[2], [](const DicomdirRecord& d) {
            std::printf("{\"index\":%zu,\"parent\":%lld,\"depth\":%u,\"offset\":%llu,\"type\":%s,\"patient_id\":%s,"
                        "\"patient_name\":%s,\"study_uid\":%s,\"series_uid\":%s,\"sop_uid\":%s,\"file\":%s,\"file_ok\":%s}\n",
                        d.index, static_cast<long long>(d.parent), d.depth, static_cast<unsigned long long>(d.offset),
                        json(d.keys[Str::RecordType]).c_str(), json(d.keys[Str::PatientId]).c_str(),
                        json(d.keys[Str::PatientName]).c_str(), json(d.keys[Str::StudyInstanceUid]).c_str(),
                        json(d.keys[Str::SeriesInstanceUid]).c_str(), json(d.keys[Str::SopInstanceUid]).c_str(),
                        json(d.keys[Str::FilePath]).c_str(), d.file_status.is_ok() ? "true" : "false");
            return true;
        });
        if (!r) {
            std::fprintf(stderr, "read failed: %s\n", r.error().message.c_str());
            return 1;
        }
        std::printf("{\"summary\":true,\"records\":%zu,\"reported\":%zu,\"files\":%zu,\"unreachable\":%zu,\"fallback\":%s}\n",
                    r->records, r->reported, r->files, r->unreachable, r->sequential_fallback ? "true" : "false");
        return 0;
    }
    std::fprintf(stderr, "usage: dicomdir_tool make <dir> <n> | write <root> <file>... | read <DICOMDIR>\n");
    return 2;
}
