// SIAARA DICOM Toolkit - transport abstraction and blocking TCP sockets with timeouts.
// The DICOM Upper Layer (PS3.8 section 9) runs over TCP; this file only moves bytes.
// A TLS transport (PS3.15 Annex B) can implement Transport later without touching the upper layer.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/status.hpp"

namespace siaara::net {

// Initialises the OS socket library once (WSAStartup on Windows; no-op elsewhere).
// Thread safe; called automatically by every function below.
Status socket_library_init();

// A connected, bidirectional byte stream. Every blocking call takes a timeout in milliseconds;
// a negative timeout is treated as 0 (poll only). There is no "wait forever".
class Transport {
public:
    virtual ~Transport() = default;
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Reads up to `n` bytes. Returns the number read (>0), 0 on orderly close by the peer,
    // Errc::Timeout when nothing arrived in time, Errc::NetworkError on failure.
    virtual Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) = 0;
    // Writes all `n` bytes; `timeout_ms` applies to each wait for send-buffer space.
    virtual Status write_all(const std::uint8_t* data, std::size_t n, int timeout_ms) = 0;
    // True when data (or EOF) can be read without blocking within the timeout.
    virtual Result<bool> wait_readable(int timeout_ms) = 0;
    // Shuts down both directions; may be called from another thread to wake a blocked reader.
    virtual void shutdown() noexcept = 0;
    // Ends the sending direction only (TCP FIN after queued data), so a peer that is still
    // sending sees the end of the response before the handle is released. Default: no-op.
    virtual void shutdown_send() noexcept {}
    // Releases the OS handle. Must not race with reads/writes on other threads.
    virtual void close() noexcept = 0;
    virtual bool is_open() const noexcept = 0;
    // Numeric address of the peer ("127.0.0.1", "::1"), for logging.
    virtual std::string peer_address() const = 0;
};

// Reads exactly `n` bytes within `timeout_ms` in total: an absolute deadline for the whole read, so
// a peer dripping bytes cannot stretch it (NET-03). Errc::Timeout when the deadline passes first.
// Orderly close before `n` bytes yields Errc::NetworkError with message "connection closed".
Status read_exact(Transport& t, std::uint8_t* buf, std::size_t n, int timeout_ms);

using Deadline = std::chrono::steady_clock::time_point;
// now + timeout_ms (a negative timeout counts as 0).
Deadline deadline_after(int timeout_ms) noexcept;
// Milliseconds left until `d`, clamped to [0, INT_MAX].
int remaining_ms(Deadline d) noexcept;
// read_exact against an absolute deadline shared with other reads.
Status read_exact_until(Transport& t, std::uint8_t* buf, std::size_t n, Deadline deadline);

// Opens a TCP connection to host:port (IPv4 or IPv6, numeric or name) within `timeout_ms`.
// Name resolution uses the OS resolver, which has no portable timeout.
Result<std::unique_ptr<Transport>> tcp_connect(const std::string& host, std::uint16_t port,
                                               int timeout_ms);

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Binds and listens. `bind_address` empty means all interfaces (IPv6 dual stack when
    // available, otherwise IPv4). Port 0 picks an ephemeral port; see port().
    Status listen(const std::string& bind_address, std::uint16_t port, int backlog = 64);
    std::uint16_t port() const noexcept { return port_; }
    // Waits up to `timeout_ms` for a connection. Errc::Timeout when none arrived.
    Result<std::unique_ptr<Transport>> accept(int timeout_ms);
    void close() noexcept;
    bool is_open() const noexcept;

private:
    std::intptr_t fd_ = -1;
    std::uint16_t port_ = 0;
};

}  // namespace siaara::net
