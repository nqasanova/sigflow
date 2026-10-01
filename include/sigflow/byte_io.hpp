#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sigflow/result.hpp"

namespace sigflow {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// Appends values to a growing buffer in network byte order (big endian).
// The byte order is produced with shifts instead of htons and friends, so
// the result is the same on every host and no platform headers are needed.
class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserve) { buffer_.reserve(reserve); }

    void put_u8(std::uint8_t value) { buffer_.push_back(value); }

    template <std::unsigned_integral T>
    void put_be(T value) {
        for (std::size_t i = sizeof(T); i > 0; i--) {
            buffer_.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
        }
    }

    void put_bytes(ByteView bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }

    // Overwrites a value that was written earlier. Used for length fields
    // that are only known once the content after them has been written.
    template <std::unsigned_integral T>
    void patch_be(std::size_t offset, T value) {
        for (std::size_t i = 0; i < sizeof(T); i++) {
            buffer_.at(offset + i) = static_cast<std::uint8_t>(value >> ((sizeof(T) - 1 - i) * 8));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] ByteView view() const noexcept { return buffer_; }
    [[nodiscard]] Bytes take() && noexcept { return std::move(buffer_); }

private:
    Bytes buffer_;
};

// Reads big endian values from a byte span without ever going past its end.
// Every read is bounds checked and reports Errc::truncated instead of
// touching memory it does not own, since the input usually comes straight
// from a socket.
class ByteReader {
public:
    explicit ByteReader(ByteView data) noexcept : data_(data) {}

    template <std::unsigned_integral T>
    Result<T> read_be() {
        if (remaining() < sizeof(T)) {
            return make_error(Errc::truncated,
                              "need " + std::to_string(sizeof(T)) + " bytes, have " + std::to_string(remaining()));
        }
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); i++) {
            value = static_cast<T>((value << 8) | data_[offset_ + i]);
        }
        offset_ += sizeof(T);
        return value;
    }

    Result<ByteView> read_bytes(std::size_t count) {
        if (remaining() < count) {
            return make_error(Errc::truncated,
                              "need " + std::to_string(count) + " bytes, have " + std::to_string(remaining()));
        }
        ByteView view = data_.subspan(offset_, count);
        offset_ += count;
        return view;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
    [[nodiscard]] std::size_t position() const noexcept { return offset_; }
    [[nodiscard]] bool empty() const noexcept { return remaining() == 0; }

private:
    ByteView data_;
    std::size_t offset_ = 0;
};

}  // namespace sigflow
