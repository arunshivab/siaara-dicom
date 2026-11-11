// Release-candidate review, R-163: the overlong-length recovery of local file reads ("GE Genesis"
// lengths) is off for everything that arrives over the network - also in the WADO client, in
// siaara_web_parts_open_file, and in the store's own indexing and re-read of received bytes.
// A received data set of that kind is incomplete and goes to the receive policy; a local open of
// the same bytes is still recovered (and flagged).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "capi/capi_internal.hpp"
#include "core/incomplete_policy.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/dimse.hpp"
#include "net/scu.hpp"
#include "siaara/siaara.h"
#include "store/index.hpp"
#include "store/store.hpp"
#include "test.hpp"
#include "web/client.hpp"
#include "web/http.hpp"
#include "web/multipart.hpp"
#include "web/server.hpp"

using namespace siaara;

namespace {

constexpr const char* kMR = "1.2.840.10008.5.1.4.1.1.4";
constexpr const char* kSop = "2.25.163001";
constexpr const char* kStudy = "2.25.163900";
constexpr const char* kSeries = "2.25.163901";
constexpr const char* kRecovered = "AX T1 POST";  // what the recovery makes of (0008,103E)

std::filesystem::path fresh_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_rc1net_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

void u16(std::vector<std::uint8_t>& b, std::uint32_t x) {
    b.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
}
// Implicit VR little endian element; `stated` overrides the length written.
void imp(std::vector<std::uint8_t>& b, std::uint16_t g, std::uint16_t e, std::string_view v, std::uint32_t stated = 0xFFFFFFFFu) {
    u16(b, g);
    u16(b, e);
    const std::uint32_t n = stated == 0xFFFFFFFFu ? static_cast<std::uint32_t>(v.size()) : stated;
    u16(b, n & 0xFFFFu);
    u16(b, n >> 16);
    b.insert(b.end(), v.begin(), v.end());
}
std::string le16s(std::uint16_t x) { return std::string{static_cast<char>(x & 0xFF), static_cast<char>(x >> 8)}; }

// A raw implicit VR little endian data set in which a text element states 13 bytes for 10, as GE
// Genesis files do: Series Description (0008,103E), or - `late` - Image Comments (0020,4000),
// after the UIDs, so that a reader without the recovery still finds Study and Series UID and
// loses only what follows (Rows, Columns, Pixel Data).
std::vector<std::uint8_t> ge_dataset(bool late = false) {
    std::vector<std::uint8_t> b;
    imp(b, 0x0008, 0x0016, std::string(kMR) + '\0');
    imp(b, 0x0008, 0x0018, std::string(kSop) + '\0');
    imp(b, 0x0008, 0x0060, "MR");
    if (late) imp(b, 0x0008, 0x103E, kRecovered);
    else imp(b, 0x0008, 0x103E, kRecovered, 13);
    imp(b, 0x0008, 0x1060, "JANETOS ");
    imp(b, 0x0008, 0x1090, "GENESIS_SIGNA ");
    imp(b, 0x0010, 0x0010, "TEN^B ");
    imp(b, 0x0010, 0x0020, "ID4A");
    imp(b, 0x0020, 0x000D, std::string(kStudy) + '\0');
    imp(b, 0x0020, 0x000E, std::string(kSeries) + '\0');
    if (late) imp(b, 0x0020, 0x4000, "COMMENT OK", 13);
    imp(b, 0x0028, 0x0002, le16s(1));
    imp(b, 0x0028, 0x0004, "MONOCHROME2 ");
    imp(b, 0x0028, 0x0010, le16s(2));
    imp(b, 0x0028, 0x0011, le16s(2));
    imp(b, 0x7FE0, 0x0010, std::string("\x01\x00\x02\x00\x03\x00\x04\x00", 8));
    return b;
}

// The same as a Part 10 file (implicit VR little endian body).
std::vector<std::uint8_t> ge_part10(bool late = false) {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
    (void)f.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, kMR);
    (void)f.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, kSop);
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    const std::size_t meta_end = 144u + load_le32(out.data() + 140);
    out.resize(meta_end);
    const std::vector<std::uint8_t> body = ge_dataset(late);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

siaara_store* open_store(const std::filesystem::path& root) {
    siaara_store* s = nullptr;
    const std::string r = root.string();
    return siaara_store_open(r.c_str(), r.size(), nullptr, &s) == SIAARA_OK ? s : nullptr;
}

uint64_t instances(const siaara_store* store) {
    siaara_store_stats st;
    std::memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    return siaara_store_stats_get(store, &st) == SIAARA_OK ? st.instances : 0;
}

// Series Description of the instance as the store indexed it ("<none>" when it is not there).
std::string indexed_description(const siaara_store* store) {
    store::QueryFilter f;
    f.level = store::Level::Instance;
    f.sop_instance_uid = kSop;
    auto r = store->store->query(f);
    if (!r || r->rows.empty()) return "<none>";
    return r->rows[0][store::Str::SeriesDescription];
}

// Rows of the instance as the store indexed it (the recovery reads 2; without it Rows is lost).
// 99 when the instance is not in the index.
std::uint32_t indexed_rows(const siaara_store* store) {
    store::QueryFilter f;
    f.level = store::Level::Instance;
    f.sop_instance_uid = kSop;
    auto r = store->store->query(f);
    if (!r || r->rows.empty()) return 99;
    return r->rows[0].rows;
}

uint32_t quarantined(const siaara_store* store) {
    uint32_t n = 0;
    (void)siaara_store_quarantine_count(store, SIAARA_AREA_QUARANTINE, &n, nullptr);
    return n;
}

// A DICOMweb server that serves one file, as stored.
struct OneFileServer {
    web::DicomwebServer server;
    std::string base;
    explicit OneFileServer(const std::string& path) {
        web::Store store;
        store.locate = [path](const web::InstanceKey&, std::vector<web::StoredInstance>& out) -> Status {
            web::StoredInstance inst;
            inst.path = path;
            inst.size = std::filesystem::file_size(path);
            inst.transfer_syntax = std::string(ts::ImplicitVRLittleEndian);
            out.push_back(inst);
            return Status::ok();
        };
        if (server.start(web::DicomwebServerConfig{}, store))
            base = "http://127.0.0.1:" + std::to_string(server.port()) + "/dicomweb";
    }
    ~OneFileServer() { server.stop(); }
};

void write_bytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& b) {
    (void)io::write_file_atomic(p.string(), b.data(), b.size());
}

}  // namespace

TEST_CASE("net2: rc1 R-163 a local open still recovers the GE-style length; the network read options do not") {
    const std::vector<std::uint8_t> file = ge_part10();
    auto local = read_part10(Bytes::copy_of(file.data(), file.size()));
    REQUIRE(local);
    CHECK((local->read_loss & DicomFile::kReadRecovered) != 0);
    CHECK(local->dataset.get_string(0x0008103E).value_or("") == kRecovered);
    auto net = read_part10(Bytes::copy_of(file.data(), file.size()), network_read_options());
    if (net) {
        CHECK((net->read_loss & DicomFile::kReadRecovered) == 0);
        CHECK(!net->complete());
        CHECK(net->dataset.get_string(0x0008103E).value_or("") != kRecovered);
    }
    // The index of a local file / of received bytes.
    store::IndexRecord rec;
    REQUIRE(store::extract_index(Bytes::copy_of(file.data(), file.size()), rec));
    CHECK(rec[store::Str::SeriesDescription] == kRecovered);
    store::IndexRecord nrec;
    const Status ns = store::extract_index(Bytes::copy_of(file.data(), file.size()), nrec, store::network_extract_options());
    CHECK(!ns || nrec[store::Str::SeriesDescription] != kRecovered);
    CHECK(!store::ExtractOptions{}.network_input);
    // A local import (the user's explicit choice of a local file) keeps the local default.
    const auto dir = fresh_dir("local");
    siaara_store* store = open_store(dir / "store");
    REQUIRE(store != nullptr);
    REQUIRE(siaara_store_add_memory(store, file.data(), file.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
    CHECK(indexed_description(store) == kRecovered);
    siaara_store_free(store);
    const std::vector<std::uint8_t> late = ge_part10(true);
    store = open_store(dir / "store_late");
    REQUIRE(store != nullptr);
    REQUIRE(siaara_store_add_memory(store, late.data(), late.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
    CHECK_EQ(indexed_rows(store), 2u);
    siaara_store_free(store);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("net2: rc1 R-163 C-STORE receive and quarantine release: the GE-style data set is incomplete, never recovered") {
    const std::vector<std::uint8_t> body = ge_dataset();
    const auto send = [&](std::uint16_t port) -> std::uint16_t {
        net::ScuConfig c;
        c.calling_ae = "GE_SCU";
        c.called_ae = "GE_SCP";
        c.host = "127.0.0.1";
        c.port = port;
        net::Scu scu(c);
        scu.add_context(kMR, {std::string(ts::ImplicitVRLittleEndian)});
        if (!scu.connect()) return 0xFFFF;
        net::Association* a = scu.association();
        const net::NegotiatedContext* pc = a->find_context(kMR);
        if (!pc) return 0xFFFE;
        if (!net::send_dimse_raw(*a, pc->id, net::make_c_store_rq(a->next_message_id(), kMR, kSop), body.data(), body.size()))
            return 0xFFFD;
        auto r = net::receive_dimse(*a, 5000);
        const std::uint16_t st = r ? r->message.command.status.value_or(0xFFFC) : 0xFFFB;
        (void)scu.release();
        return st;
    };
    const auto start = [](siaara_store* store, int32_t policy, siaara_scp** scp) {
        siaara_scp_options o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.ae_title = "GE_SCP";
        o.bind_address = "127.0.0.1";
        o.allow_any_calling_ae = 1;
        o.idle_timeout_ms = 10000;
        o.incomplete_policy = policy;
        return siaara_scp_start(&o, store, nullptr, nullptr, scp) == SIAARA_OK;
    };
    const auto port_of = [](siaara_scp* scp) {
        uint32_t p = 0;
        (void)siaara_scp_port(scp, &p);
        return static_cast<std::uint16_t>(p);
    };
    const auto dir = fresh_dir("cstore");
    {  // Default policy (QUARANTINE): kept outside the archive.
        siaara_store* store = open_store(dir / "q");
        REQUIRE(store != nullptr);
        siaara_scp* scp = nullptr;
        REQUIRE(start(store, SIAARA_INCOMPLETE_DEFAULT, &scp));
        CHECK_EQ(send(port_of(scp)), 0xB007);
        CHECK_EQ(instances(store), 0u);
        CHECK_EQ(quarantined(store), 1u);
        siaara_scp_free(scp);
        // Released by the operator: stored as received, indexed without the recovery.
        siaara_quarantine_record r;
        std::memset(&r, 0, sizeof r);
        r.struct_size = sizeof r;
        REQUIRE(siaara_store_quarantine_get(store, SIAARA_AREA_QUARANTINE, 0, &r) == SIAARA_OK);
        CHECK((r.read_loss & SIAARA_READ_RECOVERED) == 0);
        char uid[65];
        REQUIRE(siaara_store_quarantine_release(store, SIAARA_AREA_QUARANTINE, r.id, SIAARA_DUP_KEEP, nullptr, uid, sizeof uid, nullptr) ==
                SIAARA_OK);
        CHECK(std::string(uid) == kSop);
        CHECK(indexed_description(store) != kRecovered);
        siaara_store_free(store);
    }
    {  // ACCEPT_AS_RECEIVED: stored exactly as received and flagged; the index is not "recovered".
        siaara_store* store = open_store(dir / "a");
        REQUIRE(store != nullptr);
        siaara_scp* scp = nullptr;
        REQUIRE(start(store, SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED, &scp));
        const std::uint16_t st = send(port_of(scp));
        CHECK(st == 0xB007 || st == 0x0000);
        CHECK_EQ(instances(store), 1u);
        CHECK(indexed_description(store) != kRecovered);
        CHECK(indexed_description(store) != "<none>");
        siaara_scp_free(scp);
        siaara_store_free(store);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("web/ rc1 R-163 WADO client, parts to file, retrieve-to-store and STOW-RS do not recover the GE-style length") {
    const auto dir = fresh_dir("web");
    const std::vector<std::uint8_t> file = ge_part10(true);  // the UIDs are readable without the recovery
    write_bytes(dir / "ge.dcm", file);
    OneFileServer origin((dir / "ge.dcm").string());
    REQUIRE(!origin.base.empty());
    {  // C++ client: retrieve() parses the parts with the network read options.
        web::DicomwebClientConfig cc;
        cc.base_url = origin.base;
        web::DicomwebClient client(cc);
        auto files = client.retrieve(kStudy, {}, {}, {"*"});
        if (files) {
            REQUIRE(files->size() == 1);
            CHECK(((*files)[0].read_loss & DicomFile::kReadRecovered) == 0);
            CHECK(!(*files)[0].complete());
            CHECK(!(*files)[0].dataset.contains(tags::Rows));
        }
    }
    siaara_web_client* wc = nullptr;
    REQUIRE(siaara_web_client_create(origin.base.data(), origin.base.size(), 5000, &wc) == SIAARA_OK);
    const char* as_stored[] = {"*"};
    {  // C API: a retrieved part opened as a file.
        siaara_web_parts* parts = nullptr;
        REQUIRE(siaara_web_wado_retrieve(wc, kStudy, nullptr, nullptr, as_stored, 1, &parts) == SIAARA_OK);
        siaara_file* f = nullptr;
        const siaara_status s = siaara_web_parts_open_file(parts, 0, &f);
        if (s == SIAARA_OK) {
            siaara_read_report rr;
            std::memset(&rr, 0, sizeof rr);
            rr.struct_size = sizeof rr;
            REQUIRE(siaara_file_read_report(f, &rr) == SIAARA_OK);
            CHECK((rr.loss & SIAARA_READ_RECOVERED) == 0);
            CHECK(rr.loss != 0);
            siaara_file_free(f);
        }
        // Parts added to a store: indexed as network input.
        siaara_store* dest = open_store(dir / "parts");
        REQUIRE(dest != nullptr);
        uint32_t added = 0, failed = 0;
        (void)siaara_web_parts_add_to_store(parts, dest, SIAARA_DUP_KEEP, &added, &failed);
        CHECK(indexed_rows(dest) != 2u);
        siaara_store_free(dest);
        siaara_web_parts_free(parts);
    }
    {  // Retrieve-to-store, default policy: the part is incomplete -> quarantine, not the archive.
        siaara_store* dest = open_store(dir / "ret_q");
        REQUIRE(dest != nullptr);
        siaara_web_retrieve_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.store = dest;
        ro.transfer_syntaxes = as_stored;
        ro.transfer_syntax_count = 1;
        siaara_web_retrieve_report rep;
        std::memset(&rep, 0, sizeof rep);
        rep.struct_size = sizeof rep;
        CHECK(siaara_web_wado_retrieve_to_ex(wc, kStudy, nullptr, nullptr, &ro, nullptr, nullptr, &rep) != SIAARA_OK);
        CHECK_EQ(rep.stored, 0u);
        CHECK_EQ(rep.quarantined, 1u);
        CHECK_EQ(instances(dest), 0u);
        CHECK_EQ(quarantined(dest), 1u);
        siaara_store_free(dest);
    }
    {  // Retrieve-to-store, ACCEPT_AS_RECEIVED: kept as received; the store's index does not recover.
        siaara_store* dest = open_store(dir / "ret_a");
        REQUIRE(dest != nullptr);
        siaara_web_retrieve_options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.store = dest;
        ro.transfer_syntaxes = as_stored;
        ro.transfer_syntax_count = 1;
        ro.incomplete_policy = SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED;
        siaara_web_retrieve_report rep;
        std::memset(&rep, 0, sizeof rep);
        rep.struct_size = sizeof rep;
        CHECK(siaara_web_wado_retrieve_to_ex(wc, kStudy, nullptr, nullptr, &ro, nullptr, nullptr, &rep) == SIAARA_OK);
        CHECK_EQ(rep.incomplete_accepted, 1u);
        CHECK_EQ(instances(dest), 1u);
        CHECK_EQ(indexed_rows(dest), 0u);  // stored as received, indexed without the recovery (2 with it)
        siaara_store_free(dest);
    }
    siaara_web_client_free(wc);
    // STOW-RS into our server.
    const auto stow = [&](std::uint16_t port) {
        web::multipart::Writer w;
        w.add("application/dicom", std::string_view(reinterpret_cast<const char*>(file.data()), file.size()));
        web::http::Request rq;
        rq.method = "POST";
        rq.target = "/dicomweb/studies";
        rq.body = w.finish();
        rq.headers.push_back({"Content-Type", w.content_type("application/dicom")});
        rq.headers.push_back({"Accept", "application/dicom+json"});
        web::http::Client c("127.0.0.1", port);
        return c.send(rq);
    };
    for (const int32_t policy : {SIAARA_INCOMPLETE_DEFAULT, SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED}) {
        siaara_store* dest = open_store(dir / ("stow" + std::to_string(policy)));
        REQUIRE(dest != nullptr);
        siaara_web_server_options o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.incomplete_policy = policy;
        siaara_web_server* ws = nullptr;
        REQUIRE(siaara_web_server_start(&o, dest, &ws) == SIAARA_OK);
        uint32_t port = 0;
        REQUIRE(siaara_web_server_port(ws, &port) == SIAARA_OK);
        auto rs = stow(static_cast<std::uint16_t>(port));
        REQUIRE(rs);
        if (policy == SIAARA_INCOMPLETE_DEFAULT) {
            CHECK_EQ(instances(dest), 0u);
            CHECK_EQ(quarantined(dest), 1u);
        } else {
            CHECK_EQ(instances(dest), 1u);
            CHECK_EQ(indexed_rows(dest), 0u);
        }
        siaara_web_server_free(ws);
        siaara_store_free(dest);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Round 6, R-194: rebuild and verify re-read stored files. A file is read again the way it was
// read when it was added (the index remembers: store::kNetworkInput); a file the index does not
// know is read with the network read options unless StoreOptions says otherwise.
TEST_CASE("net2: rc2 R-194 a rebuild re-reads a received instance with the network read options, a local import with the local ones") {
    const auto dir = fresh_dir("rebuild");
    const std::vector<std::uint8_t> late = ge_part10(true);  // the UIDs are read either way; Rows only with the recovery
    const auto flags_of = [](const store::Store& st) -> std::uint32_t {
        auto r = st.instance(kSop);
        return r ? r->flags : 0xFFFFFFFFu;
    };
    {
        // Received: indexed without the recovery, flagged incomplete by the receiver.
        siaara_store* store = open_store(dir / "received");
        REQUIRE(store != nullptr);
        REQUIRE(store->store->add_buffer(Bytes::copy_of(late.data(), late.size()), store::DuplicatePolicy::Keep, store::kIncompleteRead,
                                         store::network_extract_options()));
        CHECK_EQ(indexed_rows(store), 0u);
        CHECK((flags_of(*store->store) & store::kNetworkInput) != 0);
        CHECK((flags_of(*store->store) & store::kIncompleteRead) != 0);
        REQUIRE(store->store->rebuild());
        CHECK_EQ(indexed_rows(store), 0u);  // not "recovered" by the rebuild
        CHECK((flags_of(*store->store) & store::kNetworkInput) != 0);
        CHECK((flags_of(*store->store) & store::kIncompleteRead) != 0);
        // A deep verify reads it the same way: nothing to repair, the flags stay.
        auto rep = store->store->verify(true, true);
        REQUIRE(rep);
        CHECK_EQ(rep->content_mismatch, 0u);
        CHECK_EQ(indexed_rows(store), 0u);
        CHECK((flags_of(*store->store) & (store::kNetworkInput | store::kIncompleteRead)) == (store::kNetworkInput | store::kIncompleteRead));
        siaara_store_free(store);
        // After closing and opening again (snapshot and journal) the flag is still there.
        store = open_store(dir / "received");
        REQUIRE(store != nullptr);
        CHECK((flags_of(*store->store) & store::kNetworkInput) != 0);
        REQUIRE(store->store->rebuild());
        CHECK_EQ(indexed_rows(store), 0u);
        siaara_store_free(store);
    }
    {
        // A local import keeps the local default through a rebuild.
        siaara_store* store = open_store(dir / "local");
        REQUIRE(store != nullptr);
        REQUIRE(siaara_store_add_memory(store, late.data(), late.size(), SIAARA_DUP_KEEP, nullptr) == SIAARA_OK);
        CHECK_EQ(indexed_rows(store), 2u);
        CHECK((flags_of(*store->store) & store::kNetworkInput) == 0);
        REQUIRE(store->store->rebuild());
        CHECK_EQ(indexed_rows(store), 2u);
        CHECK((flags_of(*store->store) & store::kNetworkInput) == 0);
        siaara_store_free(store);
    }
    // A file under data/ that the index does not know: the network read options by default.
    static_assert(store::StoreOptions::kDefaultReindexUnknownAsNetworkInput);
    for (const bool as_network : {true, false}) {
        const std::filesystem::path root = dir / (as_network ? "unknown_net" : "unknown_local");
        store::StoreOptions so;
        CHECK(so.reindex_unknown_as_network_input);
        so.reindex_unknown_as_network_input = as_network;
        auto st = store::Store::open(root.string(), so);
        REQUIRE(st);
        write_bytes(root / "data" / "by-hand.dcm", late);
        REQUIRE((*st)->rebuild());
        auto r = (*st)->instance(kSop);
        REQUIRE(r);
        CHECK_EQ(r->rows, as_network ? 0u : 2u);
        CHECK(((r->flags & store::kNetworkInput) != 0) == as_network);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
