// Release-candidate review, quarantine area: R-159 (an incomplete data set is checked against the
// caps before any copy and streamed to disk, never held whole in memory), R-165 (room reserved
// under the lock, sender share, free-space floor), R-166 (bytes without a record are kept and
// listed), R-167 (one release at a time; the stored SOP Instance UID is returned).
// Uses the allocation guard (alloc_guard.hpp), so this file is left out of TSan builds.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "alloc_guard.hpp"
#include "capi/capi_internal.hpp"
#include "capi/capi_net_internal.hpp"
#include "core/io.hpp"
#include "core/writer.hpp"
#include "deid/sha256.hpp"
#include "net/scu.hpp"
#include "siaara/siaara.h"
#include "store/quarantine.hpp"
#include "test.hpp"

using namespace siaara;

namespace {

constexpr std::size_t kBig = std::size_t{48} << 20;      // the "spooled" data set
constexpr std::size_t kBudget = std::size_t{2} << 20;    // heap allowed while it is quarantined

std::filesystem::path fresh_dir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("siaara_rc1q_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

siaara_store* open_store(const std::filesystem::path& root, uint64_t q_bytes = 0, uint32_t q_items = 0, uint32_t share = 0) {
    siaara_store_options so;
    std::memset(&so, 0, sizeof so);
    so.struct_size = sizeof so;
    so.create = 1;
    so.quarantine_max_bytes = q_bytes;
    so.quarantine_max_items = q_items;
    so.rejected_max_bytes = q_bytes;
    so.quarantine_sender_share_percent = share;
    siaara_store* s = nullptr;
    const std::string r = root.string();
    return siaara_store_open(r.c_str(), r.size(), &so, &s) == SIAARA_OK ? s : nullptr;
}

// A file of kBig bytes that takes (almost) no disk space where the file system has holes, mapped
// read-only: what a spooled C-STORE data set looks like to the receive hooks.
Bytes big_mapping(const std::filesystem::path& dir) {
    const std::filesystem::path p = dir / "spooled.bin";
    {
        std::ofstream f(p, std::ios::binary);
        f.write("\x08\x00\x18\x00", 4);
        f.seekp(static_cast<std::streamoff>(kBig - 1));
        f.put('\x7f');
    }
    auto m = io::map_file(p.string(), false);
    return m ? *m : Bytes{};
}

net::StoreRequest request(const Bytes& raw, const std::string& ae = "BIG_AE") {
    net::StoreRequest rq;
    rq.calling_ae = ae;
    rq.sop_class_uid = "1.2.840.10008.5.1.4.1.1.4";
    rq.sop_instance_uid = "2.25.159001";
    rq.transfer_syntax = "1.2.840.10008.1.2.1";
    rq.raw = raw;
    rq.read_loss = 1;
    rq.read_warning = "INPUT_TRUNCATED";
    return rq;
}

std::size_t files_in(const std::filesystem::path& dir) {
    std::error_code ec;
    std::size_t n = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) ++n;
    return n;
}

std::vector<std::uint8_t> small_part10(const std::string& sop) {
    DicomFile f;
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    (void)f.dataset.set_string(tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.4");
    (void)f.dataset.set_string(tags::SOPInstanceUID, VR::UI, sop);
    (void)f.dataset.set_string(tags::StudyInstanceUID, VR::UI, "2.25.167900");
    (void)f.dataset.set_string(tags::SeriesInstanceUID, VR::UI, "2.25.167901");
    (void)f.dataset.set_string(tags::Modality, VR::CS, "MR");
    std::vector<std::uint8_t> out;
    (void)write_part10(f, out);
    return out;
}

store::QuarantineRecord rec_of(const std::string& ae) {
    store::QuarantineRecord r;
    r.source = "C-STORE";
    r.calling_ae = ae;
    return r;
}

}  // namespace

TEST_CASE("rc1 R-159: an incomplete spooled data set is quarantined by streaming, within a small heap budget") {
    const auto dir = fresh_dir("stream");
    siaara_store* store = open_store(dir / "store");
    REQUIRE(store != nullptr);
    const Bytes raw = big_mapping(dir);
    REQUIRE(raw.size() == kBig);
    const net::StoreRequest rq = request(raw);
    std::uint16_t kept = 0xFFFF, kept_copy = 0xFFFF;
    std::size_t peak = 0;
    bool refused = true;
    {
        siaara_test::AllocGuard guard(kBudget);
        kept = capi::quarantine_request(store, rq, "C-STORE", "192.0.2.1");
        kept_copy = capi::quarantine_request(store, rq, "C-STORE", "192.0.2.1", true);  // the "rejected" area
        peak = guard.peak();
        refused = guard.refused();
    }
    CHECK_EQ(kept, 0);
    CHECK_EQ(kept_copy, 0);
    CHECK(!refused);
    CHECK(peak < kBudget);
    // One item: the file meta followed by the bytes as received; its id is the SHA-256 of that.
    uint32_t items = 0;
    uint64_t bytes = 0;
    REQUIRE(siaara_store_quarantine_count(store, SIAARA_AREA_QUARANTINE, &items, &bytes) == SIAARA_OK);
    CHECK_EQ(items, 1u);
    std::vector<std::uint8_t> meta;
    REQUIRE(net::encode_received_meta(rq, meta));
    CHECK_EQ(bytes, meta.size() + kBig);
    siaara_quarantine_record r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    REQUIRE(siaara_store_quarantine_get(store, SIAARA_AREA_QUARANTINE, 0, &r) == SIAARA_OK);
    deid::Sha256 sha;
    sha.update(meta.data(), meta.size());
    sha.update(raw.data(), raw.size());
    const auto digest = sha.finish();
    std::string hex;
    for (const std::uint8_t b : digest) {
        hex.push_back("0123456789abcdef"[b >> 4]);
        hex.push_back("0123456789abcdef"[b & 15u]);
    }
    CHECK(hex == r.id);
    CHECK_EQ(r.record_missing, 0);
    CHECK(std::string(r.calling_ae) == "BIG_AE");
    // The bytes on disk are the stream, and nothing else was left in the area (no temporary file).
    auto on_disk = io::map_file((dir / "store" / "quarantine" / (hex + ".bin")).string(), false);
    REQUIRE(on_disk);
    REQUIRE(on_disk->size() == meta.size() + kBig);
    CHECK(std::memcmp(on_disk->data(), meta.data(), meta.size()) == 0);
    CHECK(std::memcmp(on_disk->data() + meta.size(), raw.data(), kBig) == 0);
    on_disk = Status(Errc::Internal, "released");
    CHECK_EQ(files_in(dir / "store" / "quarantine"), 2u);
    // The same bytes again: one item, still within the budget, and no temporary file stays.
    {
        siaara_test::AllocGuard guard(kBudget);
        kept = capi::quarantine_request(store, rq, "C-STORE", "192.0.2.1");
        refused = guard.refused();
    }
    CHECK_EQ(kept, 0);
    CHECK(!refused);
    CHECK_EQ(files_in(dir / "store" / "quarantine"), 2u);
    siaara_store_free(store);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc1 R-159: a data set over the quarantine cap is refused before any copy or write") {
    const auto dir = fresh_dir("cap");
    siaara_store* store = open_store(dir / "store", std::uint64_t{1} << 20);
    REQUIRE(store != nullptr);
    const Bytes raw = big_mapping(dir);
    REQUIRE(raw.size() == kBig);
    const net::StoreRequest rq = request(raw);
    std::uint16_t kept = 0, kept_copy = 0;
    std::size_t peak = 0;
    {
        siaara_test::AllocGuard guard(kBudget);
        kept = capi::quarantine_request(store, rq, "C-STORE", "192.0.2.1");
        kept_copy = capi::quarantine_request(store, rq, "C-STORE", "192.0.2.1", true);
        peak = guard.peak();
    }
    CHECK_EQ(kept, 0xA700);
    CHECK_EQ(kept_copy, 0xA700);
    CHECK(peak < kBudget);
    // Nothing was written: not the item, not a temporary file, not even the area's folder.
    CHECK_EQ(files_in(dir / "store" / "quarantine"), 0u);
    CHECK_EQ(files_in(dir / "store" / "rejected"), 0u);
    siaara_store_free(store);
    // The free-space floor is checked first too.
    store::QuarantineLimits lim;
    lim.min_free_disk_bytes = std::uint64_t{1} << 62;
    store::Quarantine q((dir / "floor").string(), lim);
    const std::vector<std::uint8_t> small(1000, 0x41);
    auto r = q.add(Bytes::copy_of(small.data(), small.size()), rec_of("A"));
    CHECK(!r);
    CHECK(!r && r.error().code == Errc::LimitExceeded);
    CHECK_EQ(files_in(dir / "floor" / "quarantine"), 0u);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc1 R-165: room is reserved for concurrent adds; a sender's share of the caps") {
    const auto dir = fresh_dir("share");
    {
        // Eight threads, room for five items of 1000 bytes: exactly five are kept, never more.
        store::QuarantineLimits lim;
        lim.max_bytes = 5000;
        store::Quarantine q((dir / "a").string(), lim);
        std::atomic<int> added{0}, full{0}, other{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&, t] {
                const std::vector<std::uint8_t> item(1000, static_cast<std::uint8_t>('a' + t));
                auto r = q.add(Bytes::copy_of(item.data(), item.size()), rec_of("AE" + std::to_string(t)));
                if (r) added.fetch_add(1);
                else if (r.error().code == Errc::LimitExceeded) full.fetch_add(1);
                else other.fetch_add(1);
            });
        for (std::thread& t : threads) t.join();
        CHECK_EQ(added.load(), 5);
        CHECK_EQ(full.load(), 3);
        CHECK_EQ(other.load(), 0);
        CHECK_EQ(q.items(), 5u);
        CHECK_EQ(q.total_bytes(), 5000u);
    }
    {
        // Default: no share limit - one sender may fill the area.
        store::QuarantineLimits lim;
        lim.max_items = 10;
        CHECK_EQ(lim.sender_share_percent, store::kDefaultQuarantineSenderSharePercent);
        store::Quarantine q((dir / "b").string(), lim);
        for (int i = 0; i < 10; ++i) {
            const std::vector<std::uint8_t> item(100, static_cast<std::uint8_t>(i));
            CHECK(q.add(Bytes::copy_of(item.data(), item.size()), rec_of("NOISY")));
        }
        const std::vector<std::uint8_t> item(100, 0x7E);
        CHECK(!q.add(Bytes::copy_of(item.data(), item.size()), rec_of("QUIET")));
    }
    {
        // A 20 % share of 10 items: the third item of one sender is refused, others still get in.
        store::QuarantineLimits lim;
        lim.max_items = 10;
        lim.sender_share_percent = 20;
        store::Quarantine q((dir / "c").string(), lim);
        int kept = 0;
        for (int i = 0; i < 10; ++i) {
            const std::vector<std::uint8_t> item(100, static_cast<std::uint8_t>(i));
            auto r = q.add(Bytes::copy_of(item.data(), item.size()), rec_of("NOISY"));
            if (r) ++kept;
            else CHECK(r.error().code == Errc::LimitExceeded);
        }
        CHECK_EQ(kept, 2);
        const std::vector<std::uint8_t> item(100, 0x7E);
        CHECK(q.add(Bytes::copy_of(item.data(), item.size()), rec_of("QUIET")));
        // DICOMweb has no calling AE: the peer address is the sender.
        store::QuarantineRecord web;
        web.source = "STOW-RS";
        web.peer = "198.51.100.7";
        int web_kept = 0;
        for (int i = 0; i < 4; ++i) {
            const std::vector<std::uint8_t> w(100, static_cast<std::uint8_t>(0x30 + i));
            if (q.add(Bytes::copy_of(w.data(), w.size()), web)) ++web_kept;
        }
        CHECK_EQ(web_kept, 2);
        // The byte share: 20 % of the byte cap.
        store::QuarantineLimits bl;
        bl.max_bytes = 10000;
        bl.sender_share_percent = 20;
        store::Quarantine qb((dir / "d").string(), bl);
        const std::vector<std::uint8_t> big(2001, 0x55), fits(2000, 0x56);
        CHECK(!qb.add(Bytes::copy_of(big.data(), big.size()), rec_of("NOISY")));
        CHECK(qb.add(Bytes::copy_of(fits.data(), fits.size()), rec_of("NOISY")));
        CHECK(!qb.add(Bytes::copy_of(item.data(), item.size()), rec_of("NOISY")));
        CHECK(qb.add(Bytes::copy_of(item.data(), item.size()), rec_of("QUIET")));
    }
    {
        // The C API validates the option.
        siaara_store_options so;
        std::memset(&so, 0, sizeof so);
        so.struct_size = sizeof so;
        so.create = 1;
        so.quarantine_sender_share_percent = 101;
        siaara_store* s = nullptr;
        const std::string r = (dir / "e").string();
        CHECK(siaara_store_open(r.c_str(), r.size(), &so, &s) == SIAARA_ERR_INVALID_ARGUMENT);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc1 R-166: bytes without their record are kept, listed as record missing, and counted") {
    const auto dir = fresh_dir("orphan");
    const std::vector<std::uint8_t> a = small_part10("2.25.166001"), b = small_part10("2.25.166002");
    std::string id_a;
    {
        store::Quarantine q(dir.string());
        REQUIRE(q.add(Bytes::copy_of(a.data(), a.size()), rec_of("AE_A")));
        REQUIRE(q.add(Bytes::copy_of(b.data(), b.size()), rec_of("AE_B")));
        auto l = q.list();
        REQUIRE(l);
        for (const auto& r : *l)
            if (r.size == a.size() && r.calling_ae == "AE_A") id_a = r.id;
    }
    REQUIRE(id_a.size() == 64);
    // "A crash between the two writes": the record of item A is not there.
    REQUIRE(std::filesystem::remove(dir / "quarantine" / (id_a + ".rec")));
    std::vector<std::string> notes;
    store::QuarantineLimits lim;
    lim.note = [&](const std::string& m) { notes.push_back(m); };
    store::Quarantine q(dir.string(), lim);
    auto l = q.list();
    REQUIRE(l);
    REQUIRE(l->size() == 2);
    CHECK_EQ(q.total_bytes(), a.size() + b.size());
    const store::QuarantineRecord* orphan = nullptr;
    for (const auto& r : *l)
        if (r.id == id_a) orphan = &r;
    REQUIRE(orphan != nullptr);
    CHECK(orphan->record_missing);
    CHECK(orphan->calling_ae.empty());
    CHECK_EQ(orphan->size, a.size());
    CHECK(orphan->time > 1700000000);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("QUARANTINE_RECORD_MISSING") != std::string::npos);
    // The bytes are still there, exactly.
    CHECK(std::filesystem::exists(dir / "quarantine" / (id_a + ".bin")));
    auto got = q.bytes(id_a);
    REQUIRE(got);
    CHECK(got->size() == a.size() && std::memcmp(got->data(), a.data(), a.size()) == 0);
    got = Status(Errc::Internal, "released");
    // The same bytes arriving again: still one item, and it has its record again.
    auto again = q.add(Bytes::copy_of(a.data(), a.size()), rec_of("AE_A"));
    REQUIRE(again);
    CHECK(*again == store::QuarantineAdd::Duplicate);
    auto r2 = q.record(id_a);
    REQUIRE(r2);
    CHECK(!r2->record_missing);
    CHECK(r2->calling_ae == "AE_A");
    CHECK(std::filesystem::exists(dir / "quarantine" / (id_a + ".rec")));
    // A record-missing item can be deleted on request.
    REQUIRE(std::filesystem::remove(dir / "quarantine" / (id_a + ".rec")));
    store::Quarantine q2(dir.string());
    CHECK(q2.remove(id_a));
    CHECK_EQ(q2.items(), 1u);
    CHECK(!std::filesystem::exists(dir / "quarantine" / (id_a + ".bin")));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc1 R-167: two releases of one item - one stores it and returns the UID, the other finds it gone") {
    const auto dir = fresh_dir("release");
    siaara_store* store = open_store(dir / "store");
    REQUIRE(store != nullptr);
    for (int round = 0; round < 20; ++round) {
        const std::string sop = "2.25.1670" + std::to_string(10 + round);
        const std::vector<std::uint8_t> bytes = small_part10(sop);
        REQUIRE(capi::quarantine_bytes(store, Bytes::copy_of(bytes.data(), bytes.size()), "C-STORE", "AE", "", "", sop, 1, "") == 0);
        siaara_quarantine_record r;
        std::memset(&r, 0, sizeof r);
        r.struct_size = sizeof r;
        REQUIRE(siaara_store_quarantine_get(store, SIAARA_AREA_QUARANTINE, 0, &r) == SIAARA_OK);
        std::atomic<int> ok{0}, gone{0}, other{0};
        std::string uids[2];
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t)
            threads.emplace_back([&, t] {
                char uid[65] = {0};
                size_t n = 0;
                int32_t outcome = -1;
                const siaara_status s = siaara_store_quarantine_release(store, SIAARA_AREA_QUARANTINE, r.id, SIAARA_DUP_KEEP,
                                                                        &outcome, uid, sizeof uid, &n);
                if (s == SIAARA_OK) {
                    ok.fetch_add(1);
                    uids[t] = uid;
                    if (outcome != SIAARA_ADD_ADDED || n != std::strlen(uid)) other.fetch_add(1);
                } else if (s == SIAARA_ERR_NOT_FOUND) {
                    gone.fetch_add(1);
                } else {
                    other.fetch_add(1);
                }
            });
        for (std::thread& t : threads) t.join();
        CHECK_EQ(ok.load(), 1);
        CHECK_EQ(gone.load(), 1);
        CHECK_EQ(other.load(), 0);
        CHECK((uids[0] + uids[1]) == sop);
    }
    uint32_t items = 7;
    REQUIRE(siaara_store_quarantine_count(store, SIAARA_AREA_QUARANTINE, &items, nullptr) == SIAARA_OK);
    CHECK_EQ(items, 0u);
    // A buffer too small for a UID is refused before anything is done.
    const std::vector<std::uint8_t> bytes = small_part10("2.25.167099");
    REQUIRE(capi::quarantine_bytes(store, Bytes::copy_of(bytes.data(), bytes.size()), "C-STORE", "AE", "", "", "2.25.167099", 1, "") == 0);
    siaara_quarantine_record r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    REQUIRE(siaara_store_quarantine_get(store, SIAARA_AREA_QUARANTINE, 0, &r) == SIAARA_OK);
    char tiny[8];
    CHECK(siaara_store_quarantine_release(store, SIAARA_AREA_QUARANTINE, r.id, SIAARA_DUP_KEEP, nullptr, tiny, sizeof tiny, nullptr) ==
          SIAARA_ERR_BUFFER_TOO_SMALL);
    REQUIRE(siaara_store_quarantine_count(store, SIAARA_AREA_QUARANTINE, &items, nullptr) == SIAARA_OK);
    CHECK_EQ(items, 1u);
    // An older caller's record struct (without record_missing) still works.
    siaara_quarantine_record old_rec;
    std::memset(&old_rec, 0x5A, sizeof old_rec);
    old_rec.struct_size = static_cast<uint32_t>(offsetof(siaara_quarantine_record, record_missing));
    CHECK(siaara_store_quarantine_get(store, SIAARA_AREA_QUARANTINE, 0, &old_rec) == SIAARA_OK);
    CHECK_EQ(old_rec.record_missing, 0x5A5A5A5A);
    siaara_store_free(store);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
