// SIAARA DICOM Toolkit - TCP transport for POSIX sockets and Winsock2.
#include "net/socket.hpp"

#include <chrono>
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
int wait_one(Handle h, short events, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(clamp_timeout(timeout_ms));
    while (true) {
        PollFd p{};
        p.fd = h;
        p.events = events;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        const int wait_ms = left <= 0 ? 0 : (left > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                                       : static_cast<int>(left));
        const int r = do_poll(&p, 1, wait_ms);
        if (r < 0) {
            if (interrupted(last_error())) continue;
            return -1;
        }
        if (r == 0) return 0;
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
            const int w = wait_one(to_handle(fd), POLLIN, timeout_ms);
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
                const int w = wait_one(to_handle(fd), POLLOUT, timeout_ms);
                if (w < 0) return Status(Errc::NetworkError, error_text("poll", last_error()));
                if (w == 0) return Status(Errc::Timeout, "write timed out");
                continue;
            }
            return Status(Errc::NetworkError, error_text("send", e));
        }
        return Status::ok();
    }

    Result<bool> wait_readable(int timeout_ms) override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return Status(Errc::NetworkError, "socket closed");
        const int w = wait_one(to_handle(fd), POLLIN, timeout_ms);
        if (w < 0) return Status(Errc::NetworkError, error_text("poll", last_error()));
        return w > 0;
    }

    void shutdown() noexcept override {
        const std::intptr_t fd = fd_.load();
        if (fd == kInvalid) return;
#if defined(_WIN32)
        (void)::shutdown(to_handle(fd), SD_BOTH);
#else
        (void)::shutdown(to_handle(fd), SHUT_RDWR);
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
};

// Connects one candidate address with a deadline. Returns the connected handle or kInvalid.
std::intptr_t connect_one(const addrinfo* ai, int timeout_ms, Status& err) {
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
            err = Status(Errc::NetworkError, error_text("connect", e));
            return kInvalid;
        }
#if defined(_WIN32)
        // select() reports failed non-blocking connects reliably on every Windows version.
        fd_set wset{}, eset{};
        wset.fd_count = 1;
        wset.fd_array[0] = h;
        eset.fd_count = 1;
        eset.fd_array[0] = h;
        timeval tv{};
        tv.tv_sec = clamp_timeout(timeout_ms) / 1000;
        tv.tv_usec = (clamp_timeout(timeout_ms) % 1000) * 1000;
        const int w = ::select(0, nullptr, &wset, &eset, &tv);
#else
        const int w = wait_one(h, POLLOUT, timeout_ms);
#endif
        if (w == 0) {
            close_handle(h);
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

Status read_exact(Transport& t, std::uint8_t* buf, std::size_t n, int timeout_ms) {
    std::size_t got = 0;
    while (got < n) {
        Result<std::size_t> r = t.read_some(buf + got, n - got, timeout_ms);
        if (!r) return r.take_error();
        if (*r == 0) return Status(Errc::NetworkError, "connection closed");
        got += *r;
    }
    return Status::ok();
}

Result<std::unique_ptr<Transport>> tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    if (Status s = socket_library_init(); !s) return s;
    if (host.empty()) return Status(Errc::InvalidArgument, "empty host");
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    const int g = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
    if (g != 0 || list == nullptr) return Status(Errc::NetworkError, "cannot resolve host");
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(clamp_timeout(timeout_ms));
    Status err(Errc::NetworkError, "no usable address");
    std::intptr_t fd = kInvalid;
    for (addrinfo* ai = list; ai != nullptr && fd == kInvalid; ai = ai->ai_next) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) {
            err = Status(Errc::Timeout, "connect timed out");
            break;
        }
        fd = connect_one(ai, static_cast<int>(left > 0x7FFFFFFF ? 0x7FFFFFFF : left), err);
    }
    ::freeaddrinfo(list);
    if (fd == kInvalid) return err;
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
                err = Status(Errc::NetworkError, error_text("bind/listen", last_error()));
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

}  // namespace siaara::net
