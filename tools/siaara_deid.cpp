// siaara-deid: de-identifies DICOM files per PS3.15 Annex E (Basic Application Level
// Confidentiality Profile and Options).
//
//   siaara-deid [options] <input file or folder> <output folder>
//
// Output files are named after their new SOP Instance UID, because input names and folder names
// often contain patient names. Progress lines never show input paths or values.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "deid/deid.hpp"
#include "deid/sha256.hpp"

using namespace siaara;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: siaara-deid [options] <input file|folder> <output folder>\n"
                 "  --key-file F            secret key (raw bytes, >= 16) for UIDs, IDs and date offsets;\n"
                 "                          without it a random key is used (results not repeatable)\n"
                 "  --retain-safe-private   --retain-uids   --retain-device-id   --retain-institution-id\n"
                 "  --retain-patient-chars  --retain-full-dates   --modified-dates\n"
                 "  --clean-descriptors     --clean-structured    --clean-graphics\n"
                 "  --remove-optional       resolve X/Z, X/D, X/Z/D, X/Z/U* as X (default: keep conformance)\n"
                 "  --region F,X,Y,W,H      blank a rectangle (F = frame or 'all'); repeatable\n"
                 "  --pixels-clean          assert that the regions cover all burned-in text\n"
                 "  --safe-words F          file of words the clean action may keep\n"
                 "  --allow-burned-in       write files even when burned-in text may remain\n");
}

bool parse_region(const char* s, deid::PixelRegion& r) {
    unsigned long v[5] = {};
    std::string text(s);
    std::size_t start = 0;
    for (int i = 0; i < 5; ++i) {
        const std::size_t comma = text.find(',', start);
        std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (part.empty() || (i < 4 && comma == std::string::npos)) return false;
        if (i == 0 && part == "all") {
            v[0] = deid::kAllFrames;
        } else {
            char* end = nullptr;
            v[i] = std::strtoul(part.c_str(), &end, 10);
            if (!end || *end || v[i] > 0xFFFFFFFFul) return false;
        }
        start = comma == std::string::npos ? text.size() : comma + 1;
    }
    r = {static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[1]), static_cast<std::uint32_t>(v[2]),
         static_cast<std::uint32_t>(v[3]), static_cast<std::uint32_t>(v[4])};
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    deid::Options opt;
    std::vector<std::uint8_t> key;
    std::vector<std::string> positional;
    bool allow_burned_in = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--key-file") {
            const char* f = next();
            if (!f) { usage(); return 2; }
            auto r = io::read_file(f);
            if (!r) { std::fprintf(stderr, "cannot read key file\n"); return 2; }
            key.assign(r->data(), r->data() + r->size());
        } else if (a == "--retain-safe-private") opt.options |= deid::RetainSafePrivate;
        else if (a == "--retain-uids") opt.options |= deid::RetainUids;
        else if (a == "--retain-device-id") opt.options |= deid::RetainDeviceIdentity;
        else if (a == "--retain-institution-id") opt.options |= deid::RetainInstitutionIdentity;
        else if (a == "--retain-patient-chars") opt.options |= deid::RetainPatientCharacteristics;
        else if (a == "--retain-full-dates") opt.options |= deid::RetainLongitudinalFullDates;
        else if (a == "--modified-dates") opt.options |= deid::RetainLongitudinalModifiedDates;
        else if (a == "--clean-descriptors") opt.options |= deid::CleanDescriptors;
        else if (a == "--clean-structured") opt.options |= deid::CleanStructuredContent;
        else if (a == "--clean-graphics") opt.options |= deid::CleanGraphics;
        else if (a == "--remove-optional") opt.combined = deid::CombinedPolicy::Remove;
        else if (a == "--pixels-clean") opt.pixel_data_clean = true;
        else if (a == "--allow-burned-in") allow_burned_in = true;
        else if (a == "--region") {
            const char* r = next();
            deid::PixelRegion pr;
            if (!r || !parse_region(r, pr)) { usage(); return 2; }
            opt.pixel_regions.push_back(pr);
        } else if (a == "--safe-words") {
            const char* f = next();
            if (!f) { usage(); return 2; }
            auto r = io::read_file(f);
            if (!r) { std::fprintf(stderr, "cannot read safe words file\n"); return 2; }
            std::string w;
            for (char c : std::string(r->as_chars()) + "\n") {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                    if (!w.empty()) opt.safe_words.push_back(w);
                    w.clear();
                } else {
                    w.push_back(c);
                }
            }
        } else if (!a.empty() && a[0] == '-') {
            usage();
            return 2;
        } else {
            positional.emplace_back(a);
        }
    }
    if (positional.size() != 2) { usage(); return 2; }
    if (key.empty()) {
        std::random_device rd;
        key.resize(32);
        for (auto& b : key) b = static_cast<std::uint8_t>(rd());
        std::fprintf(stderr, "note: no --key-file; using a random key (UIDs and offsets will differ between runs)\n");
    }
    if (key.size() < 16) {
        std::fprintf(stderr, "key file must hold at least 16 bytes\n");
        return 2;
    }
    opt.key = key;

    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> inputs;
    const fs::path in(positional[0]);
    if (fs::is_directory(in, ec)) {
        for (fs::recursive_directory_iterator it(in, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) inputs.push_back(it->path());
        std::sort(inputs.begin(), inputs.end());
    } else {
        inputs.push_back(in);
    }
    const fs::path out_dir(positional[1]);
    fs::create_directories(out_dir, ec);
    if (!fs::is_directory(out_dir, ec)) {
        std::fprintf(stderr, "cannot create the output folder\n");
        deid::secure_wipe(key.data(), key.size());
        return 2;
    }

    int failures = 0, written = 0, skipped = 0, held = 0;
    for (std::size_t n = 0; n < inputs.size(); ++n) {
        auto f = read_file(inputs[n].string());
        if (!f) {
            ++skipped;  // not DICOM, or unreadable
            continue;
        }
        deid::Report rep;
        if (Status s = deid::deidentify(*f, opt, &rep); !s) {
            std::fprintf(stderr, "input %zu: not de-identified: %s\n", n + 1, s.message.c_str());
            ++failures;
            continue;
        }
        std::string name(f->meta.get_raw_string(tags::MediaStorageSOPInstanceUID).value_or(""));
        while (!name.empty() && (name.back() == '\0' || name.back() == ' ')) name.pop_back();
        if (name.empty()) name = "instance" + std::to_string(n + 1);
        if ((rep.flags & deid::NotFullyDeidentified) && !allow_burned_in) {
            std::fprintf(stderr, "input %zu: held back (%s%s); use --region/--pixels-clean or --allow-burned-in\n",
                         n + 1, (rep.flags & deid::BurnedInAnnotationRemains) ? "burned-in annotation may remain" : "",
                         (rep.flags & deid::RecognizableFeaturesRemain) ? " recognizable visual features" : "");
            ++held;
            continue;
        }
        const fs::path target = out_dir / (name + ".dcm");
        if (Status s = write_file(*f, target.string()); !s) {
            std::fprintf(stderr, "input %zu: write failed: %s\n", n + 1, s.message.c_str());
            ++failures;
            continue;
        }
        ++written;
        std::printf("%s.dcm: removed %zu, emptied %zu, dummy %zu, cleaned %zu, uids %zu, dates %zu, private kept %zu%s\n",
                    name.c_str(), rep.removed, rep.emptied, rep.dummied, rep.cleaned, rep.uids_replaced,
                    rep.dates_shifted, rep.private_kept,
                    (rep.flags & deid::NotFullyDeidentified) ? " [NOT FULLY DE-IDENTIFIED]" : "");
    }
    deid::secure_wipe(key.data(), key.size());
    std::printf("%d written, %d held back, %d failed, %d skipped (not DICOM)\n", written, held, failures, skipped);
    return failures ? 1 : (held ? 3 : 0);
}
