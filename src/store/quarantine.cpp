// SIAARA DICOM Toolkit - quarantine area of a store (store/quarantine.hpp).
#include "store/quarantine.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#include "core/io.hpp"
#include "deid/sha256.hpp"
#include "store/common.hpp"

namespace siaara::store {

namespace {

namespace fs = std::filesystem;
constexpr std::string_view kMagic = "SIAARAQUARANTINE1";
constexpr std::uint64_t kMaxRecordFile = std::uint64_t{64} * 1024;

bool id_ok(std::string_view id) noexcept {
    if (id.size() != 64) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string hex_of(const std::uint8_t* p, std::size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0xFu]);
    }
    return out;
}

std::string one_line(std::string_view v, std::size_t cap = 256) {
    std::string out(v.substr(0, cap));
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
    return out;
}

std::uint64_t parse_u64(std::string_view s) noexcept {
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || v > (UINT64_MAX - 9) / 10) return 0;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

std::string path_of(const std::string& dir, std::string_view id, std::string_view suffix) {
    return to_utf8(u8path(dir) / u8path(std::string(id) + std::string(suffix)));
}

std::string encode(const QuarantineRecord& r) {
    std::string t(kMagic);
    t += "\n";
    const auto put = [&](std::string_view k, std::string_view v) {
        t += k;
        t += "=";
        t += one_line(v);
        t += "\n";
    };
    put("time", std::to_string(r.time));
    put("source", r.source);
    put("calling_ae", r.calling_ae);
    put("peer", r.peer);
    put("sop_class_uid", r.sop_class_uid);
    put("sop_instance_uid", r.sop_instance_uid);
    put("size", std::to_string(r.size));
    put("read_loss", std::to_string(r.read_loss));
    put("warning", r.warning);
    return t;
}

bool decode(std::string_view text, QuarantineRecord& r) {
    if (text.substr(0, kMagic.size()) != kMagic) return false;
    std::size_t pos = text.find('\n');
    while (pos != std::string_view::npos && pos + 1 < text.size()) {
        const std::size_t nl = text.find('\n', pos + 1);
        const std::string_view line = text.substr(pos + 1, nl == std::string_view::npos ? std::string_view::npos : nl - pos - 1);
        pos = nl;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "time") r.time = static_cast<std::int64_t>(parse_u64(v));
        else if (k == "source") r.source = v;
        else if (k == "calling_ae") r.calling_ae = v;
        else if (k == "peer") r.peer = v;
        else if (k == "sop_class_uid") r.sop_class_uid = v;
        else if (k == "sop_instance_uid") r.sop_instance_uid = v;
        else if (k == "size") r.size = parse_u64(v);
        else if (k == "read_loss") r.read_loss = static_cast<std::uint32_t>(parse_u64(v));
        else if (k == "warning") r.warning = v;
    }
    return true;
}

}  // namespace

Quarantine::Quarantine(std::string store_root, const QuarantineLimits& limits, std::string_view area)
    : dir_(to_utf8(u8path(store_root) / u8path(std::string(area)))), lim_(limits) {}

namespace {

// Who an item counts for under the sender share: the calling AE, or the peer address without one.
const std::string& sender_of(const QuarantineRecord& r) noexcept { return r.calling_ae.empty() ? r.peer : r.calling_ae; }

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The modification time of a file as seconds since the Unix epoch (0 when unknown).
std::int64_t mtime_seconds(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    // file_time_type's epoch is not the system clock's everywhere: go through "now" on both.
    const auto sys = std::chrono::system_clock::now() +
                     std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

}  // namespace

Status Quarantine::load_locked() const {
    if (loaded_) return Status::ok();
    recs_.clear();
    bytes_ = 0;
    std::vector<std::string> bins;  // ids of every "<id>.bin" seen
    std::error_code ec;
    for (fs::directory_iterator it(u8path(dir_), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = to_utf8(it->path().filename());
        if (name.size() != 68 || !id_ok(std::string_view(name).substr(0, 64))) continue;
        std::error_code fe;
        if (name.compare(64, 4, ".bin") == 0) {
            if (it->is_regular_file(fe) && !fe) bins.push_back(name.substr(0, 64));
            continue;
        }
        if (name.compare(64, 4, ".rec") != 0) continue;
        if (!it->is_regular_file(fe) || it->file_size(fe) > kMaxRecordFile || fe) continue;
        auto text = io::read_file(to_utf8(it->path()));
        QuarantineRecord r;
        if (!text || !decode(text->as_chars(), r)) continue;
        r.id = name.substr(0, 64);
        // The record counts only with its bytes beside it.
        const std::uintmax_t size = fs::file_size(u8path(path_of(dir_, r.id, ".bin")), fe);
        if (fe) continue;
        r.size = size;
        bytes_ += r.size;
        recs_.push_back(std::move(r));
    }
    // R-166: bytes without a (readable) record - e.g. a crash between the two writes. They may
    // hold patient data: kept and listed, flagged, counted toward the caps; never deleted here.
    std::size_t orphans = 0;
    for (const std::string& id : bins) {
        if (std::any_of(recs_.begin(), recs_.end(), [&](const QuarantineRecord& r) { return r.id == id; })) continue;
        std::error_code fe;
        const fs::path bin = u8path(path_of(dir_, id, ".bin"));
        const std::uintmax_t size = fs::file_size(bin, fe);
        if (fe) continue;
        QuarantineRecord r;
        r.id = id;
        r.size = size;
        r.time = mtime_seconds(bin);
        r.record_missing = true;
        r.warning = "QUARANTINE_RECORD_MISSING";
        bytes_ += r.size;
        recs_.push_back(std::move(r));
        ++orphans;
    }
    loaded_ = true;
    if (orphans != 0 && lim_.note) {
        try {
            lim_.note("QUARANTINE_RECORD_MISSING: " + std::to_string(orphans) +
                      " item(s) in the area have bytes but no record; they are kept and listed as \"record missing\"");
        } catch (...) {  // NOLINT(bugprone-empty-catch): a note sink must never fail a load
        }
    }
    return Status::ok();
}

void Quarantine::evict_locked(std::vector<QuarantineRecord>::iterator it) {
    std::error_code ec;
    fs::remove(u8path(path_of(dir_, it->id, ".rec")), ec);
    fs::remove(u8path(path_of(dir_, it->id, ".bin")), ec);
    bytes_ -= std::min(bytes_, it->size);
    recs_.erase(it);
}

Result<QuarantineAdd> Quarantine::add(const Bytes& bytes, QuarantineRecord rec) {
    const QuarantinePart one{bytes.data(), bytes.size()};
    return add(&one, 1, std::move(rec));
}

Result<QuarantineAdd> Quarantine::add(const QuarantinePart* parts, std::size_t part_count, QuarantineRecord rec) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < part_count; ++i) {
        if (parts[i].size > UINT64_MAX - total) return Status(Errc::LimitExceeded, "quarantine is full");
        total += parts[i].size;
    }
    rec.size = total;
    rec.time = now_seconds();
    rec.record_missing = false;
    const std::string sender = sender_of(rec);
    const std::uint32_t share = std::min<std::uint32_t>(lim_.sender_share_percent, 100);
    // One sender's part of the caps (the whole of them without a share limit).
    const std::uint64_t sender_max_bytes = share == 0 ? lim_.max_bytes : lim_.max_bytes / 100 * share + lim_.max_bytes % 100 * share / 100;
    const std::size_t sender_max_items =
        share == 0 ? lim_.max_items : std::max<std::size_t>(1, lim_.max_items / 100 * share + lim_.max_items % 100 * share / 100);
    const auto sender_use = [&](std::size_t& items, std::uint64_t& bytes) {
        items = 0;
        bytes = 0;
        for (const QuarantineRecord& r : recs_)
            if (sender_of(r) == sender) {
                ++items;
                bytes += r.size;
            }
    };

    // 1. Under the lock: decide from the sizes alone, and reserve the room. Nothing is read or
    //    written before this says yes (R-159).
    {
        const std::lock_guard<std::mutex> l(mu_);
        if (rec.size > lim_.max_bytes || lim_.max_items == 0 || rec.size > sender_max_bytes)
            return Status(Errc::LimitExceeded, "quarantine is full");
        if (Status s = create_private_dirs(u8path(dir_)); !s) return s;
        if (Status s = load_locked(); !s) return s;
        if (!lim_.evict_oldest) {
            if (recs_.size() + pending_.items >= lim_.max_items || bytes_ + pending_.bytes > lim_.max_bytes - rec.size)
                return Status(Errc::LimitExceeded, "quarantine is full");
            if (share != 0) {
                std::size_t items = 0;
                std::uint64_t bytes = 0;
                sender_use(items, bytes);
                const auto p = pending_by_sender_.find(sender);
                if (p != pending_by_sender_.end()) {
                    items += p->second.items;
                    bytes += p->second.bytes;
                }
                if (items >= sender_max_items || bytes > sender_max_bytes - rec.size)
                    return Status(Errc::LimitExceeded, "quarantine: the sender's share of the area is full");
            }
        }
        if (lim_.min_free_disk_bytes != 0) {
            std::error_code ec;
            const fs::space_info si = fs::space(u8path(dir_), ec);
            // Room other adds have reserved and not yet written is theirs.
            if (!ec && (si.available < lim_.min_free_disk_bytes || si.available - lim_.min_free_disk_bytes < pending_.bytes ||
                        si.available - lim_.min_free_disk_bytes - pending_.bytes < rec.size))
                return Status(Errc::LimitExceeded, "quarantine: not enough free disk space");
        }
        ++pending_.items;
        pending_.bytes += rec.size;
        Pending& ps = pending_by_sender_[sender];
        ++ps.items;
        ps.bytes += rec.size;
    }
    bool reserved = true;
    const auto unreserve_locked = [&] {
        if (!reserved) return;
        reserved = false;
        --pending_.items;
        pending_.bytes -= rec.size;
        const auto p = pending_by_sender_.find(sender);
        if (p != pending_by_sender_.end()) {
            --p->second.items;
            p->second.bytes -= rec.size;
            if (p->second.items == 0) pending_by_sender_.erase(p);
        }
    };

    // 2. Outside the lock: stream the pieces to an owner-only temporary file, hashing as they go.
    //    3. (final_path, under the lock again) name the item by its hash, drop a duplicate, make
    //    room when the option says so. The lock is then held until the record is written.
    deid::Sha256 sha;
    std::unique_lock<std::mutex> lk(mu_, std::defer_lock);
    bool duplicate = false;
    bool adopt = false;  // the same bytes were there without their record (R-166): it is written now
    std::vector<io::Slice> slices;
    slices.reserve(part_count);
    for (std::size_t i = 0; i < part_count; ++i) slices.push_back(io::Slice{parts[i].data, parts[i].size});
    const Status written = io::write_parts_atomic(
        path_of(dir_, "item", ".bin"), slices.data(), slices.size(),
        [&sha](const std::uint8_t* p, std::size_t n) { sha.update(p, n); },
        [&]() -> Result<std::string> {
            const auto digest = sha.finish();
            rec.id = hex_of(digest.data(), digest.size());
            lk.lock();
            unreserve_locked();
            for (const QuarantineRecord& r : recs_)
                if (r.id == rec.id) {
                    duplicate = true;  // the same bytes: one item; the temporary file is removed
                    adopt = r.record_missing;
                    return Status(Errc::Rejected, "duplicate");
                }
            if (lim_.evict_oldest) {
                // Option "evict oldest": the oldest items make room - the sender's own while it is
                // over its share, then anyone's.
                const auto oldest = [&](bool own) {
                    auto best = recs_.end();
                    for (auto it = recs_.begin(); it != recs_.end(); ++it)
                        if ((!own || sender_of(*it) == sender) && (best == recs_.end() || it->time < best->time)) best = it;
                    return best;
                };
                for (;;) {
                    std::size_t items = 0;
                    std::uint64_t bytes = 0;
                    sender_use(items, bytes);
                    const bool own_full = share != 0 && (items >= sender_max_items || bytes > sender_max_bytes - rec.size);
                    const bool full = recs_.size() >= lim_.max_items || bytes_ > lim_.max_bytes - rec.size;
                    if (!own_full && !full) break;
                    const auto victim = oldest(own_full);
                    if (victim == recs_.end()) return Status(Errc::LimitExceeded, "quarantine is full");
                    evict_locked(victim);
                }
            }
            return path_of(dir_, rec.id, ".bin");
        });
    if (!lk.owns_lock()) lk.lock();
    unreserve_locked();
    if (duplicate) {
        if (adopt) {
            const std::string text = encode(rec);
            const auto it = std::find_if(recs_.begin(), recs_.end(), [&](const QuarantineRecord& r) { return r.id == rec.id; });
            if (it != recs_.end() &&
                io::write_file_atomic(path_of(dir_, rec.id, ".rec"), reinterpret_cast<const std::uint8_t*>(text.data()), text.size())) {
                rec.size = it->size;
                *it = std::move(rec);
            }
        }
        return QuarantineAdd::Duplicate;
    }
    if (!written) return written;
    // Bytes first, then the record: a ".bin" without its record is listed as "record missing".
    const std::string text = encode(rec);
    if (Status s = io::write_file_atomic(path_of(dir_, rec.id, ".rec"), reinterpret_cast<const std::uint8_t*>(text.data()),
                                         text.size());
        !s) {
        std::error_code ec;
        fs::remove(u8path(path_of(dir_, rec.id, ".bin")), ec);
        return s;
    }
    bytes_ += rec.size;
    recs_.push_back(std::move(rec));
    return QuarantineAdd::Added;
}

Result<std::vector<QuarantineRecord>> Quarantine::list() const {
    const std::lock_guard<std::mutex> l(mu_);
    if (Status s = load_locked(); !s) return s;
    std::vector<QuarantineRecord> out = recs_;
    std::stable_sort(out.begin(), out.end(), [](const QuarantineRecord& a, const QuarantineRecord& b) { return a.time > b.time; });
    return out;
}

Result<QuarantineRecord> Quarantine::record(std::string_view id) const {
    const std::lock_guard<std::mutex> l(mu_);
    if (Status s = load_locked(); !s) return s;
    for (const QuarantineRecord& r : recs_)
        if (r.id == id) return r;
    return Status(Errc::NotFound, "no such quarantine item");
}

Result<Bytes> Quarantine::bytes(std::string_view id) const {
    if (!id_ok(id)) return Status(Errc::NotFound, "no such quarantine item");
    {
        const std::lock_guard<std::mutex> l(mu_);
        if (Status s = load_locked(); !s) return s;
        const bool known = std::any_of(recs_.begin(), recs_.end(), [&](const QuarantineRecord& r) { return r.id == id; });
        if (!known) return Status(Errc::NotFound, "no such quarantine item");
    }
    // The area's own file (owner-only, only ever replaced by rename): mapped, never copied.
    return io::map_file(path_of(dir_, id, ".bin"));
}

Status Quarantine::remove(std::string_view id) {
    if (!id_ok(id)) return Status(Errc::NotFound, "no such quarantine item");
    const std::lock_guard<std::mutex> l(mu_);
    if (Status s = load_locked(); !s) return s;
    const auto it = std::find_if(recs_.begin(), recs_.end(), [&](const QuarantineRecord& r) { return r.id == id; });
    if (it == recs_.end()) return Status(Errc::NotFound, "no such quarantine item");
    std::error_code ec;
    fs::remove(u8path(path_of(dir_, id, ".rec")), ec);  // the record first
    const fs::path bin = u8path(path_of(dir_, id, ".bin"));
    fs::remove(bin, ec);
    std::error_code ee;
    if (ec && fs::exists(bin, ee)) {
        // The bytes could not be removed (e.g. still mapped by a reader on Windows): the item
        // stays, now without its record, and the caller is told.
        it->record_missing = true;
        return Status(Errc::IoError, "the quarantine item's bytes could not be removed");
    }
    bytes_ -= std::min(bytes_, it->size);
    recs_.erase(it);
    return Status::ok();
}

std::size_t Quarantine::items() const {
    const std::lock_guard<std::mutex> l(mu_);
    (void)load_locked();
    return recs_.size();
}

std::uint64_t Quarantine::total_bytes() const {
    const std::lock_guard<std::mutex> l(mu_);
    (void)load_locked();
    return bytes_;
}

}  // namespace siaara::store
