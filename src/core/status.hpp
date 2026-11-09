// SIAARA DICOM Toolkit - error reporting (no exceptions on library paths, see standards CPP-20)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace siaara {

// Stable numeric codes; these values are part of the C ABI (see include/siaara/siaara.h).
enum class Errc : int {
    Ok = 0,
    InvalidArgument = 1,
    IoError = 2,
    NotDicom = 3,
    Truncated = 4,
    Malformed = 5,
    Unsupported = 6,
    NotFound = 7,
    TypeMismatch = 8,
    LimitExceeded = 9,
    OutOfMemory = 10,
    NetworkError = 11,
    Timeout = 12,
    Rejected = 13,
    Cancelled = 14,
    BufferTooSmall = 15,
    Internal = 99,
};

struct [[nodiscard]] Status {
    Errc code = Errc::Ok;
    std::string message;
    std::uint64_t offset = 0;  // byte offset in the input, when parsing
    std::uint32_t tag = 0;     // tag involved, when known

    Status() = default;
    Status(Errc c, std::string m, std::uint64_t off = 0, std::uint32_t t = 0)
        : code(c), message(std::move(m)), offset(off), tag(t) {}
    static Status ok() { return {}; }
    // Errc::OutOfMemory for catch handlers: never throws. The message is set only if storing it
    // succeeds (it may need memory itself); otherwise the status has the code and no message.
    static Status out_of_memory(const char* msg) noexcept {
        Status s;
        s.code = Errc::OutOfMemory;
        try {
            s.message = msg;
        } catch (...) {
        }
        return s;
    }
    bool is_ok() const noexcept { return code == Errc::Ok; }
    explicit operator bool() const noexcept { return is_ok(); }
};

// Minimal expected-like type (std::expected is C++23).
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::move(value)) {}            // NOLINT(google-explicit-constructor)
    Result(Status err) : v_(std::move(err)) {}           // NOLINT(google-explicit-constructor)
    bool is_ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return is_ok(); }
    T& value() & { return std::get<0>(v_); }
    const T& value() const& { return std::get<0>(v_); }
    T&& value() && { return std::get<0>(std::move(v_)); }
    T* operator->() { return &std::get<0>(v_); }
    const T* operator->() const { return &std::get<0>(v_); }
    T& operator*() & { return std::get<0>(v_); }
    const Status& error() const { return std::get<1>(v_); }
    Status take_error() { return std::get<1>(std::move(v_)); }

private:
    std::variant<T, Status> v_;
};

// Checked arithmetic for input-derived sizes (standards CPP-10).
inline bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    out = a + b;
    return out >= a;
}
inline bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > UINT64_MAX / a) return false;
    out = a * b;
    return true;
}

}  // namespace siaara
