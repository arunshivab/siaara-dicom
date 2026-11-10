// SIAARA DICOM Toolkit - DICOMweb origin server (PS3.18): QIDO-RS, WADO-RS, WADO-URI and STOW-RS
// on top of http::Server, backed by a pluggable Store.
//
// Routes (below ServerConfig::base_path):
//   GET  /studies | /series | /instances | /studies/{s}/series | /studies/{s}/instances |
//        /studies/{s}/series/{se}/instances                                   QIDO-RS
//   GET  /studies/{s}[/series/{se}[/instances/{i}]]                           WADO-RS instances
//   GET  .../metadata                                                         WADO-RS metadata
//   GET  /studies/{s}/series/{se}/instances/{i}/frames/{n[,n...]}             WADO-RS frames
//   GET  /studies/{s}/series/{se}/instances/{i}/bulkdata/{tag[/item/tag...]}  WADO-RS bulk data
//   GET  .../rendered                                                         406 (no renderer)
//   POST /studies[/{s}]                                                       STOW-RS
//   GET  /wado?requestType=WADO&... (DicomwebServerConfig::wado_uri_path)    WADO-URI
// Error bodies are fixed text: request content is never echoed back.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/dataset.hpp"
#include "core/incomplete_policy.hpp"
#include "core/status.hpp"
#include "web/dicom_model.hpp"
#include "web/dicomweb.hpp"
#include "web/http.hpp"
#include "web/multipart.hpp"

namespace siaara::web {

struct SearchQuery {
    QueryLevel level = QueryLevel::Study;
    std::string study_uid;   // from the path (may be empty)
    std::string series_uid;  // from the path (may be empty)
    std::vector<std::pair<Tag, std::string>> match;  // attribute matching (decoded values)
    std::vector<Tag> include_fields;
    bool include_all = false;
    bool fuzzy_matching = false;
    // Paging (WEB-05). The server applies offset and limit to the store's results, so a store may
    // return every match; but it may also stop once it has offset + limit matches (limit is
    // already capped at max_qido_results + 1, so the server can still tell that results were cut).
    std::optional<std::uint32_t> limit;
    std::uint32_t offset = 0;
};

struct InstanceKey {
    std::string study;
    std::string series;    // empty: whole study
    std::string instance;  // empty: whole series
};

// One stored instance, as the store's index knows it (nothing of the file is read to tell).
struct StoredInstance {
    std::string path;             // what Store::open takes (a file path for the default open)
    std::uint64_t size = 0;       // bytes of the stored Part 10 stream
    std::string transfer_syntax;  // UID as indexed; empty when unknown
    // R-161: the size of the Part 10 stream once its pixel data is decoded to a native transfer
    // syntax (the other elements plus Rows x Columns x Number of Frames x bytes per sample x
    // Samples per Pixel), from the index. 0: not known - the server then reads the instance's
    // header (not its pixel data) when it has to know.
    std::uint64_t decoded_size = 0;
};

// What a study / series / instance retrieve sends when the Accept header names no transfer
// syntax (R-161).
enum class DefaultTransferSyntax : std::uint8_t {
    ExplicitVrLittleEndian = 0,  // PS3.18's default: compressed instances are decoded for the reply
    AsStored = 1,                // each instance in the transfer syntax it is stored in (nothing is decoded)
};
// The defaults. Change them here only.
inline constexpr DefaultTransferSyntax kDefaultWadoTransferSyntax = DefaultTransferSyntax::ExplicitVrLittleEndian;
// Memory all connections together may use for decoding instances for retrieves.
inline constexpr std::uint64_t kDefaultWebDecodeMemoryBytes = 2ull << 30;
// How long a retrieve waits for its share of that memory before the reply is abandoned.
inline constexpr int kDefaultWebDecodeWaitMs = 60 * 1000;

// Server-wide budget for memory used to decode instances for retrieve replies (R-161): one
// decoded instance per connection used to be bounded only by the number of connections.
class DecodeBudget {
public:
    explicit DecodeBudget(std::uint64_t limit) noexcept : limit_(limit) {}
    std::uint64_t limit() const noexcept { return limit_; }
    // Waits up to `wait_ms` for `n` bytes; false when they did not become free (or n > limit).
    bool take(std::uint64_t n, int wait_ms);
    void give(std::uint64_t n) noexcept;

private:
    const std::uint64_t limit_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t used_ = 0;
};

// Callbacks the server is backed by. Every callback may be called from several threads at once.
struct Store {
    // Appends every match (attributes at the query level plus the requested ones).
    // Return a non-ok Status for failures (Errc::Unsupported -> 400, others -> 500).
    std::function<Status(const SearchQuery&, std::vector<Dataset>& out)> search;
    // Appends the Part 10 streams of every instance under the key; Errc::NotFound when none.
    // Used only when `locate` is not set: every instance is then in memory at once.
    std::function<Status(const InstanceKey&, std::vector<Bytes>& out)> retrieve;
    // R-110: lists the instances under the key without reading them; Errc::NotFound when none.
    // With it the server adds up the sizes first (413 before anything is read), then opens one
    // instance at a time: retrieves are streamed part by part, metadata is produced instance by
    // instance. Preferred over `retrieve`.
    std::function<Status(const InstanceKey&, std::vector<StoredInstance>& out)> locate;
    // Opens one located instance. Unset: the file at `path` is memory-mapped (io::map_file), so
    // its pixel data is paged in only when it is sent.
    std::function<Result<Bytes>(const StoredInstance&)> open;
    // Stores one received instance; returns 0 on success or a STOW-RS failure reason.
    std::function<std::uint16_t(const DicomFile& file, const Bytes& part10)> store;
    // Keeps an incomplete part outside the archive (IncompletePolicy::Quarantine); `peer` is the
    // client's address when known. Returns 0, or a failure reason (0xA700 when full).
    std::function<std::uint16_t(const DicomFile& file, const Bytes& part, const std::string& peer)> quarantine;
    // Keeps a copy of a refused incomplete part for diagnosis (IncompletePolicy::RefuseKeepCopy).
    std::function<void(const DicomFile& file, const Bytes& part, const std::string& peer)> keep_rejected;
};

struct DicomwebServerConfig {
    http::ServerConfig http;
    std::string base_path = "/dicomweb";   // prefix of every DICOMweb route ("" for the root)
    // Absolute URL of base_path as clients reach it (e.g. "http://pacs.example:8080/dicomweb"),
    // used for BulkDataURI and RetrieveURL. Empty: built from the request's Host header.
    std::string public_base_url;
    std::string wado_uri_path = "/wado";   // WADO-URI endpoint below base_path ("" disables it)
    // Binary values longer than this are returned as BulkDataURI in metadata responses.
    std::uint64_t bulk_data_threshold = 1024;
    std::size_t max_qido_results = 10000;  // hard cap; more matches are cut with a Warning header
    // WADO-RS frames: largest multipart response built in memory (413 above it; PARSE-31).
    std::uint64_t max_frames_response_bytes = 256ull << 20;
    // Study / series / instance retrieve: 413 when the stored instances are larger than this in
    // total - decided from the index sizes before any file is read when the store has `locate`
    // (R-110). Also the cap of every response built in memory (metadata, bulk data; R-052).
    std::uint64_t max_response_bytes = 1ull << 30;
    // R-161. A retrieve whose instances have to be decoded for the reply (Explicit VR Little
    // Endian asked for, or nothing asked for and default_transfer_syntax says so) is sized from
    // the decoded sizes - index values, or the instance headers - and answered 413 before
    // anything is sent when that passes max_response_bytes or one instance cannot be decoded
    // within max_decode_memory.
    DefaultTransferSyntax default_transfer_syntax = kDefaultWadoTransferSyntax;
    // Memory all connections together may use for decoding instances for retrieve replies. One
    // instance is charged twice its decoded size (the decoded frames and the stream built from
    // them) while its part is produced and sent; a retrieve waits up to decode_wait_ms for room.
    std::uint64_t max_decode_memory = kDefaultWebDecodeMemoryBytes;
    int decode_wait_ms = kDefaultWebDecodeWaitMs;
    // STOW-RS parts whose read was not complete (R-116): Refuse lists them in the Failed SOP
    // Sequence with Failure Reason 0xC000; AcceptAsReceived stores the part's bytes unchanged and
    // adds Warning Reason 0xB006 to its item. A part without a Part 10 preamble that is
    // incomplete is refused under both (it could only be stored rewritten). A complete part
    // without a preamble is stored as a new Part 10 file and reported with Warning Reason 0xB000.
    IncompletePolicy incomplete_policy = kDefaultIncompletePolicy;
    // Quarantine (needs Store::quarantine, else Refuse): the part's bytes are kept outside the
    // archive; the response lists the instance in the Referenced SOP Sequence with Warning Reason
    // 0xB007 and no Retrieve URL (Warning), or in the Failed SOP Sequence with 0xC000 (Failure).
    // A full quarantine gives Failure Reason 0xA700.
    QuarantineResponse quarantine_response = kDefaultQuarantineResponse;
    multipart::Limits multipart;
    // Access control: return false to answer 401. Default: everything allowed.
    // Off loopback, start() requires TLS plus authorize or client certificates
    // (http.tls.require_client_certificate), unless http.allow_insecure_remote is set (APP-01).
    std::function<bool(const http::Request&)> authorize;
    // CORS is off unless an origin is configured ("*" or an exact origin). "*" is refused together
    // with authorize (WEB-04): name the origin that may send credentials.
    std::string cors_allow_origin;
};

class DicomwebServer {
public:
    DicomwebServer() = default;
    ~DicomwebServer() { stop(); }
    DicomwebServer(const DicomwebServer&) = delete;
    DicomwebServer& operator=(const DicomwebServer&) = delete;

    Status start(const DicomwebServerConfig& cfg, Store store);
    std::uint16_t port() const noexcept { return http_.port(); }
    void stop() noexcept { http_.stop(); }

    // The request handler (usable without the built-in HTTP server, e.g. behind another one).
    void handle(const http::Request& rq, http::Response& rs) const;

private:
    DicomwebServerConfig cfg_;
    Store store_;
    std::shared_ptr<DecodeBudget> decode_budget_ = std::make_shared<DecodeBudget>(kDefaultWebDecodeMemoryBytes);
    http::Server http_;
};

}  // namespace siaara::web
