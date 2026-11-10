// SIAARA example (C++): the C API from C++, with the handle owned by a unique_ptr.
//     read_file_cpp [file.dcm]    without an argument a sample file is written and read
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include <siaara/siaara.h>

#include "../c/make_sample.h"

namespace {

struct FileCloser {
    void operator()(siaara_file* f) const noexcept { siaara_file_free(f); }
};
using File = std::unique_ptr<siaara_file, FileCloser>;

void check(siaara_status status, const char* what) {
    if (status != SIAARA_OK) throw std::runtime_error(std::string(what) + ": " + siaara_last_error_message());
}

File open_file(const std::string& path) {
    siaara_file* raw = nullptr;
    check(siaara_file_open(path.data(), path.size(), nullptr, &raw), "open");
    return File(raw);
}

// The text of the element at `path` ("PatientName", "BeamSequence[0].BeamName", "(0010,0020)").
std::string text_at(const siaara_dataset* ds, const std::string& path) {
    siaara_path_value value{};
    value.struct_size = sizeof value;
    std::size_t needed = 0;
    // First call: the size; second call: the text.
    siaara_status s = siaara_get_by_path(ds, path.data(), path.size(), &value, nullptr, 0, &needed);
    if (s != SIAARA_OK && s != SIAARA_ERR_BUFFER_TOO_SMALL) check(s, path.c_str());
    std::string text(needed + 1, '\0');
    check(siaara_get_by_path(ds, path.data(), path.size(), &value, text.data(), text.size(), &needed), path.c_str());
    text.resize(needed);
    return text;
}

}  // namespace

int main(int argc, char** argv) try {
    const std::string path = argc > 1 ? argv[1] : "siaara_example_cpp.dcm";
    if (argc < 2 && example_make_sample(path.c_str()) != 0) return 1;

    File file = open_file(path);
    siaara_dataset* ds = nullptr;
    check(siaara_file_dataset(file.get(), &ds), "dataset");
    std::printf("PatientName = %s\n", text_at(ds, "PatientName").c_str());
    std::printf("PatientID   = %s\n", text_at(ds, "(0010,0020)").c_str());

    siaara_pixel_value pixel{};
    pixel.struct_size = sizeof pixel;
    check(siaara_file_pixel_value(file.get(), 0, 1, 0, 0, &pixel), "pixel value");
    std::printf("pixel (1,0): stored %lld, modality value %g\n", static_cast<long long>(pixel.stored), pixel.modality_value);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
