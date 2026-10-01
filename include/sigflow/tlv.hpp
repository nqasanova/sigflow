#pragma once

// Tag Length Value encoding used for message bodies.
//
//   +--------+----------------+-------------------+
//   | tag u8 | length u16 BE  | value (length B)  |
//   +--------+----------------+-------------------+
//
// Fields can appear in any order and unknown tags are skipped by the
// decoder. That is what makes the format extensible: a newer peer can add
// fields and an older one keeps working, which is the same idea behind the
// extension markers in ASN.1 based protocols.

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "sigflow/byte_io.hpp"
#include "sigflow/result.hpp"

namespace sigflow {

inline constexpr std::size_t kTlvHeaderSize = 3;
inline constexpr std::size_t kMaxFieldSize = 0xFFFF;

struct TlvField {
    std::uint8_t tag;
    ByteView value;  // points into the buffer that was parsed, does not own it
};

// Splits a message body into its fields. Only the framing is checked here,
// the meaning of each value is up to the generated decoder.
Result<std::vector<TlvField>> parse_tlv(ByteView payload);

namespace detail {

template <typename T>
inline constexpr bool is_byte_vector = std::is_same_v<T, Bytes>;

// Generated structs expose encode/decode, which is all that is needed to
// nest one inside another.
template <typename T>
concept Encodable = requires(const T& value, ByteWriter& out, ByteView in) {
    { value.encode(out) } -> std::same_as<Status>;
    { T::decode(in) } -> std::same_as<Result<T>>;
};

// Enums get an is_valid() overload from the code generator, found via ADL.
template <typename E>
concept CheckedEnum = std::is_enum_v<E> && requires(E e) {
    { is_valid(e) } -> std::same_as<bool>;
};

Error bad_length(std::size_t actual, std::size_t expected);

}  // namespace detail

class TlvWriter {
public:
    explicit TlvWriter(ByteWriter& out) noexcept : out_(out) {}

    // One entry point for every supported field type. The generated code
    // calls put(tag, field) and the right encoding is picked at compile
    // time, which keeps the code generator itself very simple.
    template <typename T>
    Status put(std::uint8_t tag, const T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            put_header(tag, 1);
            out_.put_u8(value ? 1 : 0);
        } else if constexpr (std::is_enum_v<T>) {
            return put(tag, static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_integral_v<T>) {
            // Signed values go out as their two's complement bit pattern.
            put_header(tag, sizeof(T));
            out_.put_be(static_cast<std::make_unsigned_t<T>>(value));
        } else if constexpr (std::is_same_v<T, std::string>) {
            return put_raw(tag, ByteView(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        } else if constexpr (detail::is_byte_vector<T>) {
            return put_raw(tag, value);
        } else if constexpr (detail::Encodable<T>) {
            ByteWriter nested;
            SIGFLOW_TRY(value.encode(nested));
            return put_raw(tag, nested.view());
        } else {
            static_assert(sizeof(T) == 0, "type cannot be encoded as a TLV field");
        }
        return Status::success();
    }

private:
    void put_header(std::uint8_t tag, std::uint16_t length) {
        out_.put_u8(tag);
        out_.put_be(length);
    }

    Status put_raw(std::uint8_t tag, ByteView bytes);

    ByteWriter& out_;
};

// Counterpart of TlvWriter::put. Checks that the field has exactly the size
// the type needs, a wrong length usually means both sides disagree on the
// schema, and silently reading half a value would hide that.
template <typename T>
Result<T> read_value(const TlvField& field) {
    if constexpr (std::is_same_v<T, bool>) {
        if (field.value.size() != 1) return detail::bad_length(field.value.size(), 1);
        if (field.value[0] > 1) return make_error(Errc::invalid_value, "bool must be 0 or 1");
        return field.value[0] == 1;
    } else if constexpr (std::is_enum_v<T>) {
        using Underlying = std::underlying_type_t<T>;
        auto raw = read_value<Underlying>(field);
        if (!raw) return std::move(raw).error();
        auto value = static_cast<T>(*raw);
        if constexpr (detail::CheckedEnum<T>) {
            if (!is_valid(value)) {
                return make_error(Errc::invalid_value, "unknown enum value " + std::to_string(*raw));
            }
        }
        return value;
    } else if constexpr (std::is_integral_v<T>) {
        if (field.value.size() != sizeof(T)) return detail::bad_length(field.value.size(), sizeof(T));
        ByteReader reader(field.value);
        auto raw = reader.read_be<std::make_unsigned_t<T>>();
        if (!raw) return std::move(raw).error();
        return static_cast<T>(*raw);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return std::string(field.value.begin(), field.value.end());
    } else if constexpr (detail::is_byte_vector<T>) {
        return Bytes(field.value.begin(), field.value.end());
    } else if constexpr (detail::Encodable<T>) {
        return T::decode(field.value);
    } else {
        static_assert(sizeof(T) == 0, "type cannot be decoded from a TLV field");
    }
}

}  // namespace sigflow
