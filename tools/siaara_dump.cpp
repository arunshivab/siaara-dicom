// siaara-dump: prints the data elements of a DICOM file.
#include <cstdio>
#include <string>

#include "core/reader.hpp"
#include "dict/dictionary.hpp"
#include "tool_help.hpp"

using namespace siaara;

namespace {

void print_value(const Dataset& ds, const Element& e) {
    if (e.is_sequence()) {
        std::printf("(Sequence with %zu item(s))", e.items.size());
        return;
    }
    if (e.is_encapsulated()) {
        std::printf("(Encapsulated, %zu fragment(s))", e.fragments.size());
        return;
    }
    if (vr_is_string(e.vr)) {
        std::string_view s = trim_value(e.value.as_chars(), e.vr);
        if (s.size() > 64) std::printf("[%.*s...]", 64, s.data());
        else std::printf("[%.*s]", static_cast<int>(s.size()), s.data());
        return;
    }
    const std::size_t vm = ds.value_multiplicity(e.tag);
    if (vr_value_size(e.vr) > 1 && vm <= 16 && e.vr != VR::OW && e.vr != VR::OB) {
        for (std::size_t i = 0; i < vm; ++i) {
            if (auto v = ds.get_string(e.tag, i)) std::printf("%s%s", i ? "\\" : "", v->c_str());
        }
        return;
    }
    std::printf("(%zu bytes)", e.value.size());
}

// Private data elements: the private dictionary's keyword (or name), and how sure it is.
std::string private_label(const Dataset& ds, const Element& e, std::string& note) {
    const auto p = dict::lookup_private_element(ds, e.tag);
    if (!p) return {};
    std::string label(!p->keyword.empty() ? p->keyword : p->name);
    if (p->confidence == dict::PrivateConfidence::Assumed) note += ", creator assumed";
    if (p->match == dict::PrivateMatch::Fuzzy) note += ", creator spelled \"" + std::string(p->creator) + "\"";
    if (p->conflict) note += ", VR conflict";
    if (!note.empty()) note = "  (private dictionary" + note + ")";
    return label;
}

void dump(const Dataset& ds, int depth) {
    for (const Element& e : ds.elements()) {
        // Private tags first: the public lookup also matches repeating groups such as 7Fxx on odd groups.
        const auto* d = tag_is_private(e.tag) ? nullptr : dict::lookup(e.tag);
        std::string note;
        std::string label = d ? std::string(d->keyword) : std::string();
        if (tag_is_private(e.tag)) {
            const std::uint16_t el = tag_element(e.tag);
            if (el >= 0x0010 && el <= 0x00FF) label = "PrivateCreator";
            else label = private_label(ds, e, note);
        }
        if (label.empty()) label = "Unknown";
        std::printf("%*s(%04X,%04X) %.2s %-40s ", depth * 2, "", tag_group(e.tag), tag_element(e.tag),
                    vr_name(e.vr).data(), label.c_str());
        print_value(ds, e);
        std::printf("%s\n", note.c_str());
        for (std::size_t i = 0; i < e.items.size(); ++i) {
            std::printf("%*s  > Item %zu\n", depth * 2, "", i + 1);
            dump(e.items[i], depth + 2);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (siaara_tools::wants_help(argc, argv) || argc < 2) {
        std::fprintf(siaara_tools::usage_stream(), "usage: siaara-dump <file.dcm> [--headers-only]\n");
        return argc < 2 && siaara_tools::usage_stream() == stderr ? 2 : 0;
    }
    ReadOptions opt;
    if (argc > 2 && std::string(argv[2]) == "--headers-only") opt.stop_before_pixel_data = true;
    auto f = read_file(argv[1], opt);
    if (!f) {
        std::fprintf(stderr, "error: %s (offset %llu)\n", f.error().message.c_str(),
                     static_cast<unsigned long long>(f.error().offset));
        return 1;
    }
    std::printf("# Transfer syntax: %s\n# File meta:\n", f->transfer_syntax.c_str());
    dump(f->meta, 0);
    std::printf("# Data set:\n");
    dump(f->dataset, 0);
    for (const auto& w : f->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    return 0;
}
