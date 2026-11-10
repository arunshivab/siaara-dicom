// SIAARA DICOM Toolkit - the quarantine area of a store (R-116, IncompletePolicy::Quarantine):
// instances that arrived incomplete are kept here exactly as received, outside the archive index,
// so C-FIND / QIDO / C-GET / C-MOVE / WADO never return them, until an operator releases or
// deletes them.
//
// Layout: <store root>/quarantine/<id>.bin (the bytes, verbatim) and <id>.rec (a small text
// record), both owner-only and written atomically; <id> is the SHA-256 of the bytes in hex, so
// the same bytes arriving again are one item. The record holds the time, the calling AE and peer
// address, SOP Class / Instance UID when readable, the size, the read-loss bits and the first
// read warning - no other patient data.
//
// Memory and locking (R-159, R-165): an item is checked against the caps before a byte of it is
// read, then streamed in pieces to an owner-only temporary file while its SHA-256 is computed;
// nothing of the item's size is ever allocated. Room is reserved under the area lock, the bytes
// are written outside it (one large item does not block other senders), and the item is named,
// de-duplicated and recorded under the lock again.
//
// A ".bin" file without its record (a crash between the two writes, R-166) is never deleted
// silently: it is listed with QuarantineRecord::record_missing set and counts toward the caps.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::store {

inline constexpr std::size_t kDefaultQuarantineMaxItems = 10000;
inline constexpr std::uint64_t kDefaultQuarantineMaxBytes = 4ull << 30;
inline constexpr bool kDefaultQuarantineEvictOldest = false;
inline constexpr std::uint32_t kDefaultQuarantineSenderSharePercent = 0;       // no share limit
inline constexpr std::uint64_t kDefaultQuarantineMinFreeDiskBytes = 64ull << 20;

struct QuarantineLimits {
    std::size_t max_items = kDefaultQuarantineMaxItems;
    std::uint64_t max_bytes = kDefaultQuarantineMaxBytes;
    // When a cap would be passed: false (default) refuses the new item (Errc::LimitExceeded);
    // true removes the oldest items until it fits.
    bool evict_oldest = kDefaultQuarantineEvictOldest;
    // The share of both caps one sender may take (R-165), in percent: 0 (default) = no share limit;
    // 1 .. 100: one sender's items together hold at most this share of max_bytes and of max_items
    // (at least one item), so a single sender cannot fill the area and lock everyone else out. A
    // sender is the calling AE title, or the peer address where there is none (DICOMweb). Past
    // its share a sender is refused like a full area (with evict_oldest: its own oldest items go).
    std::uint32_t sender_share_percent = kDefaultQuarantineSenderSharePercent;
    // Free space the area leaves on the volume: an item that would go below it is refused
    // (Errc::LimitExceeded) before anything is written. 0 = no floor.
    std::uint64_t min_free_disk_bytes = kDefaultQuarantineMinFreeDiskBytes;
    // Receives one-line notes (no patient data): a ".bin" file found without its record.
    std::function<void(const std::string&)> note;
};
struct QuarantineRecord {
    std::string id;            // SHA-256 of the bytes, 64 lower-case hex digits
    std::int64_t time = 0;     // seconds since the Unix epoch (UTC) when it was quarantined
    std::string source;        // "C-STORE", "C-GET", "STOW-RS", "WADO-RS"
    std::string calling_ae;    // empty for DICOMweb
    std::string peer;          // network address when known
    std::string sop_class_uid, sop_instance_uid;  // when readable
    std::uint64_t size = 0;
    std::uint32_t read_loss = 0;  // DicomFile::ReadLoss bits
    std::string warning;          // the first read warning (a fixed code and offsets)
    // R-166: the bytes are there but their record is not (e.g. a crash between the two writes).
    // Only id, size and time (the file's modification time) are known. Kept, never deleted silently.
    bool record_missing = false;
};

// One piece of an item handed to Quarantine::add (the item is the pieces in order).
struct QuarantinePart {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

enum class QuarantineAdd { Added, Duplicate };

class Quarantine {
public:
    // `store_root`: the store's root folder (UTF-8). Nothing is created until the first add().
    // `area`: the folder under the store root ("quarantine", or "rejected" for copies of refused
    // instances - same layout, separate caps).
    explicit Quarantine(std::string store_root, const QuarantineLimits& limits = {}, std::string_view area = "quarantine");

    // Keeps the pieces, in order, verbatim as one item. `rec.id`, `rec.size` and `rec.time` are
    // filled in here. Errc::LimitExceeded when the item cap, the byte cap, the sender's share or
    // the free-space floor would be passed: decided from the sizes alone, before any byte is read
    // or written (nothing is kept). The pieces are streamed; no copy of the item is made.
    Result<QuarantineAdd> add(const QuarantinePart* parts, std::size_t part_count, QuarantineRecord rec);
    Result<QuarantineAdd> add(const Bytes& bytes, QuarantineRecord rec);
    // Newest first.
    Result<std::vector<QuarantineRecord>> list() const;
    Result<QuarantineRecord> record(std::string_view id) const;
    // The item's bytes, memory-mapped (the area's own owner-only file; nothing is copied).
    Result<Bytes> bytes(std::string_view id) const;
    Status remove(std::string_view id);
    // Held by a caller across "read the bytes, store them, remove the item" (R-167): two releases
    // of one item then run one after the other and the second finds the item gone.
    std::unique_lock<std::mutex> release_lock() const { return std::unique_lock<std::mutex>(release_mu_); }
    std::size_t items() const;
    std::uint64_t total_bytes() const;

private:
    struct Pending {
        std::size_t items = 0;
        std::uint64_t bytes = 0;
    };
    Status load_locked() const;
    void evict_locked(std::vector<QuarantineRecord>::iterator it);
    std::string dir_;
    QuarantineLimits lim_;
    mutable std::mutex mu_;
    mutable std::mutex release_mu_;
    mutable bool loaded_ = false;
    mutable std::vector<QuarantineRecord> recs_;  // under mu_
    mutable std::uint64_t bytes_ = 0;
    // Room reserved by adds that are writing outside the lock (under mu_): in all and per sender.
    Pending pending_;
    std::map<std::string, Pending, std::less<>> pending_by_sender_;
};

}  // namespace siaara::store
