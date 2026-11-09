// SIAARA DICOM Toolkit - DICOMweb user agent (PS3.18): QIDO-RS, WADO-RS, WADO-URI and STOW-RS.
// One client keeps one persistent HTTP connection; it is not safe to share between threads.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "web/dicom_model.hpp"
#include "web/dicomweb.hpp"
#include "web/http.hpp"
#include "web/multipart.hpp"

namespace siaara::web {

struct DicomwebClientConfig {
    std::string base_url;        // e.g. "http://127.0.0.1:8042/dicom-web"
    std::string wado_uri_url;    // WADO-URI endpoint; empty: base_url + "/wado"
    http::Limits limits;
    multipart::Limits multipart;
    ModelReadOptions model;
    int connect_timeout_ms = 10000;
    // Returns the Authorization header value for each request (e.g. from bearer_auth() or
    // basic_auth()); an empty string sends none.
    std::function<std::string()> authorization;
    // Used for https:// base URLs (trust anchors, client identity for mutual TLS, ...); the
    // `enabled` flag is implied by the scheme. Setting tls.enabled with an http:// URL is an error.
    net::TlsSettings tls;
};

std::string bearer_auth(std::string_view token);
std::string basic_auth(std::string_view user, std::string_view password);

struct QidoParams {
    std::vector<std::pair<std::string, std::string>> match;  // keyword or tag -> value
    std::vector<std::string> include_fields;                 // keywords or tags
    bool include_all = false;
    std::optional<std::uint32_t> limit;
    std::optional<std::uint32_t> offset;
    bool fuzzy_matching = false;
};

struct BulkPart {
    std::string content_type;
    std::string data;
};

struct StowResult {
    int http_status = 0;
    Dataset response;                     // the Store Instances Response (may be empty)
    std::vector<std::string> stored;      // Referenced SOP Instance UIDs
    std::vector<std::pair<std::string, std::uint16_t>> failed;  // SOP Instance UID, Failure Reason
};

class DicomwebClient {
public:
    explicit DicomwebClient(DicomwebClientConfig cfg);
    // Validates the configuration (URL); every call below also reports it.
    Status status() const { return init_; }

    Result<std::vector<Dataset>> search_studies(const QidoParams& q = {});
    Result<std::vector<Dataset>> search_series(const QidoParams& q = {}, std::string_view study = {});
    Result<std::vector<Dataset>> search_instances(const QidoParams& q = {}, std::string_view study = {},
                                                  std::string_view series = {});

    // WADO-RS: Part 10 streams of a study, series or instance. `transfer_syntaxes` lists the
    // acceptable syntaxes in preference order ("*" = as stored); empty: the server default.
    Result<std::vector<std::string>> retrieve_raw(std::string_view study, std::string_view series = {},
                                                  std::string_view instance = {},
                                                  const std::vector<std::string>& transfer_syntaxes = {});
    Result<std::vector<DicomFile>> retrieve(std::string_view study, std::string_view series = {},
                                            std::string_view instance = {},
                                            const std::vector<std::string>& transfer_syntaxes = {});
    Result<std::vector<Dataset>> retrieve_metadata(std::string_view study, std::string_view series = {},
                                                   std::string_view instance = {});
    // Frames are 1-based. `transfer_syntaxes` as above; each entry is sent with the media type
    // of that syntax (application/octet-stream for native ones, image/jls for JPEG-LS, ...).
    Result<std::vector<BulkPart>> retrieve_frames(std::string_view study, std::string_view series,
                                                  std::string_view instance, const std::vector<std::uint32_t>& frames,
                                                  const std::vector<std::string>& transfer_syntaxes = {});
    // A BulkDataURI from metadata (absolute http URL on the same server, or a path).
    Result<std::vector<BulkPart>> retrieve_bulkdata(std::string_view uri);
    // Rendered resources: media_type such as "image/jpeg"; frame 0 = the instance as a whole.
    Result<BulkPart> retrieve_rendered(std::string_view study, std::string_view series, std::string_view instance,
                                       std::string_view media_type, std::uint32_t frame = 0);
    // WADO-URI (legacy, PS3.18 chapter 9). Returns the response body.
    Result<std::string> wado_uri(std::string_view study, std::string_view series, std::string_view object,
                                 std::string_view content_type = "application/dicom",
                                 std::string_view transfer_syntax = {});

    // STOW-RS: Part 10 streams. `study` empty posts to /studies.
    Result<StowResult> store(const std::vector<std::string>& part10, std::string_view study = {});

    // A raw request below the base path (path starts with '/'); for anything not covered above.
    Result<http::Response> request(std::string_view method, std::string_view path_and_query,
                                   http::Headers headers = {}, std::string body = {});

private:
    DicomwebClientConfig cfg_;
    Status init_;
    http::Url url_;
    http::Url wado_url_;
    std::optional<http::Client> http_;
    std::shared_ptr<net::TlsContext> tls_;

    Result<http::Response> send(http::Request rq);
    Result<std::vector<Dataset>> qido(std::string path, const QidoParams& q);
    Result<std::vector<BulkPart>> multipart_get(std::string target, std::string accept);
    Status check_status(const http::Response& rs) const;
};

}  // namespace siaara::web
