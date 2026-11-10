// Regression tests of security review round 4 and the independent testing findings, DICOMweb
// side: R-110 (retrieve decided from the index, streamed), R-116 / R-152 (incomplete and foreign
// parts), R-120 (response deadline), T-P3-1 (QIDO Retrieve URL). Loopback, ours against ours.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/socket.hpp"
#include "siaara/siaara.h"
#include "capi/capi_internal.hpp"
#include "store/index.hpp"
#include "store/store.hpp"
#include "test.hpp"
#include "web/client.hpp"
#include "web/dicom_json.hpp"
#include "web/dicom_xml.hpp"
#include "web/http.hpp"
#include "web/multipart.hpp"
#include "web/server.hpp"

using namespace siaara;

namespace {

constexpr const char* kMR = "1.2.840.10008.5.1.4.1.1.4";

std::filesystem::path temp_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_sec4bw_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

DicomFile make_image(const std::string& sop, const std::string& study, const std::string& series) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kMR);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, study);
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, series);
    (void)ds.set_string(tags::PatientName, VR::PN, "Web^Four");
    (void)ds.set_string(tags::PatientID, VR::LO, "W4");
    (void)ds.set_string(tags::Modality, VR::CS, "MR");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_uint16(tags::Rows, 64);
    (void)ds.set_uint16(tags::Columns, 64);
    (void)ds.set_uint16(tags::BitsAllocated, 8);
    (void)ds.set_uint16(tags::BitsStored, 8);
    (void)ds.set_uint16(tags::HighBit, 7);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    std::vector<std::uint8_t> px(64 * 64);
    for (std::size_t k = 0; k < px.size(); ++k) px[k] = static_cast<std::uint8_t>((k * 7) & 0x7F);
    (void)ds.set_bytes(tags::PixelData, VR::OB, Bytes::copy_of(px.data(), px.size()));
    return f;
}

std::vector<std::uint8_t> part10(const DicomFile& f) {
    std::vector<std::uint8_t> bytes;
    (void)write_part10(f, bytes);
    return bytes;
}

std::vector<std::uint8_t> truncated_part10(const DicomFile& f) {
    std::vector<std::uint8_t> b = part10(f);
    b.resize(b.size() - 62);
    return b;
}

struct StoreOpen {
    siaara_store* store = nullptr;
    explicit StoreOpen(const std::filesystem::path& p) {
        const std::string s = p.string();
        (void)siaara_store_open(s.c_str(), s.size(), nullptr, &store);
    }
    ~StoreOpen() { siaara_store_free(store); }
    StoreOpen(const StoreOpen&) = delete;
    StoreOpen& operator=(const StoreOpen&) = delete;
    uint64_t instances() const {
        siaara_store_stats st;
        std::memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        if (siaara_store_stats_get(store, &st) != SIAARA_OK) return 0;
        return st.instances;
    }
};

// The index flags of an instance (0xFFFFFFFF when it is not in the index).
std::uint32_t index_flags(const siaara_store* s, const std::string& sop) {
    store::QueryFilter f;
    f.level = store::Level::Instance;
    f.sop_instance_uid = sop;
    auto r = s->store->query(f);
    return r && !r->rows.empty() ? r->rows[0].flags : 0xFFFFFFFFu;
}

std::uint32_t area_items(const siaara_store* s, int32_t area) {
    uint32_t n = 0;
    (void)siaara_store_quarantine_count(s, area, &n, nullptr);
    return n;
}

siaara_web_server_options server_options() {
    siaara_web_server_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    return o;
}

// POSTs the parts as a STOW-RS request and returns the response.
Result<web::http::Response> stow(std::uint16_t port, const std::vector<std::vector<std::uint8_t>>& parts) {
    web::multipart::Writer w;
    for (const auto& p : parts) w.add("application/dicom", std::string_view(reinterpret_cast<const char*>(p.data()), p.size()));
    web::http::Request rq;
    rq.method = "POST";
    rq.target = "/dicomweb/studies";
    rq.body = w.finish();
    rq.headers.push_back({"Content-Type", w.content_type("application/dicom")});
    rq.headers.push_back({"Accept", "application/dicom+json"});
    web::http::Client c("127.0.0.1", port);
    return c.send(rq);
}

std::int64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

// ---------------------------------------------------------------- R-110
TEST_CASE("web/sec4 R-110 a retrieve is refused from the index sizes before any instance is opened, else streamed") {
    const auto dir = temp_dir("r110");
    std::vector<web::StoredInstance> stored;
    for (int i = 0; i < 3; ++i) {
        const std::string path = (dir / ("i" + std::to_string(i) + ".dcm")).string();
        const DicomFile f = make_image("2.25.6100" + std::to_string(i), "2.25.61900", "2.25.61901");
        REQUIRE(write_file(f, path).is_ok());
        web::StoredInstance inst;
        inst.path = path;
        inst.size = std::filesystem::file_size(path);
        inst.transfer_syntax = f.transfer_syntax;
        stored.push_back(inst);
    }
    const std::uint64_t one = stored[0].size;
    std::atomic<int> opened{0};
    web::Store store;
    store.locate = [&](const web::InstanceKey& k, std::vector<web::StoredInstance>& out) -> Status {
        if (k.study != "2.25.61900") return Status(Errc::NotFound, "not found");
        out = stored;
        return Status::ok();
    };
    store.open = [&](const web::StoredInstance& inst) -> Result<Bytes> {
        opened.fetch_add(1);
        return io::map_file(inst.path, false);
    };
    web::DicomwebServerConfig cfg;
    cfg.max_response_bytes = one * 2 + 10;  // two instances fit, three do not
    web::DicomwebServer server;
    REQUIRE(server.start(cfg, store).is_ok());
    web::DicomwebClientConfig cc;
    cc.base_url = "http://127.0.0.1:" + std::to_string(server.port()) + "/dicomweb";
    {
        web::DicomwebClient c(cc);
        auto r = c.retrieve("2.25.61900", "", "");
        CHECK(!r.is_ok());  // 413
        CHECK_EQ(opened.load(), 0);  // answered from the sizes alone: no file was opened
        // Metadata of the same study is not subject to the stored-size cap and needs no pixel data.
        auto m = c.retrieve_metadata("2.25.61900", "", "");
        REQUIRE(m.is_ok());
        CHECK_EQ(m->size(), 3u);
        CHECK_EQ(opened.load(), 3);
    }
    server.stop();
    // With room for all three the reply is streamed part by part (chunked) and arrives whole.
    cfg.max_response_bytes = one * 4;
    web::DicomwebServer server2;
    REQUIRE(server2.start(cfg, store).is_ok());
    cc.base_url = "http://127.0.0.1:" + std::to_string(server2.port()) + "/dicomweb";
    {
        web::http::Request rq;
        rq.method = "GET";
        rq.target = "/dicomweb/studies/2.25.61900";
        rq.headers.push_back({"Accept", "multipart/related; type=\"application/dicom\"; transfer-syntax=*"});
        web::http::Client raw("127.0.0.1", server2.port());
        auto rs = raw.send(rq);
        REQUIRE(rs.is_ok());
        CHECK_EQ(rs->status, 200);
        const std::string* te = web::http::find_header(rs->headers, "Transfer-Encoding");
        CHECK(te && *te == "chunked");
        CHECK(web::http::find_header(rs->headers, "Content-Length") == nullptr);
        const std::string* ct = web::http::find_header(rs->headers, "Content-Type");
        REQUIRE(ct);
        auto mt = web::http::parse_media_type(*ct);
        REQUIRE(mt.is_ok());
        const std::string* boundary = mt->param("boundary");
        REQUIRE(boundary);
        auto parts = web::multipart::parse(rs->body, *boundary, web::multipart::Limits{});
        REQUIRE(parts.is_ok());
        REQUIRE(parts->size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            auto disk = io::read_file(stored[i].path);
            REQUIRE(disk.is_ok());
            CHECK(parts.value()[i].body == disk->as_chars());  // the stored bytes, unchanged
        }
        web::DicomwebClient c(cc);
        auto r = c.retrieve("2.25.61900", "", "");
        REQUIRE(r.is_ok());
        CHECK_EQ(r->size(), 3u);
    }
    server2.stop();
}

// ---------------------------------------------------------------- R-116 (STOW-RS)
TEST_CASE("web/sec4 R-116 STOW-RS refuses an incomplete part by default, or stores it verbatim with a warning") {
    const auto whole = part10(make_image("2.25.62001", "2.25.62900", "2.25.62901"));
    const auto cut = truncated_part10(make_image("2.25.62002", "2.25.62900", "2.25.62901"));
    std::vector<std::uint8_t> raw_dataset;  // a complete data set without preamble and file meta
    REQUIRE(write_dataset(make_image("2.25.62003", "2.25.62900", "2.25.62901").dataset, ts::ExplicitVRLittleEndian,
                          raw_dataset)
                .is_ok());
    {  // REFUSE.
        const auto dir = temp_dir("r116stow_a");
        StoreOpen st(dir / "store");
        siaara_web_server_options o = server_options();
        o.incomplete_policy = SIAARA_INCOMPLETE_REFUSE;
        siaara_web_server* ws = nullptr;
        REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
        uint32_t port = 0;
        REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
        auto rs = stow(static_cast<std::uint16_t>(port), {whole, cut});
        REQUIRE(rs.is_ok());
        CHECK_EQ(rs->status, 202);  // one stored, one failed
        auto body = web::dataset_from_json(rs->body);
        REQUIRE(body.is_ok());
        const Element* failed = body->find(0x00081198);
        REQUIRE(failed != nullptr);
        REQUIRE(failed->items.size() == 1);
        CHECK(failed->items[0].get_string(0x00081155).value_or("") == "2.25.62002");
        CHECK_EQ(failed->items[0].get_int(0x00081197).value_or(0), 0xC000);
        CHECK(st.instances() == 1);
        // Alone, the incomplete part gives 409 and stores nothing.
        auto rs2 = stow(static_cast<std::uint16_t>(port), {cut});
        REQUIRE(rs2.is_ok());
        CHECK_EQ(rs2->status, 409);
        CHECK(st.instances() == 1);
        // A complete data set without a preamble is stored as a new Part 10 file, and reported.
        auto rs3 = stow(static_cast<std::uint16_t>(port), {raw_dataset});
        REQUIRE(rs3.is_ok());
        CHECK_EQ(rs3->status, 202);
        auto body3 = web::dataset_from_json(rs3->body);
        REQUIRE(body3.is_ok());
        const Element* ok3 = body3->find(0x00081199);
        REQUIRE(ok3 != nullptr);
        REQUIRE(ok3->items.size() == 1);
        CHECK_EQ(ok3->items[0].get_int(0x00081196).value_or(0), 0xB000);
        CHECK(st.instances() == 2);
        siaara_web_server_free(ws);
    }
    {  // ACCEPT_AS_RECEIVED: stored exactly as received, Warning Reason in the response.
        const auto dir = temp_dir("r116stow_b");
        StoreOpen st(dir / "store");
        siaara_web_server_options o = server_options();
        o.incomplete_policy = SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED;
        siaara_web_server* ws = nullptr;
        REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
        uint32_t port = 0;
        REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
        auto rs = stow(static_cast<std::uint16_t>(port), {cut});
        REQUIRE(rs.is_ok());
        CHECK_EQ(rs->status, 202);
        auto body = web::dataset_from_json(rs->body);
        REQUIRE(body.is_ok());
        const Element* ok = body->find(0x00081199);
        REQUIRE(ok != nullptr);
        REQUIRE(ok->items.size() == 1);
        CHECK_EQ(ok->items[0].get_int(0x00081196).value_or(0), 0xB006);
        CHECK(st.instances() == 1);
        // B2: flagged in the index, as the Storage SCP does.
        CHECK((index_flags(st.store, "2.25.62002") & store::kIncompleteRead) != 0);
        bool same = false;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir / "store")) {
            if (!e.is_regular_file() || e.file_size() != cut.size()) continue;
            auto b = io::read_file(e.path().string());
            same = same || (b && std::memcmp(b->data(), cut.data(), cut.size()) == 0);
        }
        CHECK(same);
        // An incomplete data set without a preamble could only be stored rewritten: refused here too.
        std::vector<std::uint8_t> raw_cut = raw_dataset;
        raw_cut.resize(raw_cut.size() - 62);
        auto rs2 = stow(static_cast<std::uint16_t>(port), {raw_cut});
        REQUIRE(rs2.is_ok());
        CHECK_EQ(rs2->status, 409);
        CHECK(st.instances() == 1);
        siaara_web_server_free(ws);
    }
    // The answer for an ACCEPTED incomplete part follows quarantine_response (P3-1): SUCCESS -
    // 200 and no Warning Reason; FAILURE - listed as failed with 0xC000 although it is stored.
    for (const int32_t answer : {SIAARA_QUARANTINE_ANSWER_SUCCESS, SIAARA_QUARANTINE_ANSWER_FAILURE}) {
        const auto dir = temp_dir("r116stow_answer");
        StoreOpen st(dir / "store");
        siaara_web_server_options o = server_options();
        o.incomplete_policy = SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED;
        o.quarantine_response = answer;
        siaara_web_server* ws = nullptr;
        REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
        uint32_t port = 0;
        REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
        auto rs = stow(static_cast<std::uint16_t>(port), {cut});
        REQUIRE(rs.is_ok());
        auto body = web::dataset_from_json(rs->body);
        REQUIRE(body.is_ok());
        CHECK(st.instances() == 1);
        CHECK((index_flags(st.store, "2.25.62002") & store::kIncompleteRead) != 0);
        if (answer == SIAARA_QUARANTINE_ANSWER_SUCCESS) {
            CHECK_EQ(rs->status, 200);
            const Element* ok = body->find(0x00081199);
            REQUIRE(ok != nullptr);
            REQUIRE(ok->items.size() == 1);
            CHECK(!ok->items[0].contains(0x00081196));
        } else {
            CHECK_EQ(rs->status, 409);
            const Element* bad = body->find(0x00081198);
            REQUIRE(bad != nullptr);
            REQUIRE(bad->items.size() == 1);
            CHECK_EQ(bad->items[0].get_int(0x00081197).value_or(0), 0xC000);
        }
        siaara_web_server_free(ws);
    }
    {  // Default (QUARANTINE): kept outside the archive; Warning Reason 0xB007 and no Retrieve URL.
        const auto dir = temp_dir("r116stow_c");
        StoreOpen st(dir / "store");
        siaara_web_server_options o = server_options();
        siaara_web_server* ws = nullptr;
        REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
        uint32_t port = 0;
        REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
        auto rs = stow(static_cast<std::uint16_t>(port), {cut});
        REQUIRE(rs.is_ok());
        CHECK_EQ(rs->status, 202);
        auto body = web::dataset_from_json(rs->body);
        REQUIRE(body.is_ok());
        const Element* ok = body->find(0x00081199);
        REQUIRE(ok != nullptr);
        REQUIRE(ok->items.size() == 1);
        CHECK_EQ(ok->items[0].get_int(0x00081196).value_or(0), 0xB007);
        CHECK(!ok->items[0].contains(0x00081190));
        CHECK(st.instances() == 0);
        uint32_t items = 0;
        REQUIRE(siaara_store_quarantine_count(st.store, SIAARA_AREA_QUARANTINE, &items, nullptr) == SIAARA_OK);
        CHECK_EQ(items, 1u);
        siaara_quarantine_record rec;
        std::memset(&rec, 0, sizeof rec);
        rec.struct_size = sizeof rec;
        REQUIRE(siaara_store_quarantine_get(st.store, SIAARA_AREA_QUARANTINE, 0, &rec) == SIAARA_OK);
        CHECK(std::string(rec.source) == "STOW-RS");
        CHECK(rec.size == cut.size());  // the part's bytes, unchanged
        siaara_web_server_free(ws);
    }
}

// ---------------------------------------------------------------- R-116, R-152 (retrieve to store)
TEST_CASE("web/sec4 R-116 R-152 retrieve-to-store applies the incomplete policy and checks the UIDs") {
    const auto dir = temp_dir("r116ret");
    StoreOpen origin(dir / "origin");
    const auto whole = part10(make_image("2.25.63001", "2.25.63900", "2.25.63901"));
    const auto cut = truncated_part10(make_image("2.25.63002", "2.25.63900", "2.25.63901"));
    REQUIRE(siaara_store_add_memory(origin.store, whole.data(), whole.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
    REQUIRE(siaara_store_add_memory(origin.store, cut.data(), cut.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
    siaara_web_server_options o = server_options();
    siaara_web_server* ws = nullptr;
    REQUIRE(siaara_web_server_start(&o, origin.store, &ws) == SIAARA_OK);
    uint32_t port = 0;
    REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/dicomweb";
    siaara_web_client* wc = nullptr;
    REQUIRE(siaara_web_client_create(base.data(), base.size(), 5000, &wc) == SIAARA_OK);
    const char* as_stored[] = {"*"};
    {  // Default (QUARANTINE): the truncated instance is not in the archive or the folder, and not reported as stored.
        StoreOpen dest(dir / "dest_a");
        const std::string folder = (dir / "files_a").string();
        std::filesystem::create_directories(folder);
        uint32_t stored = 0, failed = 0;
        CHECK(siaara_web_wado_retrieve_to(wc, "2.25.63900", nullptr, nullptr, as_stored, 1, dest.store, SIAARA_DUP_KEEP,
                                          folder.c_str(), nullptr, nullptr, &stored, &failed) != SIAARA_OK);
        CHECK_EQ(stored, 1u);
        CHECK_EQ(failed, 1u);
        CHECK(dest.instances() == 1);
        CHECK(std::filesystem::exists(std::filesystem::path(folder) / "2.25.63001.dcm"));
        CHECK(!std::filesystem::exists(std::filesystem::path(folder) / "2.25.63002.dcm"));
    }
    {  // ACCEPT_AS_RECEIVED: kept byte for byte, and the report says so.
        StoreOpen dest(dir / "dest_b");
        const std::string folder = (dir / "files_b").string();
        std::filesystem::create_directories(folder);
        siaara_web_retrieve_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.store = dest.store;
        ro.folder = folder.c_str();
        ro.transfer_syntaxes = as_stored;
        ro.transfer_syntax_count = 1;
        ro.incomplete_policy = SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED;
        siaara_web_retrieve_report rep;
        std::memset(&rep, 0, sizeof rep);
        rep.struct_size = sizeof rep;
        CHECK(siaara_web_wado_retrieve_to_ex(wc, "2.25.63900", nullptr, nullptr, &ro, nullptr, nullptr, &rep) == SIAARA_OK);
        CHECK_EQ(rep.stored, 2u);
        CHECK_EQ(rep.failed, 0u);
        CHECK_EQ(rep.incomplete_accepted, 1u);
        CHECK_EQ(rep.rewritten, 0u);
        // B2: the incomplete instance is flagged in the index; the complete one is not.
        CHECK((index_flags(dest.store, "2.25.63002") & store::kIncompleteRead) != 0);
        CHECK((index_flags(dest.store, "2.25.63001") & store::kIncompleteRead) == 0);
        auto kept = io::read_file((std::filesystem::path(folder) / "2.25.63002.dcm").string());
        REQUIRE(kept.is_ok());
        REQUIRE(kept->size() == cut.size());
        CHECK(std::memcmp(kept->data(), cut.data(), cut.size()) == 0);
    }
    {  // REFUSE_KEEP_COPY (P3-3): refused, with a copy of the bytes in the "rejected" area.
        StoreOpen dest(dir / "dest_keep");
        siaara_web_retrieve_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.store = dest.store;
        ro.transfer_syntaxes = as_stored;
        ro.transfer_syntax_count = 1;
        ro.incomplete_policy = SIAARA_INCOMPLETE_REFUSE_KEEP_COPY;
        siaara_web_retrieve_report rep;
        std::memset(&rep, 0, sizeof rep);
        rep.struct_size = sizeof rep;
        CHECK(siaara_web_wado_retrieve_to_ex(wc, "2.25.63900", nullptr, nullptr, &ro, nullptr, nullptr, &rep) != SIAARA_OK);
        CHECK_EQ(rep.stored, 1u);
        CHECK_EQ(rep.incomplete_refused, 1u);
        CHECK(dest.instances() == 1);
        CHECK_EQ(area_items(dest.store, SIAARA_AREA_QUARANTINE), 0u);
        REQUIRE(area_items(dest.store, SIAARA_AREA_REJECTED) == 1u);
        siaara_quarantine_record qr;
        std::memset(&qr, 0, sizeof qr);
        qr.struct_size = sizeof qr;
        REQUIRE(siaara_store_quarantine_get(dest.store, SIAARA_AREA_REJECTED, 0, &qr) == SIAARA_OK);
        CHECK(std::string(qr.source) == "WADO-RS");
        CHECK_EQ(qr.size, cut.size());
        std::vector<std::uint8_t> kept(cut.size());
        size_t need = 0;
        REQUIRE(siaara_store_quarantine_bytes(dest.store, SIAARA_AREA_REJECTED, qr.id, kept.data(), kept.size(), &need) == SIAARA_OK);
        CHECK(kept == cut);
    }
    siaara_web_client_free(wc);
    siaara_web_server_free(ws);

    // R-152: a server that answers with an instance of another study.
    const std::string foreign = (dir / "foreign.dcm").string();
    REQUIRE(write_file(make_image("2.25.63777", "2.25.63999", "2.25.63998"), foreign).is_ok());
    web::Store store;
    store.locate = [&](const web::InstanceKey&, std::vector<web::StoredInstance>& out) -> Status {
        web::StoredInstance inst;
        inst.path = foreign;
        inst.size = std::filesystem::file_size(foreign);
        out.push_back(inst);
        return Status::ok();
    };
    web::DicomwebServer server;
    REQUIRE(server.start(web::DicomwebServerConfig{}, store).is_ok());
    const std::string base2 = "http://127.0.0.1:" + std::to_string(server.port()) + "/dicomweb";
    REQUIRE(siaara_web_client_create(base2.data(), base2.size(), 5000, &wc) == SIAARA_OK);
    {
        StoreOpen dest(dir / "dest_c");
        const std::string folder = (dir / "files_c").string();
        std::filesystem::create_directories(folder);
        siaara_web_retrieve_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.store = dest.store;
        ro.folder = folder.c_str();
        siaara_web_retrieve_report rep;
        std::memset(&rep, 0, sizeof rep);
        rep.struct_size = sizeof rep;
        CHECK(siaara_web_wado_retrieve_to_ex(wc, "2.25.63900", nullptr, nullptr, &ro, nullptr, nullptr, &rep) ==
              SIAARA_ERR_REJECTED);
        CHECK_EQ(rep.stored, 0u);
        CHECK_EQ(rep.uid_mismatch, 1u);
        CHECK(dest.instances() == 0);
        CHECK(std::filesystem::is_empty(folder));  // nothing left behind
        ro.allow_other_uids = 1;
        CHECK(siaara_web_wado_retrieve_to_ex(wc, "2.25.63900", nullptr, nullptr, &ro, nullptr, nullptr, &rep) == SIAARA_OK);
        CHECK_EQ(rep.stored, 1u);
    }
    siaara_web_client_free(wc);
    server.stop();
}

// ---------------------------------------------------------------- R-120
TEST_CASE("web/sec4 R-120 a client that does not read cannot hold a response open beyond the deadline") {
    web::http::ServerConfig cfg;
    cfg.limits.send_timeout_ms = 600;
    const std::string big(32u << 20, 'x');  // far more than the socket buffers take
    web::http::Server server;
    REQUIRE(server.start(cfg, [&](const web::http::Request&, web::http::Response& rs) {
                      rs.status = 200;
                      rs.set("Content-Type", "application/octet-stream");
                      rs.body = big;
                  })
                .is_ok());
    auto t = net::tcp_connect("127.0.0.1", server.port(), 3000);
    REQUIRE(t.is_ok());
    const std::string rq = "GET /x HTTP/1.1\r\nHost: localhost\r\n\r\n";
    REQUIRE((*t)->write_all(reinterpret_cast<const std::uint8_t*>(rq.data()), rq.size(), 3000).is_ok());
    // Read nothing for longer than the deadline: the server gives the response up and closes.
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    std::uint64_t got = 0;
    std::vector<std::uint8_t> buf(1 << 16);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        auto n = (*t)->read_some(buf.data(), buf.size(), 3000);
        if (!n || *n == 0) break;
        got += *n;
        if (ms_since(t0) > 20000) break;
    }
    CHECK(got < big.size());  // cut off: the old code would have delivered all of it
    (*t)->close();
    const auto t1 = std::chrono::steady_clock::now();
    server.stop();
    CHECK(ms_since(t1) < 5000);
    // A client that reads normally still gets a large response.
    web::http::Server server2;
    REQUIRE(server2.start(cfg, [&](const web::http::Request&, web::http::Response& rs) {
                       rs.status = 200;
                       rs.body = std::string(4u << 20, 'y');
                   })
                .is_ok());
    web::http::Client c("127.0.0.1", server2.port());
    web::http::Request get;
    get.method = "GET";
    get.target = "/y";
    auto rs = c.send(get);
    REQUIRE(rs.is_ok());
    CHECK_EQ(rs->body.size(), std::size_t{4u << 20});
    server2.stop();
}

// ---------------------------------------------------------------- T-P3-1
TEST_CASE("web/sec4 T-P3-1 QIDO-RS results carry the Retrieve URL of their level") {
    const auto dir = temp_dir("tp31");
    StoreOpen st(dir / "store");
    const auto bytes = part10(make_image("2.25.64001", "2.25.64900", "2.25.64901"));
    REQUIRE(siaara_store_add_memory(st.store, bytes.data(), bytes.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
    siaara_web_server_options o = server_options();
    siaara_web_server* ws = nullptr;
    REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
    uint32_t port = 0;
    REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    const auto get = [&](const std::string& path) -> Dataset {
        web::http::Request rq;
        rq.method = "GET";
        rq.target = "/dicomweb" + path;
        rq.headers.push_back({"Accept", "application/dicom+json"});
        web::http::Client c("127.0.0.1", static_cast<std::uint16_t>(port));
        auto rs = c.send(rq);
        if (!rs || rs->status != 200) return Dataset{};
        auto all = web::datasets_from_json(rs->body);
        if (!all || all->empty()) return Dataset{};
        return all->front();
    };
    CHECK(get("/studies").get_string(0x00081190).value_or("") == origin + "/dicomweb/studies/2.25.64900");
    CHECK(get("/studies/2.25.64900/series").get_string(0x00081190).value_or("") ==
          origin + "/dicomweb/studies/2.25.64900/series/2.25.64901");
    CHECK(get("/studies/2.25.64900/series/2.25.64901/instances").get_string(0x00081190).value_or("") ==
          origin + "/dicomweb/studies/2.25.64900/series/2.25.64901/instances/2.25.64001");
    siaara_web_server_free(ws);
    // A configured public base URL wins over the Host header.
    o.public_base_url = "https://pacs.example/dw";
    REQUIRE(siaara_web_server_start(&o, st.store, &ws) == SIAARA_OK);
    REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
    CHECK(get("/studies").get_string(0x00081190).value_or("") == "https://pacs.example/dw/studies/2.25.64900");
    siaara_web_server_free(ws);
}

// ---------------------------------------------------------------- JSON / XML: bytes outside the character set
TEST_CASE("web/sec4 JSON and XML write U+FFFD for bytes outside the declared character set, never a guess") {
    const std::string fffd = "\xEF\xBF\xBD";
    {  // No Specific Character Set (default repertoire) with a Latin-1 byte, as in old files.
        Dataset ds;
        Element e;
        e.tag = tags::PatientName;
        e.vr = VR::PN;
        const std::string raw = "M\xFCller^Hans ";  // 0xFC: not a character of ISO_IR 6
        e.value = Bytes::copy_of(raw.data(), raw.size());
        ds.put(std::move(e));
        std::string json;
        REQUIRE(web::dataset_to_json(ds, json).is_ok());
        CHECK(json.find("M" + fffd + "ller") != std::string::npos);
        CHECK(json.find("M\xC3\xBCller") == std::string::npos);  // not guessed as Latin-1
        std::string xml;
        REQUIRE(web::dataset_to_xml(ds, xml).is_ok());
        CHECK(xml.find("M" + fffd + "ller") != std::string::npos);
        // The raw bytes are still what the data set holds.
        CHECK(ds.get_raw_string(tags::PatientName).value_or("") == raw);
    }
    {  // ISO_IR 6 declared; bytes that happen to form UTF-8 are not UTF-8 text either.
        Dataset ds;
        (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 6");
        Element e;
        e.tag = 0x00081030;
        e.vr = VR::LO;
        const std::string raw = "caf\xC3\xA9";
        e.value = Bytes::copy_of(raw.data(), raw.size());
        ds.put(std::move(e));
        std::string json;
        REQUIRE(web::dataset_to_json(ds, json).is_ok());
        CHECK(json.find("caf" + fffd + fffd) != std::string::npos);
        web::ModelWriteOptions strict;
        strict.strict_charset = true;
        std::string out;
        CHECK(!web::dataset_to_json(ds, out, strict).is_ok());
    }
    {  // Declared character sets still decode: ISO_IR 100 and ISO_IR 192.
        Dataset ds;
        (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 100");
        Element e;
        e.tag = tags::PatientName;
        e.vr = VR::PN;
        const std::string raw = "M\xFCller";
        e.value = Bytes::copy_of(raw.data(), raw.size());
        ds.put(std::move(e));
        std::string json;
        REQUIRE(web::dataset_to_json(ds, json).is_ok());
        CHECK(json.find("M\xC3\xBCller") != std::string::npos);
    }
}
