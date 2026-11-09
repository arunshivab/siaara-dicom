// Fuzz target: local store index recovery - snapshot load and journal replay at Store::open.
// Records are framed by the harness (length + CRC-32 + type), so mutations reach the record
// decoders instead of stopping at the CRC check. Input layout:
//   byte 0      flags: bit 0 write a snapshot, bit 1 append a raw (torn) tail to the journal,
//               bit 2 journal generation one below the snapshot's (stale journal)
//   byte 1      generation
//   [u16 n]     with bit 0: number of snapshot records that follow
//   records     type (1 byte), length (u16 LE), payload; the first n go to the snapshot
//   rest        with bit 1: raw tail bytes (whatever is left after a record that does not fit)
// Property: when open succeeds, compacting and reopening gives the same counts.
#include <fstream>

#include "fuzz.hpp"
#include "fuzz_util.hpp"
#include "store/common.hpp"
#include "store/store.hpp"

using namespace siaara;
using namespace siaara::store;
namespace fs = std::filesystem;

namespace {

void header(std::string& out, const char* magic, std::uint32_t gen) {
    out.assign(magic, 8);
    std::uint8_t b[8];
    store_le32(b, 1);
    store_le32(b + 4, gen);
    out.append(reinterpret_cast<const char*>(b), 8);
}

void frame(std::string& out, std::uint8_t type, std::string_view payload) {
    std::uint8_t h[9];
    store_le32(h, static_cast<std::uint32_t>(payload.size()));
    std::uint32_t c = crc32(&type, 1);
    c = crc32(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(), c);
    store_le32(h + 4, c);
    h[8] = type;
    out.append(reinterpret_cast<const char*>(h), 9);
    out.append(payload);
}

void write(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

}  // namespace

SIAARA_FUZZ_TARGET(store_journal) {
    siaara_fuzz::Input in(data, size);
    const std::uint8_t flags = in.u8();
    const std::uint32_t gen = 1u + in.u8() % 4u;
    std::size_t snap_records = (flags & 1u) ? in.u16() : 0;
    std::string snap, journal;
    header(snap, "SIAARAS1", gen);
    header(journal, "SIAARAJ1", (flags & 4u) ? gen - 1 : ((flags & 1u) ? gen : 0));
    while (in.remaining() >= 3) {
        const std::uint8_t type = in.u8();
        const std::uint16_t len = in.u16();
        if (len > in.remaining()) break;
        const std::string_view payload = in.take(len);
        if (snap_records > 0) {
            frame(snap, type, payload);
            --snap_records;
        } else {
            frame(journal, type, payload);
        }
    }
    if (flags & 2u) journal.append(in.rest());

    const fs::path root = siaara_fuzz::scratch_dir("store") / "s";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "index", ec);
    if (flags & 1u) write(root / "index" / "snapshot.sxs", snap);
    write(root / "index" / "journal.sxj", journal);

    StoreStats first;
    {
        auto s = Store::open(root.string());
        if (!s) return 0;
        first = (*s)->stats();
        for (Level lv : {Level::Patient, Level::Study, Level::Series, Level::Instance}) {
            QueryFilter q;
            q.level = lv;
            (void)(*s)->query(q);
        }
        FUZZ_REQUIRE_OK((*s)->compact());
    }
    auto s = Store::open(root.string());
    FUZZ_REQUIRE_OK(s);
    const StoreStats again = (*s)->stats();
    FUZZ_REQUIRE(again.patients == first.patients && again.studies == first.studies &&
                 again.series == first.series && again.instances == first.instances);
    return 0;
}
