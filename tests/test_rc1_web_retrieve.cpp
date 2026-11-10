// Release-candidate review, DICOMweb server retrieves: R-161 (a retrieve that has to decode is
// sized from the decoded sizes and answered 413 before anything is sent; the default transfer
// syntax is an option; decode memory is a server-wide budget), R-168 (no whole-message deadline
// for streamed replies unless configured), R-182 (too many instances under a key is 413).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "codec/transcode.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"
#include "web/http.hpp"
#include "web/multipart.hpp"
#include "web/server.hpp"

using namespace siaara;

namespace {

constexpr const char* kRle = "1.2.840.10008.1.2.5";
constexpr const char* kStudy = "2.25.161900";
constexpr std::uint64_t kDecodedPixels = 1024ull * 1024ull * 2ull * 4ull;  // 8 MiB once decoded

std::filesystem::path fresh_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_rc1web_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// 4 frames of 1024 x 1024, 16 bit, all zero: 8 MiB of pixel data that RLE stores in a few kB.
std::vector<std::uint8_t> rle_instance(const std::string& sop) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.4.1");
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, kStudy);
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.161901");
    (void)ds.set_string(tags::Modality, VR::CS, "MR");
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_string(tags::NumberOfFrames, VR::IS, "4");
    (void)ds.set_uint16(tags::Rows, 1024);
    (void)ds.set_uint16(tags::Columns, 1024);
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    (void)ds.set_uint16(tags::BitsStored, 12);
    (void)ds.set_uint16(tags::HighBit, 11);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    const std::vector<std::uint8_t> px(static_cast<std::size_t>(kDecodedPixels), 0);
    (void)ds.set_bytes(tags::PixelData, VR::OW, Bytes::copy_of(px.data(), px.size()));
    (void)codec::transcode_file(f, kRle);
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return out;
}

struct Reply {
    int status = 0;
    std::size_t parts = 0;
    std::string first_ts;       // transfer-syntax parameter of the first part
    std::uint64_t body_bytes = 0;
    bool complete = false;      // the multipart body parsed to its closing boundary
};

Reply get(std::uint16_t port, const std::string& target, const std::string& accept) {
    Reply out;
    web::http::Request rq;
    rq.method = "GET";
    rq.target = target;
    rq.headers.push_back({"Accept", accept});
    web::http::Limits lim;
    lim.max_body = std::uint64_t{512} << 20;
    web::http::Client c("127.0.0.1", port);
    c.set_limits(lim);
    auto rs = c.send(rq);
    if (!rs) {
        out.status = -1;  // the connection was cut
        return out;
    }
    out.status = rs->status;
    out.body_bytes = rs->body.size();
    if (rs->status != 200) return out;
    const std::string* ct = web::http::find_header(rs->headers, "Content-Type");
    if (!ct) return out;
    auto mt = web::http::parse_media_type(*ct);
    if (!mt) return out;
    const std::string* boundary = mt->param("boundary");
    if (!boundary) return out;
    auto parts = web::multipart::parse(rs->body, *boundary, web::multipart::Limits{});
    if (!parts) return out;
    out.complete = true;
    out.parts = parts->size();
    if (!parts->empty())
        if (const std::string* pct = parts.value()[0].content_type())
            if (auto pm = web::http::parse_media_type(*pct))
                if (const std::string* t = pm->param("transfer-syntax")) out.first_ts = *t;
    return out;
}

const std::string kAnyDicom = "multipart/related; type=\"application/dicom\"";
const std::string kAsStored = "multipart/related; type=\"application/dicom\"; transfer-syntax=*";

siaara_store* store_with(const std::filesystem::path& root, int instances) {
    siaara_store* s = nullptr;
    const std::string r = root.string();
    if (siaara_store_open(r.c_str(), r.size(), nullptr, &s) != SIAARA_OK) return nullptr;
    for (int i = 0; i < instances; ++i) {
        const std::vector<std::uint8_t> b = rle_instance("2.25.16100" + std::to_string(i));
        if (siaara_store_add_memory(s, b.data(), b.size(), SIAARA_DUP_KEEP, nullptr) != SIAARA_OK) {
            siaara_store_free(s);
            return nullptr;
        }
    }
    return s;
}

struct CServer {
    siaara_web_server* ws = nullptr;
    std::uint16_t port = 0;
    CServer(siaara_store* store, const siaara_web_server_options& o) {
        if (siaara_web_server_start(&o, store, &ws) != SIAARA_OK) return;
        uint32_t p = 0;
        (void)siaara_web_server_port(ws, &p);
        port = static_cast<std::uint16_t>(p);
    }
    ~CServer() { siaara_web_server_free(ws); }
};

siaara_web_server_options options() {
    siaara_web_server_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    return o;
}

}  // namespace

TEST_CASE("web/ rc1 R-161 a retrieve that must decode is sized from the decoded sizes: 413 up front, never a cut 200") {
    const auto dir = fresh_dir("r161");
    siaara_store* store = store_with(dir / "store", 3);  // a few kB stored each, 8 MiB decoded each
    REQUIRE(store != nullptr);
    const std::string target = std::string("/dicomweb/studies/") + kStudy;
    {
        // The cap holds the stored study many times over, and two decoded instances but not three.
        siaara_web_server_options o = options();
        o.max_response_bytes = 20u << 20;
        CServer s(store, o);
        REQUIRE(s.ws != nullptr);
        // No transfer syntax in Accept: Explicit VR Little Endian, i.e. 3 x 8 MiB decoded > 20 MiB.
        const Reply dflt = get(s.port, target, kAnyDicom);
        CHECK_EQ(dflt.status, 413);  // on the old code: 200, then the connection was cut mid-stream
        // As stored: the same study is a few kB and is sent.
        const Reply stored = get(s.port, target, kAsStored);
        CHECK_EQ(stored.status, 200);
        CHECK(stored.complete);
        CHECK_EQ(stored.parts, 3u);
        CHECK(stored.first_ts == kRle);
        CHECK(stored.body_bytes < (1u << 20));
        // One instance decoded fits.
        const Reply one = get(s.port, target + "/series/2.25.161901/instances/2.25.161000", kAnyDicom);
        CHECK_EQ(one.status, 200);
        CHECK(one.complete);
        CHECK(one.first_ts == ts::ExplicitVRLittleEndian);
        CHECK(one.body_bytes > kDecodedPixels);
    }
    {
        // Option: default_transfer_syntax AS_STORED - nothing is decoded for a plain retrieve.
        siaara_web_server_options o = options();
        o.max_response_bytes = 20u << 20;
        o.default_transfer_syntax = SIAARA_WEB_DEFAULT_TS_AS_STORED;
        CServer s(store, o);
        REQUIRE(s.ws != nullptr);
        const Reply r = get(s.port, target, kAnyDicom);
        CHECK_EQ(r.status, 200);
        CHECK(r.complete);
        CHECK_EQ(r.parts, 3u);
        CHECK(r.first_ts == kRle);
        // Explicit VR Little Endian asked for by name is still decoded - and still sized first.
        const Reply le = get(s.port, target, kAnyDicom + "; transfer-syntax=1.2.840.10008.1.2.1");
        CHECK_EQ(le.status, 413);
        siaara_web_server_options bad = options();
        bad.default_transfer_syntax = 7;
        siaara_web_server* ws = nullptr;
        CHECK(siaara_web_server_start(&bad, store, &ws) == SIAARA_ERR_INVALID_ARGUMENT);
    }
    {
        // The decode memory budget: an instance that cannot be decoded within it is 413 up front;
        // with room for one at a time, two clients are served one after the other, both whole.
        siaara_web_server_options o = options();
        o.max_decode_memory_bytes = 4u << 20;  // one instance is charged 2 x 8 MiB
        CServer small(store, o);
        REQUIRE(small.ws != nullptr);
        CHECK_EQ(get(small.port, target + "/series/2.25.161901/instances/2.25.161000", kAnyDicom).status, 413);
        CHECK_EQ(get(small.port, target, kAsStored).status, 200);  // nothing to decode: not affected
        o.max_decode_memory_bytes = 20u << 20;  // room for one instance (16 MiB + a little) at a time
        CServer one_at_a_time(store, o);
        REQUIRE(one_at_a_time.ws != nullptr);
        Reply a, b;
        std::thread ta([&] { a = get(one_at_a_time.port, target, kAnyDicom); });
        std::thread tb([&] { b = get(one_at_a_time.port, target, kAnyDicom); });
        ta.join();
        tb.join();
        CHECK_EQ(a.status, 200);
        CHECK_EQ(b.status, 200);
        CHECK(a.complete && a.parts == 3);
        CHECK(b.complete && b.parts == 3);
    }
    siaara_store_free(store);
    {
        // A store that does not know decoded sizes (no index values): the instance headers are read.
        const std::vector<std::uint8_t> bytes = rle_instance("2.25.161050");
        const std::string path = (dir / "one.dcm").string();
        REQUIRE(io::write_file_atomic(path, bytes.data(), bytes.size()));
        std::atomic<int> opened{0};
        web::Store custom;
        custom.locate = [&](const web::InstanceKey&, std::vector<web::StoredInstance>& out) -> Status {
            web::StoredInstance inst;
            inst.path = path;
            inst.size = bytes.size();
            inst.transfer_syntax = kRle;
            out.assign(3, inst);
            return Status::ok();
        };
        custom.open = [&](const web::StoredInstance& inst) -> Result<Bytes> {
            opened.fetch_add(1);
            return io::map_file(inst.path, false);
        };
        web::DicomwebServerConfig cfg;
        cfg.max_response_bytes = 20u << 20;
        CHECK(cfg.default_transfer_syntax == web::kDefaultWadoTransferSyntax);
        CHECK_EQ(cfg.max_decode_memory, web::kDefaultWebDecodeMemoryBytes);
        web::DicomwebServer server;
        REQUIRE(server.start(cfg, custom));
        CHECK_EQ(get(server.port(), target, kAnyDicom).status, 413);
        CHECK_EQ(get(server.port(), target, kAsStored).status, 200);
        server.stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("web/ rc1 R-168 a streamed reply to a healthy client is not cut by the whole-message deadline; R-182 413") {
    const auto dir = fresh_dir("r168");
    const std::vector<std::uint8_t> bytes = rle_instance("2.25.168001");
    const std::string path = (dir / "one.dcm").string();
    REQUIRE(io::write_file_atomic(path, bytes.data(), bytes.size()));
    std::atomic<bool> too_many{false};
    web::Store store;
    store.locate = [&](const web::InstanceKey&, std::vector<web::StoredInstance>& out) -> Status {
        if (too_many.load()) return Status(Errc::LimitExceeded, "too many instances");
        web::StoredInstance inst;
        inst.path = path;
        inst.size = bytes.size();
        inst.transfer_syntax = kRle;
        out.assign(6, inst);
        return Status::ok();
    };
    // A store that needs 150 ms per instance: the reply takes about 0.9 s to produce.
    store.open = [&](const web::StoredInstance& inst) -> Result<Bytes> {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return io::map_file(inst.path, false);
    };
    const std::string target = std::string("/dicomweb/studies/") + kStudy;
    web::DicomwebServerConfig cfg;
    cfg.http.limits.send_timeout_ms = 300;  // the deadline of responses that are not streamed
    CHECK_EQ(cfg.http.limits.stream_send_timeout_ms, web::http::kDefaultStreamSendTimeoutMs);
    CHECK_EQ(web::http::kDefaultStreamSendTimeoutMs, 0);
    {
        web::DicomwebServer server;
        REQUIRE(server.start(cfg, store));
        const auto t0 = std::chrono::steady_clock::now();
        const Reply r = get(server.port(), target, kAsStored);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 600);              // it did outlast send_timeout_ms
        CHECK_EQ(r.status, 200);       // on the old code the reply was cut after 300 ms
        CHECK(r.complete);
        CHECK_EQ(r.parts, 6u);
        // R-182: more instances under the key than a reply may hold.
        too_many.store(true);
        CHECK_EQ(get(server.port(), target, kAsStored).status, 413);
        too_many.store(false);
        server.stop();
    }
    {
        // The option: an overall deadline for streamed replies too.
        cfg.http.limits.stream_send_timeout_ms = 300;
        web::DicomwebServer server;
        REQUIRE(server.start(cfg, store));
        const Reply r = get(server.port(), target, kAsStored);
        CHECK(!(r.status == 200 && r.complete));
        server.stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
