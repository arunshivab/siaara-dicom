// SIAARA DICOM Toolkit - HTTP/1.1 (RFC 9110 semantics, RFC 9112 message syntax) over the
// project's Transport abstraction (src/net/socket.hpp). A TLS Transport plugs in unchanged.
//
// Messages are fully buffered (no streaming bodies); every limit is configurable.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/status.hpp"
#include "net/socket.hpp"
#include "net/tls.hpp"

namespace siaara::web::http {

struct Limits {
    std::size_t max_request_line = 8192;      // request-line / status-line, bytes (414 when exceeded)
    std::size_t max_header_bytes = 64 * 1024; // whole header section (431)
    std::size_t max_headers = 100;            // header field lines (431)
    std::uint64_t max_body = 256ull << 20;    // decoded body (413)
    std::size_t max_chunk_line = 4096;        // chunk-size line including extensions
    int read_timeout_ms = 30000;              // each wait for incoming bytes
    int write_timeout_ms = 30000;             // each wait for send-buffer space
    // The whole start line + header section, from the first byte of the message (408).
    int header_timeout_ms = 30000;
    // The whole message body (408); 0: no overall deadline.
    int body_timeout_ms = 10 * 60 * 1000;
    // Minimum average body rate (bytes/s) over each body_rate_window_ms (408); 0: none.
    std::uint32_t min_body_rate = 1024;
    int body_rate_window_ms = 30000;
};

// Server-wide budget for request bodies held in memory at once (WEB-01). Thread safe.
class BodyBudget {
public:
    explicit BodyBudget(std::uint64_t limit) noexcept : limit_(limit) {}
    bool take(std::uint64_t n) noexcept;
    void give(std::uint64_t n) noexcept;
    std::uint64_t in_use() const noexcept { return used_.load(); }

private:
    std::atomic<std::uint64_t> used_{0};
    std::uint64_t limit_;
};

struct Header {
    std::string name;
    std::string value;
};
using Headers = std::vector<Header>;

// Case-insensitive field-name lookup (first occurrence).
const std::string* find_header(const Headers& h, std::string_view name) noexcept;
// All values of a (possibly repeated) list-valued field, comma split, whitespace trimmed.
std::vector<std::string> header_list(const Headers& h, std::string_view name);
bool iequals(std::string_view a, std::string_view b) noexcept;

struct Request {
    std::string method;
    std::string target;        // request-target as received (origin-form for a client request)
    int version_minor = 1;     // HTTP/1.x
    Headers headers;
    std::string body;
};

struct Response {
    int status = 200;
    std::string reason;        // empty: the standard phrase
    int version_minor = 1;
    Headers headers;
    std::string body;
    void set(std::string_view name, std::string value);
};

std::string_view reason_phrase(int status) noexcept;

// ---------------------------------------------------------------- media types (RFC 9110 8.3.1, 12.5.1)
struct MediaType {
    std::string type;      // lower case
    std::string subtype;   // lower case
    std::vector<std::pair<std::string, std::string>> params;  // names lower case, values unquoted
    int weight = 1000;     // Accept "q" in thousandths

    const std::string* param(std::string_view name) const noexcept;
    bool is(std::string_view t, std::string_view s) const noexcept { return type == t && subtype == s; }
    std::string essence() const { return type + "/" + subtype; }
};
Result<MediaType> parse_media_type(std::string_view s);
// Parses an Accept value; malformed members are skipped. Sorted by weight (stable).
std::vector<MediaType> parse_accept(std::string_view s);
// type/subtype; name=value pairs (quoted when needed).
std::string format_media_type(const MediaType& m);
std::string quote_if_needed(std::string_view v);

// ---------------------------------------------------------------- URLs and targets (RFC 3986)
std::string percent_encode(std::string_view s);  // everything except unreserved characters
// For a query key or value: also leaves the RFC 3986 query characters "/:@!$'()*,;?" as they
// are (some servers do not decode them); '&', '=', '+', '#' and '%' are always encoded.
std::string percent_encode_query(std::string_view s);
// Fails (Malformed) on bad escapes and on decoded C0 controls or DEL (PARSE-40: e.g. %00).
Result<std::string> percent_decode(std::string_view s, bool plus_is_space);
struct Target {
    std::string path;                                        // decoded
    std::vector<std::string> segments;                       // decoded, empty ones dropped
    std::vector<std::pair<std::string, std::string>> query;  // decoded, in order
};
Result<Target> parse_target(std::string_view target);
struct Url {
    std::string scheme;  // "http" or "https"
    std::string host;
    std::uint16_t port = 80;  // default 80 for http, 443 for https
    std::string path;    // without trailing '/'
};
Result<Url> parse_url(std::string_view url);

// ---------------------------------------------------------------- connection
class Connection {
public:
    Connection(std::unique_ptr<net::Transport> t, const Limits& limits);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    // Request bodies are charged to `budget` while held (until release_body_budget() or the next
    // read_request()); a request that would exceed it fails with Errc::Rejected (503).
    void set_body_budget(std::shared_ptr<BodyBudget> budget) noexcept { budget_ = std::move(budget); }
    void release_body_budget() noexcept;

    // Reads one request. `first_byte_timeout_ms` bounds the wait for the start of the request
    // (keep-alive idle time). On failure, suggested_status() is the status to answer with
    // (0: answer nothing, just close - e.g. the peer closed or timed out while idle).
    Result<Request> read_request(int first_byte_timeout_ms);
    int suggested_status() const noexcept { return suggested_; }
    // `request_method` decides whether a body follows (HEAD).
    Result<Response> read_response(std::string_view request_method);

    Status write_request(const Request& rq);
    // Adds Content-Length (and Connection: close when `close`). Omits the body for HEAD.
    Status write_response(const Response& rs, bool head, bool close);
    Status write_raw(std::string_view data);

    net::Transport& transport() noexcept { return *t_; }
    bool is_open() const noexcept { return t_ && t_->is_open(); }
    void close() noexcept;
    // Close after an error response sent without reading the whole request: ends the sending
    // direction, then discards input until the peer closes, `max_ms` passes or `max_bytes` were
    // read. Closing with unread input makes the OS send a reset, and a Windows peer then drops
    // the response it had not read yet.
    void close_lingering(int max_ms, std::size_t max_bytes) noexcept;
    // True when bytes of a next message have already been received.
    bool has_buffered() const noexcept { return pos_ < buf_.size(); }

private:
    std::unique_ptr<net::Transport> t_;
    Limits lim_;
    std::string buf_;
    std::size_t pos_ = 0;
    int suggested_ = 0;
    std::int64_t deadline_ms_ = 0;  // steady-clock deadline for the header section; 0: none
    bool arm_header_deadline_ = false;  // set deadline_ms_ when the first byte of a message arrives
    // Body reading (WEB-01): overall deadline and minimum-rate window.
    bool in_body_ = false;
    std::int64_t body_deadline_ms_ = 0;
    std::int64_t window_end_ms_ = 0;
    std::uint64_t window_bytes_ = 0;
    std::size_t last_read_ = 0;
    std::shared_ptr<BodyBudget> budget_;
    std::uint64_t reserved_ = 0;

    Status fill(int timeout_ms);
    Status fill_once(int timeout_ms);
    void begin_body();
    Status read_line(std::size_t cap, std::string& line, int first_timeout_ms, bool& got_any);
    Status read_headers(Headers& h);
    Status read_exact(std::uint64_t n, std::string& out);
    Status read_chunked(std::string& body);
    Status read_to_close(std::string& body);
    int timeout_now(int base) const;
};

// True for "127.x.x.x", "::1", "[::1]" and "localhost".
bool is_loopback_address(std::string_view host) noexcept;

// Keep-alive decision for a message (RFC 9112 9.3).
bool wants_close(const Headers& h, int version_minor) noexcept;
// Writes the header section (used by both directions). Fails on CR/LF/NUL in names or values.
Status append_headers(std::string& out, const Headers& h);

// ---------------------------------------------------------------- client
class Client {
public:
    Client(std::string host, std::uint16_t port, const Limits& limits = {}, int connect_timeout_ms = 10000);
    // Sends one request and reads the response. Host and Content-Length are added. A persistent
    // connection is reused; if it turns out to be stale before any response byte arrived, the
    // request is retried once on a new connection.
    Result<Response> send(const Request& rq);
    void close() noexcept;
    // Wakes a send() blocked in another thread: the open connection is shut down and that call
    // fails. Later calls open a new connection.
    void interrupt() noexcept;
    // Optional transport factory (e.g. TLS); default: plain TCP.
    std::function<Result<std::unique_ptr<net::Transport>>(const std::string& host, std::uint16_t port, int timeout_ms)>
        connector;
    // The scheme's default port, left out of the Host header (443 for https).
    std::uint16_t default_port = 80;

private:
    std::string host_;
    std::uint16_t port_;
    Limits lim_;
    int connect_timeout_ms_;
    std::unique_ptr<Connection> conn_;
    std::mutex conn_mu_;  // conn_ changes and interrupt() (the connection itself is used by one thread)
    std::atomic<bool> interrupted_{false};
    Result<Response> attempt(const Request& rq, bool& sent_on_reused, bool& got_bytes);
};

// ---------------------------------------------------------------- server
struct ServerConfig {
    // Plain HTTP is refused on an address other than loopback unless allow_insecure_remote is set
    // (APP-01); DicomwebServer also requires authorize or client certificates there.
    std::string bind_address = "127.0.0.1";
    bool allow_insecure_remote = false;  // when set, a warning is logged at start
    std::uint16_t port = 0;              // 0: ephemeral, see Server::port()
    std::size_t max_connections = 32;    // further connections get 503 and are closed
    // Concurrent connections from one peer address (NET-04); further ones get 503. 0: unlimited.
    // (Browsers open up to 6 per host.)
    std::size_t max_connections_per_peer = 8;
    // Request bytes held in memory across all connections (WEB-01); over it: 503. 0: unlimited.
    std::uint64_t max_inflight_body_bytes = 1ull << 30;
    Limits limits;
    int idle_timeout_ms = 15000;         // keep-alive wait for the next request
    std::uint32_t max_requests_per_connection = 1000;
    // Diagnostics (addresses, status codes; never request content).
    std::function<void(const std::string&)> log;
    // HTTPS when tls.enabled (every connection on this port; tls.log defaults to log).
    net::TlsSettings tls;
};

using Handler = std::function<void(const Request&, Response&)>;

class Server {
public:
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    Status start(const ServerConfig& cfg, Handler handler);
    std::uint16_t port() const noexcept { return cfg_.tls.enabled ? tls_listener_.port() : listener_.port(); }
    // Stops accepting, wakes every connection and joins all threads.
    void stop() noexcept;

    // Wraps an already connected transport (TLS adapters, tests); runs on the calling thread.
    void serve_connection(std::unique_ptr<net::Transport> t);

private:
    struct Worker {
        std::thread thread;
        std::atomic<bool> done{false};
        net::Transport* transport = nullptr;
        std::string peer;
    };
    std::shared_ptr<BodyBudget> budget_;
    ServerConfig cfg_;
    Handler handler_;
    net::TcpListener listener_;
    net::TlsListener tls_listener_;  // instead of listener_ when cfg_.tls.enabled
    std::thread accept_thread_;
    std::atomic<bool> stopping_{false};
    std::mutex mu_;
    std::list<Worker> workers_;
    void accept_loop();
    void reap(bool all);
    void serve(Connection& c, const std::string& peer);
};

}  // namespace siaara::web::http
