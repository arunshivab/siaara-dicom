#include "store/scan.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/io.hpp"
#include "store/common.hpp"
#include "store/dicomdir.hpp"

namespace siaara::store {

namespace fs = std::filesystem;

namespace {

class Queue {
public:
    explicit Queue(std::size_t cap) : cap_(cap) {}
    // false when the queue was closed
    bool push(std::string s) {
        std::unique_lock<std::mutex> g(m_);
        not_full_.wait(g, [&] { return q_.size() < cap_ || closed_; });
        if (closed_) return false;
        q_.push_back(std::move(s));
        not_empty_.notify_one();
        return true;
    }
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> g(m_);
        not_empty_.wait(g, [&] { return !q_.empty() || closed_ || done_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return true;
    }
    void finish() {  // no more input; drain what is queued
        std::lock_guard<std::mutex> g(m_);
        done_ = true;
        not_empty_.notify_all();
    }
    void close() {  // cancel: drop what is queued
        std::lock_guard<std::mutex> g(m_);
        closed_ = true;
        q_.clear();
        not_empty_.notify_all();
        not_full_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable not_empty_, not_full_;
    std::deque<std::string> q_;
    std::size_t cap_;
    bool done_ = false, closed_ = false;
};

}  // namespace

Result<ScanStats> scan_folder(const std::string& utf8_root, const ScanCallback& cb, const ScanOptions& opt) {
    ScanStats stats;
    std::error_code ec;
    const fs::path root = u8path(utf8_root);
    if (!fs::is_directory(root, ec)) return Status(Errc::NotFound, "scan root is not a folder");

    unsigned n = opt.threads ? opt.threads : std::thread::hardware_concurrency();
    n = std::clamp(n, 1u, 64u);
    if (!opt.threads) n = std::min(n, 16u);

    Queue q(4096);
    std::mutex cb_mutex;
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint64_t> instances{0}, dicomdirs{0}, skipped{0}, errors{0};

    auto deliver = [&](const ScanItem& item) {
        std::lock_guard<std::mutex> g(cb_mutex);
        if (cancelled.load()) return;
        if (cb && !cb(item)) {
            cancelled.store(true);
            q.close();
        }
    };

    auto worker = [&]() {
        std::string path;
        ScanItem item;
        while (q.pop(path)) {
            if (cancelled.load()) break;
            item.path = path;
            item.status = Status::ok();
            auto bytes = io::map_file(path);
            if (!bytes) {
                item.kind = ScanKind::Error;
                item.status = bytes.take_error();
                item.record = IndexRecord{};
                ++errors;
                deliver(item);
                continue;
            }
            if (!looks_like_dicom(bytes->data(), bytes->size(), opt.allow_raw)) {
                ++skipped;
                if (opt.report_skipped) {
                    item.kind = ScanKind::NotDicom;
                    item.record = IndexRecord{};
                    deliver(item);
                }
                continue;
            }
            Status s = extract_index(*bytes, item.record, ExtractOptions{opt.allow_raw});
            if (!s) {
                if (s.code == Errc::NotDicom) {
                    ++skipped;
                    if (!opt.report_skipped) continue;
                    item.kind = ScanKind::NotDicom;
                } else {
                    ++errors;
                    item.kind = ScanKind::Error;
                }
                item.status = std::move(s);
                deliver(item);
                continue;
            }
            if (item.record[Str::SopInstanceUid].empty() && item.record[Str::SopClassUid] != kMediaStorageDirectoryUid) {
                ++errors;
                item.kind = ScanKind::Error;
                item.status = Status(Errc::Malformed, "no SOP Instance UID");
                deliver(item);
                continue;
            }
            if (item.record[Str::SopClassUid] == kMediaStorageDirectoryUid) {
                item.kind = ScanKind::Dicomdir;
                ++dicomdirs;
            } else {
                item.kind = ScanKind::Instance;
                ++instances;
            }
            deliver(item);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned i = 0; i < n; ++i) pool.emplace_back(worker);

    Status producer_status;
    std::uint64_t files = 0;
    auto offer = [&](const fs::path& p) {
        if (opt.max_files && files >= opt.max_files) return false;
        ++files;
        return q.push(to_utf8(p));
    };

    const fs::path dicomdir = root / "DICOMDIR";
    if (opt.use_dicomdir && fs::is_regular_file(dicomdir, ec)) {
        stats.used_dicomdir = true;
        std::vector<fs::path> listed;
        auto r = read_dicomdir(to_utf8(dicomdir), [&](const DicomdirRecord& rec) {
            if (!rec.keys[Str::FilePath].empty()) listed.push_back(u8path(rec.keys[Str::FilePath]));
            return true;
        });
        if (!r) {
            producer_status = r.take_error();
        } else {
            (void)offer(dicomdir);
            for (const auto& p : listed)
                if (!offer(p)) break;
        }
    } else {
        auto opts = fs::directory_options::skip_permission_denied;
        if (opt.follow_symlinks) opts |= fs::directory_options::follow_directory_symlink;
        if (opt.recursive) {
            for (fs::recursive_directory_iterator it(root, opts, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code ec2;
                const auto st = opt.follow_symlinks ? it->status(ec2) : it->symlink_status(ec2);
                if (ec2 || !fs::is_regular_file(st)) continue;
                if (!offer(it->path())) break;
            }
        } else {
            for (fs::directory_iterator it(root, opts, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code ec2;
                const auto st = opt.follow_symlinks ? it->status(ec2) : it->symlink_status(ec2);
                if (ec2 || !fs::is_regular_file(st)) continue;
                if (!offer(it->path())) break;
            }
        }
        if (ec && !cancelled.load()) producer_status = Status(Errc::IoError, "folder walk failed");
    }
    q.finish();
    for (auto& t : pool) t.join();

    stats.files = files;
    stats.instances = instances.load();
    stats.dicomdirs = dicomdirs.load();
    stats.skipped = skipped.load();
    stats.errors = errors.load();
    if (cancelled.load()) return Status(Errc::Cancelled, "cancelled by the callback");
    if (!producer_status) return producer_status;
    return stats;
}

}  // namespace siaara::store
