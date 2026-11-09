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

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/dataset.hpp"
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
    // Limit and offset are applied by the server to the store's results.
    std::optional<std::uint32_t> limit;
    std::uint32_t offset = 0;
};

struct InstanceKey {
    std::string study;
    std::string series;    // empty: whole study
    std::string instance;  // empty: whole series
};

// Callbacks the server is backed by. Every callback may be called from several threads at once.
struct Store {
    // Appends every match (attributes at the query level plus the requested ones).
    // Return a non-ok Status for failures (Errc::Unsupported -> 400, others -> 500).
    std::function<Status(const SearchQuery&, std::vector<Dataset>& out)> search;
    // Appends the Part 10 streams of every instance under the key; Errc::NotFound when none.
    std::function<Status(const InstanceKey&, std::vector<Bytes>& out)> retrieve;
    // Stores one received instance; returns 0 on success or a STOW-RS failure reason.
    std::function<std::uint16_t(const DicomFile& file, const Bytes& part10)> store;
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
    multipart::Limits multipart;
    // Access control: return false to answer 401. Default: everything allowed.
    std::function<bool(const http::Request&)> authorize;
    // CORS is off unless an origin is configured ("*" or an exact origin).
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
    http::Server http_;
};

}  // namespace siaara::web
