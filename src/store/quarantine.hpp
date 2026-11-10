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
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace siaara::store {

struct QuarantineLimits {
    std::size_t max_items = 10000;
    std::uint64_t max_bytes = 4ull << 30;
    // When a cap would be passed: false (default) refuses the new item (Errc::LimitExceeded);
    // true removes the oldest items until it fits.
    bool evict_oldest = false;
};
inline constexpr std::size_t kDefaultQuarantineMaxItems = 10000;
inline constexpr std::uint64_t kDefaultQuarantineMaxBytes = 4ull << 30;
inline constexpr bool kDefaultQuarantineEvictOldest = false;

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
};

enum class QuarantineAdd { Added, Duplicate };

class Quarantine {
public:
    // `store_root`: the store's root folder (UTF-8). Nothing is created until the first add().
    // `area`: the folder under the store root ("quarantine", or "rejected" for copies of refused
    // instances - same layout, separate caps).
    explicit Quarantine(std::string store_root, const QuarantineLimits& limits = {}, std::string_view area = "quarantine");

    // Keeps `bytes` verbatim. `rec.id`, `rec.size` and `rec.time` are filled in here.
    // Errc::LimitExceeded when the item or byte cap would be passed (nothing is kept).
    Result<QuarantineAdd> add(const Bytes& bytes, QuarantineRecord rec);
    // Newest first.
    Result<std::vector<QuarantineRecord>> list() const;
    Result<QuarantineRecord> record(std::string_view id) const;
    Result<Bytes> bytes(std::string_view id) const;
    Status remove(std::string_view id);
    // True when an item holds this SOP Instance UID (Storage Commitment: never "committed").
    bool holds_instance(std::string_view sop_instance_uid) const;
    std::size_t items() const;
    std::uint64_t total_bytes() const;

private:
    Status load_locked() const;
    std::string dir_;
    QuarantineLimits lim_;
    mutable std::mutex mu_;
    mutable bool loaded_ = false;
    mutable std::vector<QuarantineRecord> recs_;  // under mu_
    mutable std::uint64_t bytes_ = 0;
};

}  // namespace siaara::store
