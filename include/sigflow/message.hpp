#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

#include "sigflow/byte_io.hpp"
#include "sigflow/frame.hpp"
#include "sigflow/result.hpp"

namespace sigflow {

// What the code generator produces for every top level message. Any type
// that satisfies this can be sent through an Endpoint, so hand written
// messages work too, as long as they follow the same shape.
template <typename T>
concept Message = requires(const T& message, ByteWriter& out, ByteView payload) {
    { T::kMessageId } -> std::convertible_to<std::uint16_t>;
    { T::kName } -> std::convertible_to<std::string_view>;
    { message.encode(out) } -> std::same_as<Status>;
    { T::decode(payload) } -> std::same_as<Result<T>>;
};

template <Message T>
Result<Bytes> encode_message(const T& message, std::uint16_t sequence) {
    ByteWriter body;
    SIGFLOW_TRY(message.encode(body));
    return encode_frame(T::kMessageId, sequence, body.view());
}

template <Message T>
Result<T> decode_message(const Frame& frame) {
    if (frame.header.message_id != T::kMessageId) {
        return make_error(Errc::unexpected_message,
                          "expected " + std::string(T::kName) + ", got id " + std::to_string(frame.header.message_id));
    }
    return T::decode(frame.payload);
}

}  // namespace sigflow
