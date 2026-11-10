// SIAARA DICOM Toolkit - TCP transport for POSIX sockets and Winsock2.
#include "net/socket.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <limits>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace siaara::net {

namespace {

#if defined(_WIN32)
using Handle = SOCKET;
const std::intptr_t kInvalid = static_cast<std::intptr_t>(INVALID_SOCKET);
inline Handle to_handle(std::intptr_t v) { return static_cast<SOCKET>(v); }
inline int last_error() { return WSAGetLastError(); }
inline bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline bool interrupted(int e) { return e == WSAEINTR; }
inline void close_handle(Handle h) { ::closesocket(h); }
using PollFd = WSAPOLLFD;
inline int do_poll(PollFd* p, unsigned long n, int ms) { return ::WSAPoll(p, n, ms); }
#else
using Handle = int;
const std::intptr_t kInvalid = -1;
inline Handle to_handle(std::intptr_t v) { return static_cast<int>(v); }
inline int last_error() { return errno; }
inline bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS; }
inline bool interrupted(int e) { return e == EINTR; }
inline void close_handle(Handle h) { ::close(h); }
using PollFd = pollfd;
inline int do_poll(PollFd* p, unsigned long n, int ms) { return ::poll(p, static_cast<nfds_t>(n), ms); }
#endif

std::string error_text(const char* what, int err) {
    return std::string(what) + " failed (os error " + std::to_string(err) + ")";
}

Status set_nonblocking(Handle h) {
#if defined(_WIN32)
    u_long on = 1;
    if (::ioctlsocket(h, FIONBIO, &on) != 0) return Status(Errc::NetworkError, error_text("ioctlsocket", last_error()));
#else
    const int flags = ::fcntl(h, F_GETFL, 0);
    if (flags < 0 || ::fcntl(h, F_SETFL, flags | O_NONBLOCK) < 0)
        return Status(Errc::NetworkError, error_text("fcntl", last_error()));
    const int fdflags = ::fcntl(h, F_GETFD, 0);
    if (fdflags >= 0) (void)::fcntl(h, F_SETFD, fdflags | FD_CLOEXEC);
#endif
    return Status::ok();
}

void tune_stream_socket(Handle h) {
    int one = 1;
    (void)::setsockopt(h, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), static_cast<socklen_t>(sizeof one));
#if defined(__APPLE__)
    (void)::setsockopt(h, SOL_SOCKET, SO_NOSIGPIPE, &one, static_cast<socklen_t>(sizeof one));
#endif
}

int clamp_timeout(int ms) { return ms < 0 ? 0 : ms; }

// Waits for `events` on one socket. Returns 1 ready, 0 timeout, -1 error (errno/WSA set).
// `cancel`, when given, is checked between short poll slices: WSAPoll does not reliably wake a
// waiting thread when another thread shuts the socket down, so a local shutdown must be noticed
// without relying on the poll result.
int wait_one(Handle h, short events, int timeout_ms, const std::atomic<bool>* cancel = nullptr) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(clamp_timeout(timeout_ms));
    while (true) {
        PollFd p{};
        p.fd = h;
        p.events = events;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        int wait_ms = left <= 0 ? 0 : (left > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                                 : static_cast<int>(left));
        if (cancel) {
            if (cancel->load()) return 1;  // the following recv/send reports the shutdown
            if (wait_ms > 50) wait_ms = 50;
        }
        const int r = do_poll(&p, 1, wait_ms);
        if (r < 0) {
            if (interrupted(last_error())) continue;
            return -1;
        }
        if (r == 0) {
            if (cancel && left > 50) continue;
            return 0;
        }
        return 1;  // readiness, hang-up or error: the following call reports which
    }
}

std::string numeric_address(const sockaddr* sa, socklen_t len) {
    char host[NI_MAXHOST] = {0};
    if (::getnameinfo(sa, len, host, static_cast<socklen_t>(sizeof host), nullptr, 0, NI_NUMERICHOST) != 0) return "?";
    return host;
}

class TcpTransport final : public Transport {
public:
    explicit TcpTransport(Handle h) : fd_(static_cast<std::intptr_t>(h)) {}
    ~TcpTransport() override { close(); }

    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int timeout_ms) override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return Status(Errc::NetworkError, "socket closed");
        if (n == 0) return std::size_t{0};
        const int chunk = n > static_cast<std::size_t>(std::numeric_limits<int>::max())
                              ? std::numeric_limits<int>::max()
                              : static_cast<int>(n);
        while (true) {
            const int w = wait_one(to_handle(fd), POLLIN, timeout_ms, &shut_);
            if (w < 0) return Status(Errc::NetworkError, error_text("poll", last_error()));
            if (w == 0) return Status(Errc::Timeout, "read timed out");
#if defined(_WIN32)
            const int r = ::recv(to_handle(fd), reinterpret_cast<char*>(buf), chunk, 0);
#else
            const ssize_t r = ::recv(to_handle(fd), buf, static_cast<std::size_t>(chunk), 0);
#endif
            if (r > 0) return static_cast<std::size_t>(r);
            if (r == 0) return std::size_t{0};
            const int e = last_error();
            if (interrupted(e) || would_block(e)) continue;
            return Status(Errc::NetworkError, error_text("recv", e));
        }
    }

    Status write_all(const std::uint8_t* data, std::size_t n, int timeout_ms) override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return Status(Errc::NetworkError, "socket closed");
        std::size_t done = 0;
        while (done < n) {
            const std::size_t left = n - done;
            const int chunk = left > (1u << 30) ? (1 << 30) : static_cast<int>(left);
#if defined(_WIN32)
            const int r = ::send(to_handle(fd), reinterpret_cast<const char*>(data + done), chunk, 0);
#elif defined(MSG_NOSIGNAL)
            const ssize_t r = ::send(to_handle(fd), data + done, static_cast<std::size_t>(chunk), MSG_NOSIGNAL);
#else
            const ssize_t r = ::send(to_handle(fd), data + done, static_cast<std::size_t>(chunk), 0);
#endif
            if (r > 0) {
                done += static_cast<std::size_t>(r);
                continue;
            }
            const int e = last_error();
            if (r < 0 && interrupted(e)) continue;
            if (r < 0 && would_block(e)) {
                int wait_ms = timeout_ms;
                if (has_write_deadline_) {
                    const int until_deadline = remaining_ms(write_deadline_);
                    if (until_deadline == 0) return Status(Errc::Timeout, "write deadline passed");
                    wait_ms = std::min(clamp_timeout(wait_ms), until_deadline);
                }
                const int w = wait_one(to_handle(fd), POLLOUT, wait_ms, &shut_);
                if (w < 0) return Status(Errc::NetworkError, error_text("poll", last_error()));
                if (w == 0) return Status(Errc::Timeout, "write timed out");
                continue;
            }
            return Status(Errc::NetworkError, error_text("send", e));
        }
        return Status::ok();
    }

    void set_write_deadline(const std::chrono::steady_clock::time_point* deadline) noexcept override {
        has_write_deadline_ = deadline != nullptr;
        if (deadline) write_deadline_ = *deadline;
    }

    Result<bool> wait_readable(int timeout_ms) override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return Status(Errc::NetworkError, "socket closed");
        const int w = wait_one(to_handle(fd), POLLIN, timeout_ms, &shut_);
        if (w < 0) return Status(Errc::NetworkError, error_text("poll", last_error()));
        return w > 0;
    }

    void shutdown() noexcept override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return;
        shut_.store(true);
#if defined(_WIN32)
        (void)::shutdown(to_handle(fd), SD_BOTH);
#else
        (void)::shutdown(to_handle(fd), SHUT_RDWR);
#endif
    }

    void shutdown_send() noexcept override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return;
#if defined(_WIN32)
        (void)::shutdown(to_handle(fd), SD_SEND);
#else
        (void)::shutdown(to_handle(fd), SHUT_WR);
#endif
    }

    void close() noexcept override {
        const std::intptr_t fd = fd_.exchange(kInvalid);
        if (fd != kInvalid) close_handle(to_handle(fd));
    }

    bool is_open() const noexcept override { return fd_.load() != kInvalid; }

    std::string peer_address() const override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return "";
        sockaddr_storage ss{};
        socklen_t len = static_cast<socklen_t>(sizeof ss);
        if (::getpeername(to_handle(fd), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "?";
        return numeric_address(reinterpret_cast<sockaddr*>(&ss), len);
    }

private:
    std::atomic<std::intptr_t> fd_;
    std::atomic<bool> shut_{false};
    bool has_write_deadline_ = false;  // writer thread only
    Deadline write_deadline_{};
};

// Connects one candidate address with a deadline. Returns the connected handle or kInvalid.
bool refused_error(int e) {
#if defined(_WIN32)
    return e == WSAECONNREFUSED;
#else
    return e == ECONNREFUSED;
#endif
}

std::intptr_t connect_one(const addrinfo* ai, int timeout_ms, Status& err, const std::atomic<bool>* cancel,
                          ConnectError& why) {
    why = ConnectError::Other;
#if defined(_WIN32)
    const Handle h = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (h == INVALID_SOCKET) {
        err = Status(Errc::NetworkError, error_text("socket", last_error()));
        return kInvalid;
    }
#else
    const Handle h = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (h < 0) {
        err = Status(Errc::NetworkError, error_text("socket", last_error()));
        return kInvalid;
    }
#endif
    if (Status s = set_nonblocking(h); !s) {
        close_handle(h);
        err = s;
        return kInvalid;
    }
    tune_stream_socket(h);
    int r = ::connect(h, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    if (r != 0) {
        const int e = last_error();
        if (!would_block(e) && !interrupted(e)) {
            close_handle(h);
            if (refused_error(e)) why = ConnectError::Refused;
            err = Status(Errc::NetworkError, error_text("connect", e));
            return kInvalid;
        }
#if defined(_WIN32)
        // select() reports failed non-blocking connects reliably on every Windows version. It is
        // called in slices of at most 50 ms so that `cancel` is noticed.
        int w = 0;
        {
            using clock = std::chrono::steady_clock;
            const auto deadline = clock::now() + std::chrono::milliseconds(clamp_timeout(timeout_ms));
            for (;;) {
                if (cancel && cancel->load()) break;
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
                if (left <= 0) break;
                const long slice = cancel && left > 50 ? 50L : static_cast<long>(left > 0x7FFFFFFF ? 0x7FFFFFFF : left);
                fd_set wset{}, eset{};
                wset.fd_count = 1;
                wset.fd_array[0] = h;
                eset.fd_count = 1;
                eset.fd_array[0] = h;
                timeval tv{};
                tv.tv_sec = slice / 1000;
                tv.tv_usec = (slice % 1000) * 1000;
                w = ::select(0, nullptr, &wset, &eset, &tv);
                if (w != 0) break;
            }
        }
#else
        const int w = wait_one(h, POLLOUT, timeout_ms, cancel);
#endif
        if (cancel && cancel->load()) {
            close_handle(h);
            why = ConnectError::Cancelled;
            err = Status(Errc::Cancelled, "connect cancelled");
            return kInvalid;
        }
        if (w == 0) {
            close_handle(h);
            why = ConnectError::Timeout;
            err = Status(Errc::Timeout, "connect timed out");
            return kInvalid;
        }
        if (w < 0) {
            close_handle(h);
            err = Status(Errc::NetworkError, error_text("connect wait", last_error()));
            return kInvalid;
        }
        int so_error = 0;
        socklen_t len = static_cast<socklen_t>(sizeof so_error);
        if (::getsockopt(h, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len) != 0 || so_error != 0) {
            close_handle(h);
            if (refused_error(so_error)) why = ConnectError::Refused;
            err = Status(Errc::NetworkError, error_text("connect", so_error));
            return kInvalid;
        }
    }
    return static_cast<std::intptr_t>(h);
}

}  // namespace

Status socket_library_init() {
#if defined(_WIN32)
    static std::once_flag once;
    static int result = 0;
    std::call_once(once, [] {
        WSADATA data;
        result = ::WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (result != 0) return Status(Errc::NetworkError, error_text("WSAStartup", result));
#endif
    return Status::ok();
}

Deadline deadline_after(int timeout_ms) noexcept {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(clamp_timeout(timeout_ms));
}

int remaining_ms(Deadline d) noexcept {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(d - std::chrono::steady_clock::now()).count();
    if (left <= 0) return 0;
    if (left > INT_MAX) return INT_MAX;
    return static_cast<int>(left);
}

Status read_exact(Transport& t, std::uint8_t* buf, std::size_t n, int timeout_ms) {
    return read_exact_until(t, buf, n, deadline_after(timeout_ms));
}

Status read_exact_until(Transport& t, std::uint8_t* buf, std::size_t n, Deadline deadline) {
    std::size_t got = 0;
    while (got < n) {
        Result<std::size_t> r = t.read_some(buf + got, n - got, remaining_ms(deadline));
        if (!r) return r.take_error();
        if (*r == 0) return Status(Errc::NetworkError, "connection closed");
        got += *r;
        if (got < n && std::chrono::steady_clock::now() >= deadline)
            return Status(Errc::Timeout, "read deadline expired");
    }
    return Status::ok();
}

Result<std::unique_ptr<Transport>> tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    return tcp_connect(host, port, timeout_ms, nullptr, nullptr);
}

Result<std::unique_ptr<Transport>> tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms,
                                               const std::atomic<bool>* cancel, ConnectError* why) {
    ConnectError local = ConnectError::None;
    ConnectError& reason = why ? *why : local;
    reason = ConnectError::Other;
    if (Status s = socket_library_init(); !s) return s;
    if (host.empty()) return Status(Errc::InvalidArgument, "empty host");
    if (cancel && cancel->load()) {
        reason = ConnectError::Cancelled;
        return Status(Errc::Cancelled, "connect cancelled");
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    const int g = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
    if (g != 0 || list == nullptr) {
        reason = ConnectError::Resolve;
        return Status(Errc::NetworkError, "cannot resolve host");
    }
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(clamp_timeout(timeout_ms));
    Status err(Errc::NetworkError, "no usable address");
    std::intptr_t fd = kInvalid;
    for (addrinfo* ai = list; ai != nullptr && fd == kInvalid; ai = ai->ai_next) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) {
            reason = ConnectError::Timeout;
            err = Status(Errc::Timeout, "connect timed out");
            break;
        }
        fd = connect_one(ai, static_cast<int>(left > 0x7FFFFFFF ? 0x7FFFFFFF : left), err, cancel, reason);
        if (fd == kInvalid && reason == ConnectError::Cancelled) break;
    }
    ::freeaddrinfo(list);
    if (fd == kInvalid) return err;
    reason = ConnectError::None;
    return std::unique_ptr<Transport>(new TcpTransport(to_handle(fd)));
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::is_open() const noexcept { return fd_ != kInvalid; }

void TcpListener::close() noexcept {
    if (fd_ != kInvalid) {
        close_handle(to_handle(fd_));
        fd_ = kInvalid;
    }
}

Status TcpListener::listen(const std::string& bind_address, std::uint16_t port, int backlog) {
    if (Status s = socket_library_init(); !s) return s;
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    const std::string service = std::to_string(port);
    // Empty address: try the IPv6 wildcard first (dual stack), then IPv4.
    const char* candidates[2] = {"::", "0.0.0.0"};
    const int ncand = bind_address.empty() ? 2 : 1;
    Status err(Errc::NetworkError, "cannot listen");
    bool in_use = false;  // a candidate address failed because the port is taken
    for (int c = 0; c < ncand; ++c) {
        const char* addr = bind_address.empty() ? candidates[c] : bind_address.c_str();
        addrinfo* list = nullptr;
        if (::getaddrinfo(addr, service.c_str(), &hints, &list) != 0 || list == nullptr) {
            err = Status(Errc::NetworkError, "cannot resolve bind address");
            continue;
        }
        for (addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
            const Handle h = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (static_cast<std::intptr_t>(h) == kInvalid) {
                err = Status(Errc::NetworkError, error_text("socket", last_error()));
                continue;
            }
            int one = 1;
#if defined(_WIN32)
            (void)::setsockopt(h, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), static_cast<socklen_t>(sizeof one));
#else
            (void)::setsockopt(h, SOL_SOCKET, SO_REUSEADDR, &one, static_cast<socklen_t>(sizeof one));
#endif
            if (ai->ai_family == AF_INET6) {
                int zero = 0;
                (void)::setsockopt(h, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), static_cast<socklen_t>(sizeof zero));
            }
            if (::bind(h, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0 ||
                ::listen(h, backlog) != 0) {
                const int e = last_error();
#if defined(_WIN32)
                // With SO_EXCLUSIVEADDRUSE a port held by another socket gives WSAEADDRINUSE, or
                // WSAEACCES when that socket belongs to another user.
                const bool taken = e == WSAEADDRINUSE || e == WSAEACCES;
#else
                const bool taken = e == EADDRINUSE;
#endif
                if (taken) in_use = true;
                err = Status(Errc::NetworkError, error_text("bind/listen", e));
                close_handle(h);
                continue;
            }
            if (Status s = set_nonblocking(h); !s) {
                close_handle(h);
                err = s;
                continue;
            }
            sockaddr_storage ss{};
            socklen_t len = static_cast<socklen_t>(sizeof ss);
            if (::getsockname(h, reinterpret_cast<sockaddr*>(&ss), &len) == 0) {
                if (ss.ss_family == AF_INET)
                    port_ = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
                else if (ss.ss_family == AF_INET6)
                    port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
            }
            fd_ = static_cast<std::intptr_t>(h);
            ::freeaddrinfo(list);
            return Status::ok();
        }
        ::freeaddrinfo(list);
    }
    // C6: a taken port is reported as such, with the port, so a caller can tell the user.
    if (in_use) return Status(Errc::AddressInUse, "port " + std::to_string(port) + " is already in use");
    return err;
}

Result<std::unique_ptr<Transport>> TcpListener::accept(int timeout_ms) {
    if (fd_ == kInvalid) return Status(Errc::NetworkError, "listener closed");
    const int w = wait_one(to_handle(fd_), POLLIN, timeout_ms);
    if (w < 0) return Status(Errc::NetworkError, error_text("poll", last_error()));
    if (w == 0) return Status(Errc::Timeout, "no connection");
    sockaddr_storage ss{};
    socklen_t len = static_cast<socklen_t>(sizeof ss);
    const Handle h = ::accept(to_handle(fd_), reinterpret_cast<sockaddr*>(&ss), &len);
    if (static_cast<std::intptr_t>(h) == kInvalid) {
        const int e = last_error();
        if (would_block(e) || interrupted(e)) return Status(Errc::Timeout, "no connection");
        return Status(Errc::NetworkError, error_text("accept", e));
    }
    if (Status s = set_nonblocking(h); !s) {
        close_handle(h);
        return s;
    }
    tune_stream_socket(h);
    return std::unique_ptr<Transport>(new TcpTransport(h));
}

namespace {
// Parses an IPv4 or IPv6 literal (brackets and an IPv6 zone index allowed). family 4 or 6, 0 if neither.
int parse_ip(std::string_view a, unsigned char out[16]) noexcept {
    if (a.size() >= 2 && a.front() == '[' && a.back() == ']') a = a.substr(1, a.size() - 2);
    if (const auto pct = a.find('%'); pct != std::string_view::npos) a = a.substr(0, pct);
    if (a.empty() || a.size() > 63) return 0;
    char buf[64];
    std::memcpy(buf, a.data(), a.size());
    buf[a.size()] = '\0';
    if (::inet_pton(AF_INET, buf, out) == 1) return 4;
    if (::inet_pton(AF_INET6, buf, out) == 1) return 6;
    return 0;
}
bool v4_mapped(const unsigned char a[16]) noexcept {
    for (int i = 0; i < 10; ++i)
        if (a[i] != 0) return false;
    return a[10] == 0xFF && a[11] == 0xFF;
}
}  // namespace

std::string peer_group(std::string_view address, unsigned ipv6_prefix_bits) {
    unsigned char a[16] = {};
    const int fam = parse_ip(address, a);
    if (fam == 0) return std::string(address);
    if (fam == 4 || v4_mapped(a)) {
        const unsigned char* v = fam == 4 ? a : a + 12;
        return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]) + "." + std::to_string(v[3]);
    }
    const unsigned bits = ipv6_prefix_bits == 0 || ipv6_prefix_bits > 128 ? 128u : ipv6_prefix_bits;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char b = a[i];
        if (8 * i >= bits) b = 0;
        else if (8 * (i + 1) > bits) b = static_cast<unsigned char>(b & (0xFFu << (8 * (i + 1) - bits)));
        out += hex[b >> 4];
        out += hex[b & 15u];
    }
    return out + "/" + std::to_string(bits);
}

bool is_loopback_literal(std::string_view host) noexcept {
    if (host == "localhost") return true;
    unsigned char a[16] = {};
    const int fam = parse_ip(host, a);
    if (fam == 4) return a[0] == 127;
    if (fam == 6) {
        if (v4_mapped(a)) return a[12] == 127;
        for (int i = 0; i < 15; ++i)
            if (a[i] != 0) return false;
        return a[15] == 1;
    }
    return false;
}

}  // namespace siaara::net
