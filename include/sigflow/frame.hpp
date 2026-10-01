#pragma once

// Every message travels inside a frame:
//
//   offset  size  field
//   0       2     magic 0x5346 ("SF")
//   2       1     protocol version
//   3       1     flags (reserved, always 0 for now)
//   4       2     message id
//   6       2     sequence number
//   8       4     payload length
//   12      N     payload (TLV encoded message body)
//   12+N    2     CRC-16/CCITT-FALSE over header and payload
//
// All multi byte fields are big endian. The magic and the CRC are there
// for stream transports, where a frame boundary has to be found again
// after a corrupted or partial read. UDP would not strictly need them, but
// one format for every transport keeps things simple.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "sigflow/byte_io.hpp"
#include "sigflow/result.hpp"

namespace sigflow {

inline constexpr std::uint16_t kFrameMagic = 0x5346;
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 12;
inline constexpr std::size_t kFrameTrailerSize = 2;
inline constexpr std::size_t kFrameOverhead = kFrameHeaderSize + kFrameTrailerSize;

// Upper bound for a single payload. Without it a corrupted length field
// could make the receiver wait for (or allocate) gigabytes of data.
inline constexpr std::size_t kMaxPayloadSize = 64 * 1024;

struct FrameHeader {
    std::uint16_t message_id = 0;
    std::uint16_t sequence = 0;
    std::uint32_t payload_size = 0;

    bool operator==(const FrameHeader&) const = default;
};

struct Frame {
    FrameHeader header;
    Bytes payload;

    bool operator==(const Frame&) const = default;
};

std::uint16_t crc16_ccitt(ByteView data, std::uint16_t crc = 0xFFFF) noexcept;

Result<Bytes> encode_frame(std::uint16_t message_id, std::uint16_t sequence, ByteView payload);

// Decodes a buffer that is expected to hold exactly one frame, which is the
// case for datagram transports like UDP.
Result<Frame> decode_frame(ByteView data);

// Rebuilds frames from a byte stream (TCP, a pipe, a serial line). Bytes
// can arrive in arbitrary chunks, so data is buffered until a full frame is
// available. On garbage or a CRC error the assembler drops one byte and
// searches for the next magic value, so a single bad frame does not kill
// the whole connection.
class FrameAssembler {
public:
    void feed(ByteView bytes);

    // Returns the next complete and valid frame, or nothing if more data is
    // needed first.
    std::optional<Frame> next();

    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - start_; }
    [[nodiscard]] std::size_t dropped_bytes() const noexcept { return dropped_bytes_; }
    [[nodiscard]] std::size_t crc_errors() const noexcept { return crc_errors_; }

private:
    void drop(std::size_t count);

    // Consumed bytes are not erased right away. start_ marks where the
    // unread data begins and the buffer is compacted once that prefix gets
    // big, which avoids shifting memory after every single frame.
    Bytes buffer_;
    std::size_t start_ = 0;
    std::size_t dropped_bytes_ = 0;
    std::size_t crc_errors_ = 0;
};

}  // namespace sigflow
