#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace sigflow {

// Everything that can go wrong while encoding, decoding or moving bytes
// around. Kept as a closed set so callers can switch over it, the free text
// in Error::detail is only meant for humans and logs.
enum class Errc : std::uint8_t {
    truncated,
    bad_magic,
    unsupported_version,
    crc_mismatch,
    payload_too_large,
    field_too_large,
    bad_field_length,
    missing_field,
    duplicate_field,
    invalid_value,
    unexpected_message,
    io_error,
};

std::string_view to_string(Errc code) noexcept;

struct Error {
    Errc code;
    std::string detail;

    // Prefixes the detail with where the error happened, so a failure deep
    // inside a nested struct still reads like "MeasurementReport.neighbours:
    // field has 3 bytes, expected 4".
    [[nodiscard]] Error with_context(std::string_view where) const {
        std::string text(where);
        if (!detail.empty()) {
            text += ": ";
            text += detail;
        }
        return Error{code, std::move(text)};
    }

    [[nodiscard]] std::string message() const {
        std::string text(to_string(code));
        if (!detail.empty()) {
            text += " (";
            text += detail;
            text += ")";
        }
        return text;
    }

    bool operator==(const Error&) const = default;
};

// A value or an error, in the spirit of C++23 std::expected. Hand rolled
// because the library targets C++20 and the subset needed here is tiny.
// Exceptions are avoided on the decode path on purpose: malformed input
// from the network is an expected situation, not an exceptional one.
template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> would be ambiguous");

public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const& { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }

    const Error& error() const& { return std::get<1>(storage_); }
    Error&& error() && { return std::get<1>(std::move(storage_)); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    std::variant<T, Error> storage_;
};

// Result for operations that produce nothing besides success or failure.
class [[nodiscard]] Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    static Status success() { return {}; }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const& { return *error_; }
    Error&& error() && { return std::move(*error_); }

private:
    std::optional<Error> error_;
};

inline Error make_error(Errc code, std::string detail = {}) {
    return Error{code, std::move(detail)};
}

}  // namespace sigflow

// Propagates a failed Status or Result to the caller. Works in any function
// that returns Status or Result<U>, since both can be built from an Error.
// A macro is the only way to get an early return without exceptions.
#define SIGFLOW_TRY(expr)                                              \
    do {                                                               \
        if (auto sigflow_try_result_ = (expr); !sigflow_try_result_) { \
            return std::move(sigflow_try_result_).error();             \
        }                                                              \
    } while (false)
