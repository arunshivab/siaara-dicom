// Release-candidate review, quarantine area: R-159 (an incomplete data set is checked against the
// caps before any copy and streamed to disk, never held whole in memory), R-165 (room reserved
// under the lock, sender share, free-space floor), R-166 (bytes without a record are kept and
// listed), R-167 (one release at a time; the stored SOP Instance UID is returned).
// Round 6, R-190: a COMPLETE spooled data set that is accepted is streamed into the store and the
// folder as well (the same heap bound on the accepted path).
// R-196: temporary files an interrupted write left in the area are removed when it is read.
// Uses the allocation guard (alloc_guard.hpp), so this file is left out of TSan builds.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

namespace {

// A complete, valid data set of kBig bytes in explicit VR little endian (a few header elements,
// then Pixel Data filling the rest) as a mapped file with a hole: a spooled C-STORE data set.
Bytes big_dataset_mapping(const std::filesystem::path& dir, const std::string& sop) {
    std::vector<std::uint8_t> head;
    const auto put16 = [&](std::uint32_t v) {
        head.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        head.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    };
    const auto text = [&](std::uint32_t group, std::uint32_t element, const char* vr, std::string v) {
        if (v.size() % 2) v.push_back(vr[0] == 'U' && vr[1] == 'I' ? '\0' : ' ');
        put16(group);
        put16(element);
        head.push_back(static_cast<std::uint8_t>(vr[0]));
        head.push_back(static_cast<std::uint8_t>(vr[1]));
        put16(static_cast<std::uint32_t>(v.size()));
        head.insert(head.end(), v.begin(), v.end());
    };
    const auto us = [&](std::uint32_t group, std::uint32_t element, std::uint32_t v) {
        put16(group);
        put16(element);
        head.push_back('U');
        head.push_back('S');
        put16(2);
        put16(v);
    };
    text(0x0008, 0x0016, "UI", "1.2.840.10008.5.1.4.1.1.7");
    text(0x0008, 0x0018, "UI", sop);
    text(0x0008, 0x0060, "CS", "OT");
    text(0x0020, 0x000D, "UI", "2.25.190900");
    text(0x0020, 0x000E, "UI", "2.25.190901");
    us(0x0028, 0x0002, 1);
    text(0x0028, 0x0004, "CS", "MONOCHROME2");
    us(0x0028, 0x0010, 1);
    us(0x0028, 0x0011, 1);
    us(0x0028, 0x0100, 8);
    us(0x0028, 0x0101, 8);
    us(0x0028, 0x0102, 7);
    us(0x0028, 0x0103, 0);
    // (7FE0,0010) OB, reserved, 32-bit length: everything up to the end of the data set.
    put16(0x7FE0);
    put16(0x0010);
    head.push_back('O');
    head.push_back('B');
    put16(0);
    const std::size_t pixel_bytes = kBig - head.size() - 4;
    put16(static_cast<std::uint32_t>(pixel_bytes & 0xFFFFu));
    put16(static_cast<std::uint32_t>((pixel_bytes >> 16) & 0xFFFFu));
    const std::filesystem::path p = dir / ("spooled-" + sop + ".bin");
    {
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        f.seekp(static_cast<std::streamoff>(kBig - 1));
        f.put('\x7f');
    }
    auto m = io::map_file(p.string(), false);
    return m ? *m : Bytes{};
}

net::StoreRequest complete_request(const Bytes& raw, const std::string& sop) {
    net::StoreRequest rq;
    rq.calling_ae = "BIG_AE";
    rq.sop_class_uid = "1.2.840.10008.5.1.4.1.1.7";
    rq.sop_instance_uid = sop;
    rq.transfer_syntax = "1.2.840.10008.1.2.1";
    rq.raw = raw;
    return rq;
}

// True when the file is `meta` followed by `raw`, byte for byte.
bool file_is(const std::string& path, const std::vector<std::uint8_t>& meta, const Bytes& raw) {
    auto on_disk = io::map_file(path, false);
    if (!on_disk || on_disk->size() != meta.size() + raw.size()) return false;
    return std::memcmp(on_disk->data(), meta.data(), meta.size()) == 0 &&
           std::memcmp(on_disk->data() + meta.size(), raw.data(), raw.size()) == 0;
}

}  // namespace

TEST_CASE("rc2 R-190: an accepted spooled data set is streamed into the store, within a small heap budget") {
    const auto dir = fresh_dir("accept_store");
    siaara_store* store = open_store(dir / "store");
    REQUIRE(store != nullptr);
    const std::string sop = "2.25.190001";
    const Bytes raw = big_dataset_mapping(dir, sop);
    REQUIRE(raw.size() == kBig);
    const net::StoreRequest rq = complete_request(raw, sop);
    std::uint16_t status = 0xFFFF, again = 0xFFFF, replaced = 0xFFFF;
    int32_t outcome = -1, outcome_again = -1, outcome_replaced = -1;
    std::string err;
    std::size_t peak = 0;
    bool refused = true;
    {
        siaara_test::AllocGuard guard(kBudget);
        status = capi::dimse::keep_instance(rq, store, store::DuplicatePolicy::Keep, std::string(), &outcome, err);
        // The same instance again: kept (nothing written), then replaced - both within the budget.
        again = capi::dimse::keep_instance(rq, store, store::DuplicatePolicy::Keep, std::string(), &outcome_again, err);
        replaced = capi::dimse::keep_instance(rq, store, store::DuplicatePolicy::Replace, std::string(), &outcome_replaced, err);
        peak = guard.peak();
        refused = guard.refused();
    }
    CHECK_EQ(status, 0);
    CHECK_EQ(again, 0);
    CHECK_EQ(replaced, 0);
    CHECK(err.empty());
    CHECK_EQ(outcome, SIAARA_ADD_ADDED);
    CHECK_EQ(outcome_again, SIAARA_ADD_KEPT_EXISTING);
    CHECK_EQ(outcome_replaced, SIAARA_ADD_REPLACED);
    CHECK(!refused);
    CHECK(peak < kBudget);
    std::printf("  48 MiB data set into the store (added, kept, replaced): peak heap %zu bytes\n", peak);
    // The stored file is the file meta followed by the bytes as received, and it is indexed as a
    // buffer of the same bytes would be (size, pixel data offset, keys).
    std::vector<std::uint8_t> meta;
    REQUIRE(net::encode_received_meta(rq, meta));
    auto rec = store->store->instance(sop);
    REQUIRE(rec);
    CHECK_EQ(rec->file_size, meta.size() + kBig);
    CHECK((*rec)[store::Str::SopClassUid] == "1.2.840.10008.5.1.4.1.1.7");
    CHECK((*rec)[store::Str::StudyInstanceUid] == "2.25.190900");
    CHECK((*rec)[store::Str::Modality] == "OT");
    CHECK((rec->flags & store::kHasPixelData) != 0);
    CHECK((rec->flags & store::kPixelOffsetKnown) != 0);
    CHECK((rec->flags & store::kIncompleteRead) == 0);
    CHECK_EQ(rec->pixel_data_offset + rec->pixel_data_length, meta.size() + kBig);
    auto list = store->store->instances_of("2.25.190901");
    REQUIRE(list);
    REQUIRE(list->size() == 1);
    CHECK(file_is((*list)[0][store::Str::FilePath], meta, raw));
    // No temporary file stays in the data folder.
    std::size_t leftovers = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir / "store" / "data", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && it->path().extension() != ".dcm") ++leftovers;
    CHECK_EQ(leftovers, 0u);
    // An instance accepted although incomplete keeps its flag on this path too.
    const std::string sop2 = "2.25.190002";
    const Bytes raw2 = big_dataset_mapping(dir, sop2);
    net::StoreRequest rq2 = complete_request(raw2, sop2);
    rq2.read_loss = 1;
    CHECK_EQ(capi::dimse::keep_instance(rq2, store, store::DuplicatePolicy::Keep, std::string(), nullptr, err), 0);
    auto rec2 = store->store->instance(sop2);
    REQUIRE(rec2);
    CHECK((rec2->flags & store::kIncompleteRead) != 0);
    siaara_store_free(store);
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc2 R-190: an accepted spooled data set is streamed into the folder, within a small heap budget") {
    const auto dir = fresh_dir("accept_folder");
    const std::filesystem::path folder = dir / "inbox";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string sop = "2.25.190003";
    const Bytes raw = big_dataset_mapping(dir, sop);
    REQUIRE(raw.size() == kBig);
    const net::StoreRequest rq = complete_request(raw, sop);
    std::uint16_t status = 0xFFFF, again = 0xFFFF;
    int32_t outcome = -1, outcome_again = -1;
    bool kept = true, kept_again = false;
    std::string err;
    std::size_t peak = 0;
    bool refused = true;
    {
        siaara_test::AllocGuard guard(kBudget);
        status = capi::dimse::keep_instance(rq, nullptr, store::DuplicatePolicy::Keep, folder.string(), &outcome, err, &kept);
        again = capi::dimse::keep_instance(rq, nullptr, store::DuplicatePolicy::Keep, folder.string(), &outcome_again, err, &kept_again);
        peak = guard.peak();
        refused = guard.refused();
    }
    CHECK_EQ(status, 0);
    CHECK_EQ(again, 0);
    CHECK(err.empty());
    CHECK_EQ(outcome, SIAARA_ADD_ADDED);
    CHECK_EQ(outcome_again, SIAARA_ADD_KEPT_EXISTING);
    CHECK(!kept);
    CHECK(kept_again);
    CHECK(!refused);
    CHECK(peak < kBudget);
    std::printf("  48 MiB data set into the folder (added, kept): peak heap %zu bytes\n", peak);
    std::vector<std::uint8_t> meta;
    REQUIRE(net::encode_received_meta(rq, meta));
    CHECK(file_is((folder / (sop + ".dcm")).string(), meta, raw));
    CHECK_EQ(files_in(folder), 1u);  // the instance, no temporary file
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rc2 R-196: stale temporary files of an interrupted quarantine write are removed when the area is read") {
    const auto dir = fresh_dir("stale");
    const std::filesystem::path area = dir / "quarantine";
    std::error_code ec;
    std::filesystem::create_directories(area, ec);
    const auto touch = [&](const char* name, int minutes_old) {
        const std::filesystem::path p = area / name;
        std::ofstream(p, std::ios::binary) << "half an instance";
        std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() - std::chrono::minutes(minutes_old), ec);
        return p;
    };
    const std::vector<std::uint8_t> a = small_part10("2.25.196001");
    static_assert(store::kDefaultQuarantineStaleTempMinutes == 60);
    const store::QuarantineLimits defaults;
    CHECK_EQ(defaults.stale_temp_after_minutes, store::kDefaultQuarantineStaleTempMinutes);
    {
        // The option off: nothing is touched.
        const auto old_file = touch(".siaara-tmp-0123456789abcdef.part", 600);
        store::QuarantineLimits lim;
        lim.stale_temp_after_minutes = 0;
        store::Quarantine q(dir.string(), lim);
        CHECK_EQ(q.items(), 0u);
        CHECK(std::filesystem::exists(old_file));
    }
    const auto old_file = area / ".siaara-tmp-0123456789abcdef.part";
    const auto fresh = touch(".siaara-tmp-fedcba9876543210.part", 5);       // could be a write in progress
    const auto other = touch("notes.part", 600);                             // not ours
    const auto other2 = touch(".siaara-tmp-keep.txt", 600);                  // not ours either
    std::vector<std::string> notes;
    store::QuarantineLimits lim;
    lim.note = [&](const std::string& m) { notes.push_back(m); };
    store::Quarantine q(dir.string(), lim);
    REQUIRE(q.add(Bytes::copy_of(a.data(), a.size()), rec_of("AE_A")));
    CHECK_EQ(q.items(), 1u);
    CHECK_EQ(q.total_bytes(), a.size());  // temporary files never counted
    CHECK(!std::filesystem::exists(old_file));
    CHECK(std::filesystem::exists(fresh));
    CHECK(std::filesystem::exists(other));
    CHECK(std::filesystem::exists(other2));
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("QUARANTINE_STALE_TEMP_REMOVED: 1 ") == 0);
    std::filesystem::remove_all(dir, ec);
}
