// Test-only helper for tests/crosscheck/web_crosscheck.py (not installed).
//   web_tool tojson FILE.dcm            DICOM JSON of the data set on stdout
//   web_tool xmljson FILE.dcm           data set -> XML -> data set -> DICOM JSON on stdout
//   web_tool fromjson IN.json OUT.dcm   DICOM JSON -> Part 10 (explicit VR little endian)
//   web_tool serve PORT [FILE...]       DICOMweb server on 127.0.0.1:PORT/dicomweb with an in-memory
//                                       store preloaded with FILEs; runs until stdin is closed
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"
#include "web/server.hpp"
#include "web_memory_store.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {
int fail(const Status& s) {
    std::fprintf(stderr, "error: %s\n", s.message.c_str());
    return 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: web_tool tojson|xmljson FILE | fromjson IN OUT | serve PORT [FILE...]\n", stderr);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "tojson" || cmd == "xmljson") {
        auto f = read_file(argv[2]);
        if (!f) return fail(f.error());
        Dataset ds = f->dataset;
        if (cmd == "xmljson") {
            std::string x;
            if (Status st = dataset_to_xml(ds, x); !st) return fail(st);
            auto back = dataset_from_xml(x);
            if (!back) return fail(back.error());
            ds = std::move(back).value();
        }
        std::string js;
        if (Status st = dataset_to_json(ds, js); !st) return fail(st);
        std::fwrite(js.data(), 1, js.size(), stdout);
        return 0;
    }
    if (cmd == "fromjson" && argc >= 4) {
        auto b = io::read_file(argv[2]);
        if (!b) return fail(b.error());
        auto ds = dataset_from_json(b->as_chars());
        if (!ds) return fail(ds.error());
        DicomFile f;
        f.transfer_syntax = "1.2.840.10008.1.2.1";
        for (auto& e : ds->elements()) {
            if (tag_group(e.tag) == 2) f.meta.put(e);
            else f.dataset.append_sorted(e);
        }
        if (Status st = write_file(f, argv[3]); !st) return fail(st);
        return 0;
    }
    if (cmd == "serve") {
        siaara_test_web::MemoryStore mem;
        for (int i = 3; i < argc; ++i) {
            auto b = io::read_file(argv[i]);
            if (!b) return fail(b.error());
            if (Status st = mem.add_part10(*b); !st) std::fprintf(stderr, "skipped %s: %s\n", argv[i], st.message.c_str());
        }
        DicomwebServerConfig cfg;
        cfg.http.bind_address = "127.0.0.1";
        cfg.http.port = static_cast<std::uint16_t>(std::atoi(argv[2]));
        DicomwebServer server;
        if (Status st = server.start(cfg, mem.make_store()); !st) return fail(st);
        std::printf("listening %u\n", static_cast<unsigned>(server.port()));
        std::fflush(stdout);
        while (std::fgetc(stdin) != EOF) {
        }
        server.stop();
        return 0;
    }
    return 2;
}
