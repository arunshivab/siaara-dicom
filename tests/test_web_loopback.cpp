// DICOMweb client <-> server over 127.0.0.1 (ephemeral port) with the test-only in-memory store:
// STOW-RS, QIDO-RS, WADO-RS (instances, metadata, frames, bulk data), WADO-URI, transfer syntax
// negotiation, failures and status codes, authorization, CORS, raw protocol abuse, concurrency,
// and the C API (DICOM JSON + DICOMweb client).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "codec/codec.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "crosscheck/web_memory_store.hpp"
#include "net/socket.hpp"
#include "siaara/siaara.h"
#include "web/client.hpp"
#include "web/dicom_json.hpp"
#include "web/server.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::web;

namespace {

constexpr const char* kCT = "1.2.840.10008.5.1.4.1.1.2";
constexpr const char* kStudyA = "2.25.100";
constexpr const char* kStudyB = "2.25.200";
constexpr const char* kSeries1 = "2.25.101";
constexpr const char* kSeries2 = "2.25.102";
constexpr const char* kRle = "1.2.840.10008.1.2.5";

std::vector<std::uint8_t> pixels(std::uint32_t frames, std::uint32_t seed) {
    std::vector<std::uint8_t> p(static_cast<std::size_t>(frames) * 32 * 32 * 2);
    for (std::size_t i = 0; i < p.size(); i += 2) {
        const auto v = static_cast<std::uint16_t>((i / 2 * 7 + seed * 131) % 4096);
        store_le16(p.data() + i, v);
    }
    return p;
}

DicomFile instance(const char* study, const char* series, const std::string& sop, const char* name, std::uint32_t frames,
                   bool rle, std::uint32_t seed) {
    DicomFile f;
    f.transfer_syntax = rle ? kRle : std::string(ts::ExplicitVRLittleEndian);
    Dataset& ds = f.dataset;
    (void)ds.set_string(tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
    (void)ds.set_string(tags::SOPClassUID, VR::UI, kCT);
    (void)ds.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)ds.set_string(0x00080020, VR::DA, "20240115");
    (void)ds.set_string(tags::Modality, VR::CS, rle ? "MR" : "CT");
    (void)ds.set_string(tags::PatientName, VR::PN, name);
    (void)ds.set_string(tags::PatientID, VR::LO, std::string("PID-") + study);
    (void)ds.set_string(tags::StudyInstanceUID, VR::UI, study);
    (void)ds.set_string(tags::SeriesInstanceUID, VR::UI, series);
    (void)ds.set_string(0x00200013, VR::IS, std::to_string(seed));
    (void)ds.set_uint16(tags::SamplesPerPixel, 1);
    (void)ds.set_string(tags::PhotometricInterpretation, VR::CS, "MONOCHROME2");
    (void)ds.set_string(tags::NumberOfFrames, VR::IS, std::to_string(frames));
    (void)ds.set_uint16(tags::Rows, 32);
    (void)ds.set_uint16(tags::Columns, 32);
    (void)ds.set_uint16(tags::BitsAllocated, 16);
    (void)ds.set_uint16(tags::BitsStored, 12);
    (void)ds.set_uint16(tags::HighBit, 11);
    (void)ds.set_uint16(tags::PixelRepresentation, 0);
    const auto px = pixels(frames, seed);
    Element e;
    e.tag = tags::PixelData;
    if (!rle) {
        e.vr = VR::OW;
        e.value = Bytes::copy_of(px.data(), px.size());
    } else {
        e.vr = VR::OB;
        e.undefined_length = true;
        e.fragments.push_back(Bytes());
        codec::FrameInfo fi;
        fi.rows = 32;
        fi.columns = 32;
        fi.bits_allocated = 16;
        fi.bits_stored = 12;
        const std::size_t fb = 32 * 32 * 2;
        for (std::uint32_t k = 0; k < frames; ++k) {
            std::vector<std::uint8_t> enc;
            (void)codec::encode_rle(std::span<const std::uint8_t>(px.data() + k * fb, fb), fi, {}, enc);
            e.fragments.push_back(Bytes::copy_of(enc.data(), enc.size()));
        }
    }
    ds.put(std::move(e));
    return f;
}

std::string part10(const DicomFile& f) {
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

struct Fixture {
    siaara_test_web::MemoryStore mem;
    DicomwebServer server;
    std::string base;
    explicit Fixture(DicomwebServerConfig cfg = {}) {
        cfg.http.port = 0;
        cfg.http.bind_address = "127.0.0.1";
        Status st = server.start(cfg, mem.make_store());
        CHECK(st.is_ok());
        base = "http://127.0.0.1:" + std::to_string(server.port()) + "/dicomweb";
    }
    DicomwebClient client() {
        DicomwebClientConfig c;
        c.base_url = base;
        c.limits.read_timeout_ms = 10000;
        return DicomwebClient(c);
    }
};

std::string raw_exchange(std::uint16_t port, const std::string& wire) {
    auto t = net::tcp_connect("127.0.0.1", port, 2000);
    if (!t) return std::string();
    (void)(*t)->write_all(reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size(), 2000);
    std::string out;
    std::uint8_t buf[4096];
    for (;;) {
        auto r = (*t)->read_some(buf, sizeof buf, 3000);
        if (!r || *r == 0) break;
        out.append(reinterpret_cast<const char*>(buf), *r);
    }
    return out;
}

// Stores the standard set: study A (series 1: two native instances, series 2: one RLE), study B.
void populate(DicomwebClient& c) {
    std::vector<std::string> a = {part10(instance(kStudyA, kSeries1, "2.25.1001", "Doe^Jane", 2, false, 1)),
                                  part10(instance(kStudyA, kSeries1, "2.25.1002", "Doe^Jane", 1, false, 2)),
                                  part10(instance(kStudyA, kSeries2, "2.25.1003", "Doe^Jane", 3, true, 3))};
    auto r = c.store(a);
    REQUIRE(r.is_ok());
    CHECK_EQ(r->http_status, 200);
    CHECK_EQ(r->stored.size(), 3u);
    CHECK_EQ(r->response.get_string(webtags::RetrieveURL).value_or("").find("/studies/2.25.100") != std::string::npos, true);
    auto r2 = c.store({part10(instance(kStudyB, "2.25.201", "2.25.2001", "Smith^John", 1, false, 4))}, kStudyB);
    REQUIRE(r2.is_ok());
    CHECK_EQ(r2->http_status, 200);
}

}  // namespace

TEST_CASE("web/loopback STOW-RS and QIDO-RS") {
    Fixture fx;
    auto c = fx.client();
    REQUIRE(c.status().is_ok());
    populate(c);
    CHECK_EQ(fx.mem.size(), 4u);
    auto all = c.search_studies();
    REQUIRE(all.is_ok());
    CHECK_EQ(all->size(), 2u);
    QidoParams q;
    q.match = {{"PatientName", "Doe*"}};
    auto doe = c.search_studies(q);
    REQUIRE(doe.is_ok());
    REQUIRE(doe->size() == 1);
    CHECK_EQ(doe->front().get_string(tags::StudyInstanceUID).value_or(""), std::string(kStudyA));
    CHECK_EQ(doe->front().get_string(0x00201208).value_or(""), std::string("3"));   // related instances
    CHECK_EQ(doe->front().get_string(0x00080061, 1).value_or(""), std::string("MR")); // modalities
    q.match = {{"0020000D", std::string(kStudyB) + "," + kStudyA}};
    auto both = c.search_studies(q);
    REQUIRE(both.is_ok());
    CHECK_EQ(both->size(), 2u);
    q = {};
    q.limit = 1;
    q.offset = 1;
    auto page = c.search_studies(q);
    REQUIRE(page.is_ok());
    CHECK_EQ(page->size(), 1u);
    q = {};
    q.match = {{"PatientName", "smith"}};
    q.fuzzy_matching = true;
    auto fuzzy = c.search_studies(q);
    REQUIRE(fuzzy.is_ok());
    CHECK_EQ(fuzzy->size(), 1u);
    q = {};
    q.match = {{"StudyDate", "20240101-20240131"}};
    auto range = c.search_studies(q);
    REQUIRE(range.is_ok());
    CHECK_EQ(range->size(), 2u);
    auto series = c.search_series({}, kStudyA);
    REQUIRE(series.is_ok());
    CHECK_EQ(series->size(), 2u);
    auto all_series = c.search_series();
    REQUIRE(all_series.is_ok());
    CHECK_EQ(all_series->size(), 3u);
    q = {};
    q.include_fields = {"PatientID", "00280010"};
    auto inst = c.search_instances(q, kStudyA, kSeries1);
    REQUIRE(inst.is_ok());
    REQUIRE(inst->size() == 2);
    CHECK(inst->front().contains(tags::PatientID));
    CHECK(inst->front().contains(tags::Rows));
    q = {};
    q.include_all = true;
    auto inst_all = c.search_instances(q, kStudyA);
    REQUIRE(inst_all.is_ok());
    CHECK_EQ(inst_all->size(), 3u);
    auto none = c.search_studies(QidoParams{{{"PatientID", "nobody"}}, {}, false, {}, {}, false});
    REQUIRE(none.is_ok());
    CHECK(none->empty());
    // Bad parameters: 400.
    auto bad = c.search_studies(QidoParams{{{"NoSuchKeyword", "x"}}, {}, false, {}, {}, false});
    CHECK(!bad.is_ok() && bad.error().code == Errc::Rejected && bad.error().message.find("400") != std::string::npos);
    auto bad_limit = c.request("GET", "/studies?limit=-1");
    REQUIRE(bad_limit.is_ok());
    CHECK_EQ(bad_limit->status, 400);
    CHECK(bad_limit->body.find("-1") == std::string::npos);  // input never echoed
    // XML results.
    auto xmlr = c.request("GET", "/studies", {{"Accept", "multipart/related; type=\"application/dicom+xml\""}});
    REQUIRE(xmlr.is_ok());
    CHECK_EQ(xmlr->status, 200);
    CHECK(xmlr->body.find("<NativeDicomModel") != std::string::npos);
    auto unacceptable = c.request("GET", "/studies", {{"Accept", "text/html"}});
    REQUIRE(unacceptable.is_ok());
    CHECK_EQ(unacceptable->status, 406);
}

TEST_CASE("web/loopback WADO-RS instances, metadata, frames and bulk data") {
    Fixture fx;
    auto c = fx.client();
    populate(c);
    auto study = c.retrieve(kStudyA);
    if (!study) std::fprintf(stderr, "  %s\n", study.error().message.c_str());
    REQUIRE(study.is_ok());
    CHECK_EQ(study->size(), 3u);
    // Default transfer syntax is Explicit VR Little Endian: the RLE instance comes decompressed.
    for (const auto& f : *study) CHECK_EQ(f.transfer_syntax, std::string(ts::ExplicitVRLittleEndian));
    for (const auto& f : *study) {
        if (f.dataset.get_string(tags::SOPInstanceUID).value_or("") != "2.25.1003") continue;
        const Element* px = f.dataset.find(tags::PixelData);
        REQUIRE(px && !px->is_encapsulated());
        const auto want = pixels(3, 3);
        CHECK(px->value.size() == want.size() && std::memcmp(px->value.data(), want.data(), want.size()) == 0);
    }
    auto stored = c.retrieve(kStudyA, kSeries2, "2.25.1003", {"*"});
    REQUIRE(stored.is_ok());
    REQUIRE(stored->size() == 1);
    CHECK_EQ(stored->front().transfer_syntax, std::string(kRle));
    auto implicit = c.retrieve(kStudyA, kSeries1, "2.25.1001", {"1.2.840.10008.1.2"});
    REQUIRE(implicit.is_ok());
    CHECK_EQ(implicit->front().transfer_syntax, std::string("1.2.840.10008.1.2"));
    // Preference list: unavailable JPEG-LS first, then big endian.
    auto pref = c.retrieve(kStudyA, kSeries1, "2.25.1002", {"1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.2"});
    REQUIRE(pref.is_ok());
    CHECK_EQ(pref->front().transfer_syntax, std::string("1.2.840.10008.1.2.2"));
    auto only_jls = c.retrieve(kStudyA, kSeries1, "2.25.1002", {"1.2.840.10008.1.2.4.80"});
    CHECK(!only_jls.is_ok());
    auto series = c.retrieve(kStudyA, kSeries1);
    REQUIRE(series.is_ok());
    CHECK_EQ(series->size(), 2u);
    CHECK(c.retrieve("2.25.999").error().code == Errc::NotFound);
    // Metadata with BulkDataURI for pixel data.
    auto meta = c.retrieve_metadata(kStudyA);
    REQUIRE(meta.is_ok());
    CHECK_EQ(meta->size(), 3u);
    auto m1 = c.request("GET", std::string("/studies/") + kStudyA + "/series/" + kSeries1 + "/instances/2.25.1001/metadata");
    REQUIRE(m1.is_ok());
    CHECK_EQ(m1->status, 200);
    auto parsed = json::parse(m1->body);
    REQUIRE(parsed.is_ok() && parsed->is_array() && parsed->array.size() == 1);
    const json::Value* pxj = parsed->array[0].get("7FE00010");
    REQUIRE(pxj && pxj->get("BulkDataURI"));
    const std::string uri = pxj->get("BulkDataURI")->text;
    CHECK(uri.find("/instances/2.25.1001/bulkdata/7FE00010") != std::string::npos);
    auto bulk = c.retrieve_bulkdata(uri);
    REQUIRE(bulk.is_ok());
    REQUIRE(bulk->size() == 1);
    CHECK(bulk->front().data == std::string(reinterpret_cast<const char*>(pixels(2, 1).data()), pixels(2, 1).size()));
    auto bulk_rle = c.retrieve_bulkdata(std::string("/dicomweb/studies/") + kStudyA + "/series/" + kSeries2 +
                                        "/instances/2.25.1003/bulkdata/7FE00010");
    REQUIRE(bulk_rle.is_ok());
    CHECK_EQ(bulk_rle->size(), 3u);
    CHECK(bulk_rle->front().content_type.find("image/dicom-rle") == 0);
    auto missing_bulk = c.retrieve_bulkdata(std::string("/dicomweb/studies/") + kStudyA + "/series/" + kSeries1 +
                                            "/instances/2.25.1001/bulkdata/00420011");
    CHECK(missing_bulk.error().code == Errc::NotFound);
    // Metadata as XML.
    auto mx = c.request("GET", std::string("/studies/") + kStudyA + "/metadata",
                        {{"Accept", "multipart/related; type=\"application/dicom+xml\""}});
    REQUIRE(mx.is_ok());
    CHECK_EQ(mx->status, 200);
    CHECK(mx->body.find("<BulkData uri=") != std::string::npos);
    // Frames: native.
    const auto px1 = pixels(2, 1);
    auto fr = c.retrieve_frames(kStudyA, kSeries1, "2.25.1001", {2, 1});
    REQUIRE(fr.is_ok());
    REQUIRE(fr->size() == 2);
    CHECK(fr->at(0).data == std::string(reinterpret_cast<const char*>(px1.data()) + 2048, 2048));
    CHECK(fr->at(1).data == std::string(reinterpret_cast<const char*>(px1.data()), 2048));
    CHECK(fr->at(0).content_type.find("application/octet-stream") == 0);
    // Frames of the RLE instance: decompressed by default, as stored with "*" or the RLE syntax.
    const auto px3 = pixels(3, 3);
    auto frd = c.retrieve_frames(kStudyA, kSeries2, "2.25.1003", {3});
    REQUIRE(frd.is_ok());
    CHECK(frd->at(0).data == std::string(reinterpret_cast<const char*>(px3.data()) + 4096, 2048));
    for (const char* t : {"*", kRle}) {
        auto fre = c.retrieve_frames(kStudyA, kSeries2, "2.25.1003", {1}, {t});
        REQUIRE(fre.is_ok());
        REQUIRE(fre->size() == 1);
        CHECK(fre->at(0).content_type.find("image/dicom-rle") == 0);
        CHECK(fre->at(0).data.size() != 2048);
    }
    CHECK(!c.retrieve_frames(kStudyA, kSeries1, "2.25.1001", {3}).is_ok());       // out of range: 404
    auto zero = c.request("GET", std::string("/studies/") + kStudyA + "/series/" + kSeries1 + "/instances/2.25.1001/frames/0");
    REQUIRE(zero.is_ok());
    CHECK_EQ(zero->status, 400);
    auto jls = c.retrieve_frames(kStudyA, kSeries1, "2.25.1001", {1}, {"1.2.840.10008.1.2.4.80"});
    CHECK(!jls.is_ok() && jls.error().message.find("406") != std::string::npos);
    // Rendered is not offered.
    auto rendered = c.retrieve_rendered(kStudyA, kSeries1, "2.25.1001", "image/jpeg");
    CHECK(!rendered.is_ok() && rendered.error().message.find("406") != std::string::npos);
}

TEST_CASE("web/loopback WADO-URI") {
    Fixture fx;
    auto c = fx.client();
    populate(c);
    auto body = c.wado_uri(kStudyA, kSeries1, "2.25.1001");
    REQUIRE(body.is_ok());
    auto f = read_part10(Bytes::from_string(*body));
    REQUIRE(f.is_ok());
    CHECK_EQ(f->dataset.get_string(tags::SOPInstanceUID).value_or(""), std::string("2.25.1001"));
    auto implicit = c.wado_uri(kStudyA, kSeries2, "2.25.1003", "application/dicom", "1.2.840.10008.1.2");
    REQUIRE(implicit.is_ok());
    auto fi = read_part10(Bytes::from_string(*implicit));
    REQUIRE(fi.is_ok());
    CHECK_EQ(fi->transfer_syntax, std::string("1.2.840.10008.1.2"));
    CHECK(!c.wado_uri(kStudyA, kSeries1, "2.25.1001", "image/jpeg").is_ok());   // 406: no renderer
    CHECK(c.wado_uri(kStudyA, kSeries1, "2.25.9").error().code == Errc::NotFound);
    auto no_type = c.request("GET", "/wado?requestType=WADO");
    REQUIRE(no_type.is_ok());
    CHECK_EQ(no_type->status, 400);  // dot segment refused
    auto missing = raw_exchange(fx.server.port(), "GET /dicomweb/wado?requestType=WADO&studyUID=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(missing.rfind("HTTP/1.1 400", 0) == 0);
}

TEST_CASE("web/loopback STOW-RS failures and status codes") {
    Fixture fx;
    auto c = fx.client();
    const std::string good = part10(instance(kStudyA, kSeries1, "2.25.5001", "A^B", 1, false, 9));
    // One bad part among good ones: 202 with a Failed SOP Sequence.
    auto partial = c.store({good, "not a DICOM file"});
    REQUIRE(partial.is_ok());
    CHECK_EQ(partial->http_status, 202);
    CHECK_EQ(partial->stored.size(), 1u);
    REQUIRE(partial->failed.size() == 1);
    CHECK_EQ(partial->failed[0].second, failure::CannotUnderstand);
    // Wrong study in the path: every instance fails, 409.
    auto conflict = c.store({good}, kStudyB);
    REQUIRE(conflict.is_ok());
    CHECK_EQ(conflict->http_status, 409);
    REQUIRE(conflict->failed.size() == 1);
    CHECK_EQ(conflict->failed[0].first, std::string("2.25.5001"));
    // Wrong media types.
    auto r415 = c.request("POST", "/studies", {{"Content-Type", "application/dicom"}}, good);
    REQUIRE(r415.is_ok());
    CHECK_EQ(r415->status, 415);
    auto r415b = c.request("POST", "/studies", {{"Content-Type", "multipart/related; type=\"application/dicom+json\"; boundary=x"}}, "--x--");
    REQUIRE(r415b.is_ok());
    CHECK_EQ(r415b->status, 415);
    auto r400 = c.request("POST", "/studies", {{"Content-Type", "multipart/related; type=\"application/dicom\"; boundary=x"}}, "garbage");
    REQUIRE(r400.is_ok());
    CHECK_EQ(r400->status, 400);
    auto empty = c.request("POST", "/studies", {{"Content-Type", "multipart/related; type=\"application/dicom\"; boundary=x"}}, "--x--\r\n");
    REQUIRE(empty.is_ok());
    CHECK_EQ(empty->status, 400);
    // Routing errors.
    auto r405 = c.request("DELETE", "/studies");
    REQUIRE(r405.is_ok());
    CHECK_EQ(r405->status, 405);
    CHECK(http::find_header(r405->headers, "Allow") != nullptr);
    auto r404 = c.request("GET", "/nothing");
    REQUIRE(r404.is_ok());
    CHECK_EQ(r404->status, 404);
    auto bad_uid = c.request("GET", "/studies/1.2.x");
    REQUIRE(bad_uid.is_ok());
    CHECK_EQ(bad_uid->status, 400);
    CHECK(bad_uid->body.find("1.2.x") == std::string::npos);
    auto outside = c.request("GET", "/../other");
    REQUIRE(outside.is_ok());
    CHECK_EQ(outside->status, 400);
    // CORS is off by default.
    auto plain = c.request("GET", "/studies");
    REQUIRE(plain.is_ok());
    CHECK(http::find_header(plain->headers, "Access-Control-Allow-Origin") == nullptr);
    auto options = c.request("OPTIONS", "/studies");
    REQUIRE(options.is_ok());
    CHECK_EQ(options->status, 405);
}

TEST_CASE("web/loopback authorization and CORS") {
    DicomwebServerConfig cfg;
    cfg.authorize = [](const http::Request& rq) {
        const std::string* a = http::find_header(rq.headers, "Authorization");
        return a && (*a == bearer_auth("secret") || *a == basic_auth("user", "pw"));
    };
    cfg.cors_allow_origin = "https://viewer.example";
    Fixture fx(cfg);
    auto anon = fx.client();
    auto denied = anon.search_studies();
    CHECK(!denied.is_ok() && denied.error().message.find("401") != std::string::npos);
    DicomwebClientConfig cc;
    cc.base_url = fx.base;
    cc.authorization = [] { return bearer_auth("secret"); };
    DicomwebClient c(cc);
    auto ok = c.search_studies();
    CHECK(ok.is_ok());
    cc.authorization = [] { return basic_auth("user", "pw"); };
    DicomwebClient b(cc);
    CHECK(b.search_studies().is_ok());
    CHECK_EQ(basic_auth("user", "pw"), std::string("Basic dXNlcjpwdw=="));
    auto r = c.request("GET", "/studies");
    REQUIRE(r.is_ok());
    CHECK(http::find_header(r->headers, "Access-Control-Allow-Origin") &&
          *http::find_header(r->headers, "Access-Control-Allow-Origin") == "https://viewer.example");
    auto pre = anon.request("OPTIONS", "/studies");
    REQUIRE(pre.is_ok());
    CHECK_EQ(pre->status, 204);
}

TEST_CASE("web/loopback raw protocol abuse, keep-alive, concurrency and shutdown") {
    DicomwebServerConfig cfg;
    cfg.http.limits.max_header_bytes = 2048;
    cfg.http.limits.max_body = 1 << 20;
    cfg.http.limits.read_timeout_ms = 2000;
    cfg.http.limits.header_timeout_ms = 2000;
    cfg.http.idle_timeout_ms = 2000;
    Fixture fx(cfg);
    const auto port = fx.server.port();
    CHECK(raw_exchange(port, "garbage\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
    CHECK(raw_exchange(port, "GET / HTTP/1.1\r\nHost: h\r\nX: " + std::string(4000, 'a') + "\r\n\r\n").rfind("HTTP/1.1 431", 0) == 0);
    CHECK(raw_exchange(port, "POST /dicomweb/studies HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999\r\n\r\n").rfind("HTTP/1.1 413", 0) == 0);
    CHECK(raw_exchange(port, "GET / HTTP/3.0\r\nHost: h\r\n\r\n").rfind("HTTP/1.1 505", 0) == 0);
    CHECK(raw_exchange(port, "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03").rfind("HTTP/1.1 400", 0) == 0);  // TLS hello
    // Pipelined requests on one connection are answered in order.
    const std::string two = raw_exchange(port, "GET /dicomweb/studies HTTP/1.1\r\nHost: h\r\n\r\nGET /dicomweb/nothing HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    const auto first = two.find("HTTP/1.1 200");
    const auto second = two.find("HTTP/1.1 404");
    CHECK(first == 0 && second != std::string::npos && second > first);
    // Keep-alive: many requests over the client's persistent connection, then concurrent clients.
    auto c = fx.client();
    populate(c);
    for (int i = 0; i < 50; ++i) CHECK(c.search_studies().is_ok());
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            auto cc = fx.client();
            for (int i = 0; i < 20; ++i) {
                auto r = cc.search_instances({}, kStudyA);
                if (!r || r->size() != 3) ++failures;
            }
        });
    for (auto& t : threads) t.join();
    CHECK_EQ(failures.load(), 0);
    // An idle connection does not keep stop() waiting.
    auto idle = net::tcp_connect("127.0.0.1", port, 2000);
    REQUIRE(idle.is_ok());
    const auto t0 = std::chrono::steady_clock::now();
    fx.server.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1500);
    CHECK(!c.search_studies().is_ok());
}

TEST_CASE("web/loopback C API") {
    // DICOM JSON round trip through the C API.
    siaara_file* f = nullptr;
    REQUIRE(siaara_file_create(&f) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(f, &ds) == SIAARA_OK);
    CHECK(siaara_dataset_set_string(ds, 0x00100010u, "PN", "Doe^Jane", 8) == SIAARA_OK);
    CHECK(siaara_dataset_set_string(ds, 0x00080016u, "UI", kCT, std::strlen(kCT)) == SIAARA_OK);
    CHECK(siaara_dataset_set_string(ds, 0x00080018u, "UI", "2.25.7001", 9) == SIAARA_OK);
    CHECK(siaara_dataset_set_string(ds, 0x0020000Du, "UI", kStudyA, std::strlen(kStudyA)) == SIAARA_OK);
    CHECK(siaara_dataset_set_string(ds, 0x0020000Eu, "UI", kSeries1, std::strlen(kSeries1)) == SIAARA_OK);
    CHECK(siaara_dataset_set_string(ds, 0x00281053u, "DS", "1.5", 3) == SIAARA_OK);
    size_t needed = 0;
    CHECK(siaara_dataset_to_json(ds, 0, nullptr, 0, &needed) == SIAARA_OK);
    std::vector<char> buf(needed + 1);
    char tiny[4];
    CHECK(siaara_dataset_to_json(ds, 0, tiny, sizeof tiny, &needed) == SIAARA_ERR_BUFFER_TOO_SMALL);
    REQUIRE(siaara_dataset_to_json(ds, 0, buf.data(), buf.size(), &needed) == SIAARA_OK);
    const std::string js(buf.data(), needed);
    CHECK(js.find("\"00281053\":{\"vr\":\"DS\",\"Value\":[1.5]}") != std::string::npos);
    buf.resize(buf.size() + 16);
    CHECK(siaara_dataset_to_json(ds, SIAARA_JSON_NUMBERS_AS_STRINGS, buf.data(), buf.size(), &needed) == SIAARA_OK);
    CHECK(std::string(buf.data()).find("[\"1.5\"]") != std::string::npos);
    CHECK(siaara_dataset_to_json(ds, 0x80, buf.data(), buf.size(), &needed) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_file* g = nullptr;
    REQUIRE(siaara_dataset_from_json(js.data(), js.size(), &g) == SIAARA_OK);
    siaara_dataset* gds = nullptr;
    REQUIRE(siaara_file_dataset(g, &gds) == SIAARA_OK);
    char name[32];
    CHECK(siaara_dataset_get_string(gds, 0x00100010u, 0, name, sizeof name, &needed) == SIAARA_OK);
    CHECK_EQ(std::string(name), std::string("Doe^Jane"));
    siaara_file* bad = nullptr;
    CHECK(siaara_dataset_from_json("{\"x\":1}", 7, &bad) == SIAARA_ERR_MALFORMED);
    CHECK(bad == nullptr);
    // DICOMweb client against the loopback server.
    Fixture fx;
    siaara_web_client* wc = nullptr;
    CHECK(siaara_web_client_create("ftp://x/", 8, 0, &wc) == SIAARA_ERR_UNSUPPORTED);  // https:// is supported now
    REQUIRE(siaara_web_client_create(fx.base.data(), fx.base.size(), 5000, &wc) == SIAARA_OK);
    CHECK(siaara_web_client_set_authorization(wc, "Bearer a\r\nX: y", 14) == SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_web_client_set_authorization(wc, "Bearer abc", 10) == SIAARA_OK);
    CHECK(siaara_web_stow(wc, g, nullptr) == SIAARA_OK);
    CHECK(siaara_web_stow(wc, g, kStudyB) == SIAARA_ERR_REJECTED);
    char small[8];
    CHECK(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, nullptr, nullptr, "PatientName=Doe%5E%2A", 21, small, sizeof small, &needed) ==
          SIAARA_ERR_BUFFER_TOO_SMALL);
    std::vector<char> res(needed + 1);
    REQUIRE(siaara_web_last_response(wc, res.data(), res.size(), &needed) == SIAARA_OK);
    auto studies = datasets_from_json(std::string_view(res.data(), needed));
    REQUIRE(studies.is_ok());
    REQUIRE(studies->size() == 1);
    CHECK(siaara_web_qido(wc, SIAARA_QIDO_INSTANCES, kStudyA, kSeries1, nullptr, 0, res.data(), res.size(), &needed) == SIAARA_OK);
    CHECK(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, kStudyA, nullptr, nullptr, 0, res.data(), res.size(), &needed) ==
          SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, nullptr, nullptr, "a b", 3, res.data(), res.size(), &needed) ==
          SIAARA_ERR_INVALID_ARGUMENT);
    CHECK(siaara_web_qido(wc, SIAARA_QIDO_STUDIES, nullptr, nullptr, "Foo=1", 5, res.data(), res.size(), &needed) ==
          SIAARA_ERR_REJECTED);
    siaara_file* got = nullptr;
    REQUIRE(siaara_web_wado_instance(wc, kStudyA, kSeries1, "2.25.7001", &got) == SIAARA_OK);
    siaara_dataset* got_ds = nullptr;
    REQUIRE(siaara_file_dataset(got, &got_ds) == SIAARA_OK);
    CHECK(siaara_dataset_get_string(got_ds, 0x00100010u, 0, name, sizeof name, &needed) == SIAARA_OK);
    CHECK_EQ(std::string(name), std::string("Doe^Jane"));
    siaara_file* none = nullptr;
    CHECK(siaara_web_wado_instance(wc, kStudyA, kSeries1, "2.25.9", &none) == SIAARA_ERR_NOT_FOUND);
    CHECK(siaara_web_wado_instance(wc, kStudyA, nullptr, "2.25.9", &none) == SIAARA_ERR_INVALID_ARGUMENT);
    siaara_file_free(got);
    siaara_web_client_free(wc);
    siaara_web_client_free(nullptr);
    siaara_file_free(g);
    siaara_file_free(f);
}
