// SIAARA DICOM Toolkit - quarantine area of a store (store/quarantine.hpp).
#include "store/quarantine.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
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

Status Quarantine::load_locked() const {
    if (loaded_) return Status::ok();
    recs_.clear();
    bytes_ = 0;
    std::error_code ec;
    for (fs::directory_iterator it(u8path(dir_), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = to_utf8(it->path().filename());
        if (name.size() != 68 || name.compare(64, 4, ".rec") != 0 || !id_ok(std::string_view(name).substr(0, 64))) continue;
        std::error_code fe;
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
    loaded_ = true;
    return Status::ok();
}

Result<QuarantineAdd> Quarantine::add(const Bytes& bytes, QuarantineRecord rec) {
    const auto digest = deid::Sha256::hash(bytes.data(), bytes.size());
    rec.id = hex_of(digest.data(), digest.size());
    rec.size = bytes.size();
    rec.time = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::lock_guard<std::mutex> l(mu_);
    if (Status s = create_private_dirs(u8path(dir_)); !s) return s;
    if (Status s = load_locked(); !s) return s;
    for (const QuarantineRecord& r : recs_)
        if (r.id == rec.id) return QuarantineAdd::Duplicate;  // the same bytes: one item
    const auto full = [&] { return recs_.size() >= lim_.max_items || bytes_ > lim_.max_bytes - std::min(rec.size, lim_.max_bytes); };
    if (rec.size > lim_.max_bytes || lim_.max_items == 0) return Status(Errc::LimitExceeded, "quarantine is full");
    while (full() && lim_.evict_oldest && !recs_.empty()) {
        // Option "evict oldest": the oldest item makes room.
        const auto oldest = std::min_element(recs_.begin(), recs_.end(),
                                             [](const QuarantineRecord& a, const QuarantineRecord& b) { return a.time < b.time; });
        std::error_code ec;
        fs::remove(u8path(path_of(dir_, oldest->id, ".rec")), ec);
        fs::remove(u8path(path_of(dir_, oldest->id, ".bin")), ec);
        bytes_ -= std::min(bytes_, oldest->size);
        recs_.erase(oldest);
    }
    if (full()) return Status(Errc::LimitExceeded, "quarantine is full");
    // Bytes first, then the record: an item is listed only once both are in place.
    if (Status s = io::write_file_atomic(path_of(dir_, rec.id, ".bin"), bytes.data(), bytes.size()); !s) return s;
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
    return io::read_file(path_of(dir_, id, ".bin"));
}

Status Quarantine::remove(std::string_view id) {
    if (!id_ok(id)) return Status(Errc::NotFound, "no such quarantine item");
    const std::lock_guard<std::mutex> l(mu_);
    if (Status s = load_locked(); !s) return s;
    const auto it = std::find_if(recs_.begin(), recs_.end(), [&](const QuarantineRecord& r) { return r.id == id; });
    if (it == recs_.end()) return Status(Errc::NotFound, "no such quarantine item");
    std::error_code ec;
    fs::remove(u8path(path_of(dir_, id, ".rec")), ec);  // the record first: the item is gone from the list
    fs::remove(u8path(path_of(dir_, id, ".bin")), ec);
    bytes_ -= std::min(bytes_, it->size);
    recs_.erase(it);
    return Status::ok();
}

bool Quarantine::holds_instance(std::string_view sop_instance_uid) const {
    const std::lock_guard<std::mutex> l(mu_);
    if (!load_locked()) return false;
    return std::any_of(recs_.begin(), recs_.end(),
                       [&](const QuarantineRecord& r) { return r.sop_instance_uid == sop_instance_uid; });
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
