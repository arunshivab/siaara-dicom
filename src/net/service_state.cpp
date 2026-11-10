// SIAARA DICOM Toolkit - the bounded, optionally persistent models behind the MPPS and UPS SCPs
// (net/services.hpp): MppsStore (PS3.4 F.7) and UpsProvider (PS3.4 Annex CC).
//
// Bounds (R-109, R-119): an item cap for instances that are still open, a cap for finished ones
// (the oldest are evicted first) and a byte budget over everything held. Beyond them a request is
// answered 0xA700 and nothing changes.
//
// Persistence (optional): with a state folder every instance lives in one owner-only file
// "<SOP Instance UID>.sst", written to a temporary file and renamed into place
// (io::write_file_atomic), so a crash leaves either the old or the new state. The file is a short
// text header ("SIAARASTATE1", then name=value lines, then an empty line) followed by the data
// set in Explicit VR Little Endian. Files that cannot be read are skipped and counted, never
// deleted.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "net/services.hpp"
#include "store/common.hpp"

namespace siaara::net {

namespace {

namespace fs = std::filesystem;

constexpr Tag kTransactionUid = 0x00081195;
constexpr Tag kPpsStatus = 0x00400252;
constexpr Tag kInputReadinessState = 0x00404041;
constexpr Tag kProcedureStepState = 0x00741000;
constexpr Tag kRequestingAe = 0x00741236;

constexpr std::uint16_t kNotAuthorized = 0x0124;
constexpr std::string_view kMagic = "SIAARASTATE1";
constexpr std::string_view kSuffix = ".sst";
constexpr std::string_view kGlobalsName = "_global.sst";
constexpr std::uint64_t kMaxStateFile = 64ull << 20;

std::string str_of(const Dataset& ds, Tag t) { return ds.get_string(t).value_or(std::string()); }

// A UID usable as a file name: digits and dots, 1..64 characters, no leading or trailing dot.
bool uid_file_safe(std::string_view s) noexcept {
    if (s.empty() || s.size() > 64 || s.front() == '.' || s.back() == '.') return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

// Header values are single lines of printable characters.
std::string one_line(std::string_view v) {
    std::string out(v);
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
    return out;
}

std::string join(const std::set<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += '\\';
        out += one_line(s);
    }
    return out;
}

std::set<std::string> split(std::string_view v) {
    std::set<std::string> out;
    while (!v.empty()) {
        const std::size_t p = v.find('\\');
        const std::string_view one = v.substr(0, p);
        if (!one.empty()) out.emplace(one);
        if (p == std::string_view::npos) break;
        v.remove_prefix(p + 1);
    }
    return out;
}

using Fields = std::vector<std::pair<std::string, std::string>>;

Status write_state(const std::string& folder, std::string_view name, const Fields& fields, const Dataset* ds) {
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    out.push_back('\n');
    for (const auto& [k, v] : fields) {
        const std::string line = k + "=" + one_line(v) + "\n";
        out.insert(out.end(), line.begin(), line.end());
    }
    out.push_back('\n');
    if (ds) {
        std::vector<std::uint8_t> body;
        if (Status s = write_dataset(*ds, ts::ExplicitVRLittleEndian, body); !s) return s;
        out.insert(out.end(), body.begin(), body.end());
    }
    const fs::path p = store::u8path(folder) / store::u8path(std::string(name));
    return io::write_file_atomic(store::to_utf8(p), out.data(), out.size(), io::WriteMode::Replace);
}

struct LoadedState {
    Fields fields;
    Dataset ds;
    const std::string* get(std::string_view key) const noexcept {
        for (const auto& [k, v] : fields)
            if (k == key) return &v;
        return nullptr;
    }
    std::string value(std::string_view key) const {
        const std::string* v = get(key);
        return v ? *v : std::string();
    }
};

Result<LoadedState> read_state(const std::string& utf8_path, bool with_dataset) {
    Result<Bytes> b = io::read_file(utf8_path);
    if (!b) return b.take_error();
    const std::string_view all = b->as_chars();
    if (all.size() < kMagic.size() + 1 || all.substr(0, kMagic.size()) != kMagic || all[kMagic.size()] != '\n')
        return Status(Errc::Malformed, "not a state file");
    std::size_t pos = kMagic.size() + 1;
    LoadedState out;
    for (;;) {
        const std::size_t nl = all.find('\n', pos);
        if (nl == std::string_view::npos || out.fields.size() > 64) return Status(Errc::Malformed, "state file header");
        const std::string_view line = all.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) break;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) return Status(Errc::Malformed, "state file header");
        out.fields.emplace_back(std::string(line.substr(0, eq)), std::string(line.substr(eq + 1)));
    }
    if (with_dataset) {
        ReadOptions ro;
        ro.lenient = false;
        Result<Dataset> ds = read_dataset(b->sub(pos, all.size() - pos), ts::ExplicitVRLittleEndian, ro);
        if (!ds) return ds.take_error();
        // The values view the file's bytes, which `b` keeps alive through the views.
        out.ds = std::move(ds.value());
    }
    return out;
}

// The state files of a folder: (UID or name without the suffix, UTF-8 path).
std::vector<std::pair<std::string, std::string>> state_files(const std::string& folder, std::size_t& skipped) {
    std::vector<std::pair<std::string, std::string>> out;
    std::error_code ec;
    for (fs::directory_iterator it(store::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = store::to_utf8(it->path().filename());
        if (name.size() <= kSuffix.size() || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        std::error_code fe;
        if (!it->is_regular_file(fe) || it->is_symlink(fe)) continue;
        const std::uintmax_t size = it->file_size(fe);
        if (fe || size > kMaxStateFile) {
            ++skipped;
            continue;
        }
        out.emplace_back(name.substr(0, name.size() - kSuffix.size()), store::to_utf8(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

void remove_state(const std::string& folder, const std::string& uid) {
    if (folder.empty() || !uid_file_safe(uid)) return;
    std::error_code ec;
    fs::remove(store::u8path(folder) / store::u8path(uid + std::string(kSuffix)), ec);
}

std::uint64_t parse_u64(const std::string& s) noexcept {
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || v > (UINT64_MAX - 9) / 10) return 0;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

bool mpps_finished(const Dataset& ds) { return str_of(ds, kPpsStatus) != "IN PROGRESS"; }
bool ups_final(std::string_view state) { return state == ups_state::Completed || state == ups_state::Canceled; }

}  // namespace

std::uint64_t approximate_size(const Dataset& ds) noexcept {
    std::uint64_t n = 32;
    for (const Element& e : ds.elements()) {
        n += 24 + e.value.size();
        for (const Dataset& item : e.items) n += approximate_size(item);
    }
    return n;
}

// ===================================================================== MPPS

Status MppsStore::set_state_folder(const std::string& folder) {
    if (folder.empty()) return Status(Errc::InvalidArgument, "empty state folder");
    if (Status s = store::create_private_dirs(store::u8path(folder)); !s) return s;
    std::lock_guard<std::mutex> l(mu_);
    folder_ = folder;
    std::uint64_t max_seq = 0;
    for (const auto& [uid, path] : state_files(folder, skipped_)) {
        if (!uid_file_safe(uid)) continue;
        Result<LoadedState> st = read_state(path, true);
        if (!st || st->value("kind") != "mpps") {
            ++skipped_;
            continue;
        }
        Item it;
        it.ds = std::move(st->ds);
        it.creator = st->value("creator");
        it.bytes = approximate_size(it.ds);
        it.finished_seq = parse_u64(st->value("seq"));
        if (mpps_finished(it.ds) && it.finished_seq == 0) it.finished_seq = 1;
        if (!mpps_finished(it.ds)) it.finished_seq = 0;
        if (items_.count(uid)) continue;
        if (bytes_ + it.bytes > lim_.max_bytes) {
            ++skipped_;
            continue;
        }
        if (it.finished_seq) {
            while (finished_.count(it.finished_seq)) ++it.finished_seq;
            finished_[it.finished_seq] = uid;
            max_seq = std::max(max_seq, it.finished_seq);
        } else {
            ++in_progress_;
        }
        bytes_ += it.bytes;
        items_.emplace(uid, std::move(it));
    }
    next_seq_ = std::max(next_seq_, max_seq + 1);
    evict_finished_over_cap();
    return Status::ok();
}

Status MppsStore::save(const std::string& uid, const Item& it) const {
    if (folder_.empty()) return Status::ok();
    const Fields f = {{"kind", "mpps"}, {"creator", it.creator}, {"seq", std::to_string(it.finished_seq)}};
    return write_state(folder_, uid + std::string(kSuffix), f, &it.ds);
}

void MppsStore::erase_file(const std::string& uid) const { remove_state(folder_, uid); }

void MppsStore::evict_finished_over_cap() {
    while (finished_.size() > lim_.max_finished) {
        const auto oldest = finished_.begin();
        const auto it = items_.find(oldest->second);
        if (it != items_.end()) {
            bytes_ -= it->second.bytes;
            erase_file(it->first);
            items_.erase(it);
        }
        finished_.erase(oldest);
    }
}

// Evicts finished instances (oldest first, never `keep`) until `need` more bytes fit.
bool MppsStore::make_room(std::uint64_t need, const std::string& keep) {
    if (need > lim_.max_bytes) return false;
    auto f = finished_.begin();
    while (bytes_ + need > lim_.max_bytes && f != finished_.end()) {
        if (f->second == keep) {
            ++f;
            continue;
        }
        const auto it = items_.find(f->second);
        if (it != items_.end()) {
            bytes_ -= it->second.bytes;
            erase_file(it->first);
            items_.erase(it);
        }
        f = finished_.erase(f);
    }
    return bytes_ + need <= lim_.max_bytes;
}

std::uint16_t MppsStore::create(const std::string& uid, const Dataset& attributes, const std::string& calling_ae) {
    if (uid.empty()) return dimse_status::MissingAttribute;
    if (attributes.get_string(kPpsStatus).value_or("") != "IN PROGRESS") return dimse_status::InvalidAttributeValue;
    std::lock_guard<std::mutex> l(mu_);
    if (!folder_.empty() && !uid_file_safe(uid)) return dimse_status::InvalidAttributeValue;
    if (items_.count(uid)) return dimse_status::DuplicateSopInstance;
    if (in_progress_ >= lim_.max_items) return dimse_status::OutOfResources;
    Item it;
    it.ds = attributes;
    it.creator = calling_ae;
    it.bytes = approximate_size(it.ds);
    if (!make_room(it.bytes, uid)) return dimse_status::OutOfResources;
    if (Status s = save(uid, it); !s) return dimse_status::ProcessingFailure;
    bytes_ += it.bytes;
    ++in_progress_;
    items_.emplace(uid, std::move(it));
    return dimse_status::Success;
}

std::uint16_t MppsStore::set(const std::string& uid, const Dataset& modifications, const std::string& calling_ae) {
    std::lock_guard<std::mutex> l(mu_);
    auto found = items_.find(uid);
    if (found == items_.end()) return dimse_status::NoSuchSopInstance;
    Item& it = found->second;
    if (lim_.bind_to_creator && it.creator != calling_ae) return kNotAuthorized;
    // PS3.4 F.7.2.2.2: once COMPLETED or DISCONTINUED the instance may no longer be updated.
    if (it.finished_seq != 0 || mpps_finished(it.ds)) return dimse_status::ProcessingFailure;
    if (auto st = modifications.get_string(kPpsStatus))
        if (*st != "IN PROGRESS" && *st != "COMPLETED" && *st != "DISCONTINUED") return dimse_status::InvalidAttributeValue;
    Item next;
    next.ds = it.ds;
    next.creator = it.creator;
    for (const Element& e : modifications.elements()) next.ds.put(e);
    next.bytes = approximate_size(next.ds);
    if (next.bytes > it.bytes && !make_room(next.bytes - it.bytes, uid)) return dimse_status::OutOfResources;
    const bool finishing = mpps_finished(next.ds);
    if (finishing) next.finished_seq = next_seq_;
    if (Status s = save(uid, next); !s) return dimse_status::ProcessingFailure;
    bytes_ = bytes_ - it.bytes + next.bytes;
    it = std::move(next);
    if (finishing) {
        finished_[next_seq_++] = uid;
        --in_progress_;
        evict_finished_over_cap();
    }
    return dimse_status::Success;
}

std::optional<Dataset> MppsStore::get(const std::string& uid) const {
    std::lock_guard<std::mutex> l(mu_);
    auto it = items_.find(uid);
    if (it == items_.end()) return std::nullopt;
    return it->second.ds;
}

std::size_t MppsStore::size() const {
    std::lock_guard<std::mutex> l(mu_);
    return items_.size();
}

std::size_t MppsStore::in_progress() const {
    std::lock_guard<std::mutex> l(mu_);
    return in_progress_;
}

std::uint64_t MppsStore::bytes() const {
    std::lock_guard<std::mutex> l(mu_);
    return bytes_;
}

std::size_t MppsStore::skipped_files() const {
    std::lock_guard<std::mutex> l(mu_);
    return skipped_;
}

// ===================================================================== UPS provider

Status UpsProvider::set_state_folder(const std::string& folder) {
    if (folder.empty()) return Status(Errc::InvalidArgument, "empty state folder");
    if (Status s = store::create_private_dirs(store::u8path(folder)); !s) return s;
    std::lock_guard<std::mutex> l(mu_);
    folder_ = folder;
    std::uint64_t max_seq = 0;
    for (const auto& [uid, path] : state_files(folder, skipped_)) {
        if (uid + std::string(kSuffix) == kGlobalsName) {
            Result<LoadedState> st = read_state(path, false);
            if (!st || st->value("kind") != "ups-global") {
                ++skipped_;
                continue;
            }
            global_ = split(st->value("subs"));
            suspended_ = split(st->value("suspended"));
            while (global_.size() > lim_.max_global_subscribers) global_.erase(std::prev(global_.end()));
            continue;
        }
        if (!uid_file_safe(uid)) continue;
        Result<LoadedState> st = read_state(path, true);
        if (!st || st->value("kind") != "ups") {
            ++skipped_;
            continue;
        }
        Item it;
        it.ds = std::move(st->ds);
        it.state = st->value("state");
        it.transaction_uid = st->value("txn");
        it.creator = st->value("creator");
        it.subscribers = split(st->value("subs"));
        it.locks = split(st->value("locks"));
        it.bytes = approximate_size(it.ds);
        it.finished_seq = parse_u64(st->value("seq"));
        const bool known = it.state == ups_state::Scheduled || it.state == ups_state::InProgress || ups_final(it.state);
        if (!known || items_.count(uid) || bytes_ + it.bytes > lim_.max_bytes) {
            ++skipped_;
            continue;
        }
        if (ups_final(it.state)) {
            if (it.finished_seq == 0) it.finished_seq = 1;
            while (finished_.count(it.finished_seq)) ++it.finished_seq;
            finished_[it.finished_seq] = uid;
            max_seq = std::max(max_seq, it.finished_seq);
        } else {
            it.finished_seq = 0;
            ++open_items_;
        }
        bytes_ += it.bytes;
        items_.emplace(uid, std::move(it));
    }
    next_seq_ = std::max(next_seq_, max_seq + 1);
    evict_finished_over_cap();
    return Status::ok();
}

Status UpsProvider::save(const std::string& uid, const Item& it) const {
    if (folder_.empty()) return Status::ok();
    const Fields f = {{"kind", "ups"},          {"state", it.state},         {"txn", it.transaction_uid},
                      {"creator", it.creator},  {"subs", join(it.subscribers)}, {"locks", join(it.locks)},
                      {"seq", std::to_string(it.finished_seq)}};
    return write_state(folder_, uid + std::string(kSuffix), f, &it.ds);
}

void UpsProvider::save_globals() const {
    if (folder_.empty()) return;
    const Fields f = {{"kind", "ups-global"}, {"subs", join(global_)}, {"suspended", join(suspended_)}};
    (void)write_state(folder_, kGlobalsName, f, nullptr);
}

void UpsProvider::erase_file(const std::string& uid) const { remove_state(folder_, uid); }

std::set<std::string> UpsProvider::watchers(const Item& it) const { return it.subscribers; }

void UpsProvider::queue_event(const std::string& ae, UpsEventReport ev) {
    if (events_.size() >= lim_.max_pending_events) return;  // nobody is taking them: bounded
    events_.emplace_back(ae, std::move(ev));
}

void UpsProvider::queue_state_report(const std::string& uid, const Item& it, const std::set<std::string>& to) {
    for (const std::string& ae : to) {
        UpsEventReport ev;
        ev.ups_uid = uid;
        ev.event_type = 1;
        (void)ev.info.set_string(kProcedureStepState, VR::CS, it.state);
        (void)ev.info.set_string(kInputReadinessState, VR::CS, it.ds.get_string(kInputReadinessState).value_or("READY"));
        queue_event(ae, std::move(ev));
    }
}

void UpsProvider::evict_finished_over_cap() {
    auto f = finished_.begin();
    std::size_t excess = finished_.size() > lim_.max_finished ? finished_.size() - lim_.max_finished : 0;
    while (excess > 0 && f != finished_.end()) {
        const auto it = items_.find(f->second);
        if (it != items_.end() && !it->second.locks.empty()) {
            ++f;  // a deletion lock keeps it (PS3.4 CC.2.3.1)
            continue;
        }
        if (it != items_.end()) {
            bytes_ -= it->second.bytes;
            erase_file(it->first);
            items_.erase(it);
        }
        f = finished_.erase(f);
        --excess;
    }
}

bool UpsProvider::make_room(std::uint64_t need, const std::string& keep) {
    if (need > lim_.max_bytes) return false;
    auto f = finished_.begin();
    while (bytes_ + need > lim_.max_bytes && f != finished_.end()) {
        const auto it = items_.find(f->second);
        if (f->second == keep || (it != items_.end() && !it->second.locks.empty())) {
            ++f;
            continue;
        }
        if (it != items_.end()) {
            bytes_ -= it->second.bytes;
            erase_file(it->first);
            items_.erase(it);
        }
        f = finished_.erase(f);
    }
    return bytes_ + need <= lim_.max_bytes;
}

// The workitem reached a final state: it no longer counts as open and becomes evictable.
void UpsProvider::finish(const std::string& uid, Item& it) {
    if (it.finished_seq != 0) return;
    it.finished_seq = next_seq_++;
    finished_[it.finished_seq] = uid;
    if (open_items_ > 0) --open_items_;
}

std::uint16_t UpsProvider::create(const std::string& uid, const Dataset& attributes, const std::string& calling_ae) {
    if (uid.empty()) return dimse_status::MissingAttribute;
    const std::optional<std::string> st = attributes.get_string(kProcedureStepState);
    if (!st || *st != ups_state::Scheduled) return dimse_status::UpsCreateStateNotScheduled;
    std::lock_guard<std::mutex> l(mu_);
    if (!folder_.empty() && !uid_file_safe(uid)) return dimse_status::InvalidAttributeValue;
    if (items_.count(uid)) return dimse_status::DuplicateSopInstance;
    if (open_items_ >= lim_.max_items) return dimse_status::OutOfResources;
    Item it;
    it.ds = attributes;
    (void)it.ds.remove(kTransactionUid);
    (void)it.ds.set_string(tags::SOPClassUID, VR::UI, sop::UpsPush);
    (void)it.ds.set_string(tags::SOPInstanceUID, VR::UI, uid);
    it.state = std::string(ups_state::Scheduled);
    it.creator = calling_ae;
    it.bytes = approximate_size(it.ds);
    if (!make_room(it.bytes, uid)) return dimse_status::OutOfResources;
    // Finished workitems kept only by deletion locks still bound the total.
    if (items_.size() >= lim_.max_items + lim_.max_finished) return dimse_status::OutOfResources;
    for (const std::string& ae : global_) it.subscribers.insert(ae);
    if (Status s = save(uid, it); !s) return dimse_status::ProcessingFailure;
    queue_state_report(uid, it, it.subscribers);
    bytes_ += it.bytes;
    ++open_items_;
    items_.emplace(uid, std::move(it));
    return dimse_status::Success;
}

std::uint16_t UpsProvider::set(const std::string& uid, const Dataset& modifications, const std::string& calling_ae) {
    std::lock_guard<std::mutex> l(mu_);
    auto f = items_.find(uid);
    if (f == items_.end()) return dimse_status::UpsNoSuchInstance;
    Item& it = f->second;
    if (ups_final(it.state)) return dimse_status::UpsMayNoLongerBeUpdated;
    if (it.state == ups_state::InProgress && str_of(modifications, kTransactionUid) != it.transaction_uid)
        return dimse_status::UpsWrongTransactionUid;
    if (lim_.bind_to_creator && it.state == ups_state::Scheduled && it.creator != calling_ae) return kNotAuthorized;
    Dataset next = it.ds;
    for (const Element& e : modifications.elements()) {
        if (e.tag == kTransactionUid || e.tag == kProcedureStepState || e.tag == tags::SOPInstanceUID ||
            e.tag == tags::SOPClassUID)
            continue;
        next.put(e);
    }
    const std::uint64_t size = approximate_size(next);
    if (size > it.bytes && !make_room(size - it.bytes, uid)) return dimse_status::OutOfResources;
    Item saved = it;
    saved.ds = std::move(next);
    saved.bytes = size;
    if (Status s = save(uid, saved); !s) return dimse_status::ProcessingFailure;
    bytes_ = bytes_ - it.bytes + size;
    it = std::move(saved);
    return dimse_status::Success;
}

std::uint16_t UpsProvider::get(const std::string& uid, const std::optional<std::vector<Tag>>& attributes,
                               Dataset& out) const {
    std::lock_guard<std::mutex> l(mu_);
    auto f = items_.find(uid);
    if (f == items_.end()) return dimse_status::UpsNoSuchInstance;
    Dataset all = f->second.ds;
    (void)all.set_string(kProcedureStepState, VR::CS, f->second.state);
    out = Dataset{};
    if (!attributes || attributes->empty()) {
        out = std::move(all);
    } else {
        for (Tag t : *attributes)
            if (const Element* e = all.find(t)) out.put(*e);
    }
    (void)out.remove(kTransactionUid);  // never returned (PS3.4 CC.2.7)
    return dimse_status::Success;
}

std::uint16_t UpsProvider::change_state(const std::string& uid, std::string_view state,
                                        const std::string& transaction_uid, const std::string& calling_ae) {
    (void)calling_ae;  // the Transaction UID is what authorises a state change (PS3.4 CC.2.1)
    std::lock_guard<std::mutex> l(mu_);
    auto f = items_.find(uid);
    if (f == items_.end()) return dimse_status::UpsNoSuchInstance;
    Item& it = f->second;
    const std::string& cur = it.state;
    std::string txn = it.transaction_uid;
    if (state == ups_state::Scheduled) return dimse_status::UpsScheduledOnlyByCreate;
    if (state == ups_state::InProgress) {
        if (cur == ups_state::InProgress) return dimse_status::UpsAlreadyInProgress;
        if (cur != ups_state::Scheduled) return dimse_status::UpsMayNoLongerBeUpdated;
        if (transaction_uid.empty()) return dimse_status::UpsWrongTransactionUid;
        txn = transaction_uid;
    } else if (state == ups_state::Completed || state == ups_state::Canceled) {
        if (cur == state) return state == ups_state::Completed ? dimse_status::UpsAlreadyCompleted : dimse_status::UpsAlreadyCanceled;
        if (cur == ups_state::Scheduled) return dimse_status::UpsNotInProgress;
        if (cur != ups_state::InProgress) return dimse_status::UpsMayNoLongerBeUpdated;
        if (transaction_uid != it.transaction_uid) return dimse_status::UpsWrongTransactionUid;
    } else {
        return dimse_status::InvalidAttributeValue;
    }
    Item next = it;
    next.transaction_uid = txn;
    next.state = std::string(state);
    (void)next.ds.set_string(kProcedureStepState, VR::CS, next.state);
    const bool final_now = ups_final(next.state);
    if (final_now) next.finished_seq = next_seq_;
    if (Status s = save(uid, next); !s) return dimse_status::ProcessingFailure;
    next.finished_seq = 0;
    it = std::move(next);
    if (final_now) finish(uid, it);
    queue_state_report(uid, it, watchers(it));
    if (final_now) evict_finished_over_cap();
    return dimse_status::Success;
}

std::uint16_t UpsProvider::request_cancel(const std::string& uid, const std::string& requesting_ae, const Dataset& info) {
    std::lock_guard<std::mutex> l(mu_);
    auto f = items_.find(uid);
    if (f == items_.end()) return dimse_status::UpsNoSuchInstance;
    Item& it = f->second;
    if (it.state == ups_state::Completed) return dimse_status::UpsCancelAlreadyCompleted;
    if (it.state == ups_state::Canceled) return dimse_status::UpsAlreadyCanceled;
    if (it.state == ups_state::Scheduled) {
        Item next = it;
        next.state = std::string(ups_state::Canceled);
        (void)next.ds.set_string(kProcedureStepState, VR::CS, next.state);
        next.finished_seq = next_seq_;
        if (Status s = save(uid, next); !s) return dimse_status::ProcessingFailure;
        next.finished_seq = 0;
        it = std::move(next);
        finish(uid, it);
        queue_state_report(uid, it, watchers(it));
        evict_finished_over_cap();
        return dimse_status::Success;
    }
    if (performer_accepts_cancel && !performer_accepts_cancel(uid)) return dimse_status::UpsPerformerChoosesNotToCancel;
    for (const std::string& ae : watchers(it)) {
        UpsEventReport ev;
        ev.ups_uid = uid;
        ev.event_type = 2;
        ev.info = info;
        (void)ev.info.set_string(kRequestingAe, VR::AE, requesting_ae);
        queue_event(ae, std::move(ev));
    }
    return dimse_status::Success;
}

std::uint16_t UpsProvider::subscribe(const std::string& uid, const std::string& ae, bool deletion_lock) {
    std::lock_guard<std::mutex> l(mu_);
    if (uid == sop::UpsGlobalSubscriptionInstance || uid == sop::UpsFilteredGlobalSubscriptionInstance) {
        if (!global_.count(ae) && global_.size() >= lim_.max_global_subscribers) return dimse_status::OutOfResources;
        global_.insert(ae);
        suspended_.erase(ae);
        save_globals();
        for (auto& [id, it] : items_) {
            if (!it.subscribers.count(ae) && it.subscribers.size() >= lim_.max_subscribers_per_item) continue;
            it.subscribers.insert(ae);
            if (deletion_lock) it.locks.insert(ae);
            (void)save(id, it);
            queue_state_report(id, it, {ae});
        }
        return dimse_status::Success;
    }
    auto f = items_.find(uid);
    if (f == items_.end()) return dimse_status::UpsNoSuchInstance;
    Item& it = f->second;
    // R-117: a bounded number of receiving AEs per workitem.
    if (!it.subscribers.count(ae) && it.subscribers.size() >= lim_.max_subscribers_per_item)
        return dimse_status::OutOfResources;
    it.subscribers.insert(ae);
    if (deletion_lock) it.locks.insert(ae);
    else it.locks.erase(ae);
    (void)save(uid, it);
    queue_state_report(uid, it, {ae});
    return dimse_status::Success;
}

std::uint16_t UpsProvider::unsubscribe(const std::string& uid, const std::string& ae) {
    std::lock_guard<std::mutex> l(mu_);
    if (uid == sop::UpsGlobalSubscriptionInstance || uid == sop::UpsFilteredGlobalSubscriptionInstance) {
        global_.erase(ae);
        save_globals();
        for (auto& [id, it] : items_) {
            if (it.subscribers.erase(ae) + it.locks.erase(ae) != 0) (void)save(id, it);
        }
        evict_finished_over_cap();
        return dimse_status::Success;
    }
    auto f = items_.find(uid);
    if (f == items_.end()) return dimse_status::UpsNoSuchInstance;
    f->second.subscribers.erase(ae);
    f->second.locks.erase(ae);
    (void)save(uid, f->second);
    evict_finished_over_cap();
    return dimse_status::Success;
}

std::uint16_t UpsProvider::suspend_global(const std::string& ae) {
    std::lock_guard<std::mutex> l(mu_);
    global_.erase(ae);
    if (suspended_.size() < lim_.max_global_subscribers) suspended_.insert(ae);
    save_globals();
    return dimse_status::Success;
}

std::vector<Dataset> UpsProvider::find(const Dataset& identifier, const MatchOptions& opt, std::size_t max_results,
                                       bool* more) const {
    if (more) *more = false;
    std::lock_guard<std::mutex> l(mu_);
    std::vector<Dataset> out;
    for (const auto& [id, it] : items_) {
        Dataset cand = it.ds;
        (void)cand.set_string(kProcedureStepState, VR::CS, it.state);
        (void)cand.remove(kTransactionUid);
        if (!query_matches(identifier, cand, opt)) continue;
        if (max_results != 0 && out.size() >= max_results) {
            if (more) *more = true;
            break;
        }
        out.push_back(query_response(identifier, cand));
    }
    return out;
}

std::optional<std::string> UpsProvider::state(const std::string& uid) const {
    std::lock_guard<std::mutex> l(mu_);
    auto f = items_.find(uid);
    if (f == items_.end()) return std::nullopt;
    return f->second.state;
}

std::size_t UpsProvider::size() const {
    std::lock_guard<std::mutex> l(mu_);
    return items_.size();
}

std::uint64_t UpsProvider::bytes() const {
    std::lock_guard<std::mutex> l(mu_);
    return bytes_;
}

std::size_t UpsProvider::skipped_files() const {
    std::lock_guard<std::mutex> l(mu_);
    return skipped_;
}

std::vector<std::pair<std::string, UpsEventReport>> UpsProvider::take_events() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<std::pair<std::string, UpsEventReport>> out;
    out.swap(events_);
    return out;
}

}  // namespace siaara::net
