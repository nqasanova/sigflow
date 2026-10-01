#include "sigflow/frame.hpp"

#include <array>
#include <string>

namespace sigflow {
namespace {

// Lookup table for CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no
// reflection). Built at compile time, so there is no init order to worry
// about and no 512 byte literal to copy from somewhere.
constexpr std::array<std::uint16_t, 256> make_crc_table() {
    std::array<std::uint16_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; i++) {
        auto crc = static_cast<std::uint16_t>(i << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc =
                (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto kCrcTable = make_crc_table();

// Header fields that can be checked before the payload has arrived.
struct HeaderCheck {
    FrameHeader header;
    std::optional<Error> error;
};

HeaderCheck parse_header(ByteView data) {
    ByteReader reader(data.first(kFrameHeaderSize));
    // The reader cannot fail here because the caller guarantees at least
    // kFrameHeaderSize bytes, hence the unchecked value() calls.
    const auto magic = reader.read_be<std::uint16_t>().value();
    const auto version = reader.read_be<std::uint8_t>().value();
    (void)reader.read_be<std::uint8_t>();  // flags, reserved
    FrameHeader header;
    header.message_id = reader.read_be<std::uint16_t>().value();
    header.sequence = reader.read_be<std::uint16_t>().value();
    header.payload_size = reader.read_be<std::uint32_t>().value();

    if (magic != kFrameMagic) return {header, make_error(Errc::bad_magic)};
    if (version != kProtocolVersion) {
        return {header, make_error(Errc::unsupported_version, "version " + std::to_string(version))};
    }
    if (header.payload_size > kMaxPayloadSize) {
        return {header, make_error(Errc::payload_too_large, std::to_string(header.payload_size) + " bytes")};
    }
    return {header, std::nullopt};
}

}  // namespace

std::uint16_t crc16_ccitt(ByteView data, std::uint16_t crc) noexcept {
    for (std::uint8_t byte : data) {
        crc = static_cast<std::uint16_t>((crc << 8) ^ kCrcTable[((crc >> 8) ^ byte) & 0xFF]);
    }
    return crc;
}

Result<Bytes> encode_frame(std::uint16_t message_id, std::uint16_t sequence, ByteView payload) {
    if (payload.size() > kMaxPayloadSize) {
        return make_error(Errc::payload_too_large, std::to_string(payload.size()) + " bytes");
    }

    ByteWriter out(kFrameOverhead + payload.size());
    out.put_be(kFrameMagic);
    out.put_u8(kProtocolVersion);
    out.put_u8(0);  // flags
    out.put_be(message_id);
    out.put_be(sequence);
    out.put_be(static_cast<std::uint32_t>(payload.size()));
    out.put_bytes(payload);
    out.put_be(crc16_ccitt(out.view()));
    return std::move(out).take();
}

Result<Frame> decode_frame(ByteView data) {
    if (data.size() < kFrameOverhead) {
        return make_error(Errc::truncated, "frame has only " + std::to_string(data.size()) + " bytes");
    }

    auto [header, error] = parse_header(data);
    if (error) return *error;

    const std::size_t expected = kFrameOverhead + header.payload_size;
    if (data.size() != expected) {
        return make_error(
            data.size() < expected ? Errc::truncated : Errc::invalid_value,
            "frame is " + std::to_string(data.size()) + " bytes, header says " + std::to_string(expected));
    }

    const std::size_t crc_offset = kFrameHeaderSize + header.payload_size;
    ByteReader trailer(data.subspan(crc_offset));
    const auto received_crc = trailer.read_be<std::uint16_t>().value();
    const auto computed_crc = crc16_ccitt(data.first(crc_offset));
    if (received_crc != computed_crc) return make_error(Errc::crc_mismatch);

    auto payload = data.subspan(kFrameHeaderSize, header.payload_size);
    return Frame{header, Bytes(payload.begin(), payload.end())};
}

void FrameAssembler::feed(ByteView bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void FrameAssembler::drop(std::size_t count) {
    start_ += count;
    dropped_bytes_ += count;
}

std::optional<Frame> FrameAssembler::next() {
    std::optional<Frame> result;

    while (!result) {
        ByteView pending(buffer_.data() + start_, buffer_.size() - start_);

        // Skip ahead to the first possible magic value. A lone 0x53 at the
        // very end is kept, it might be the first half of the next frame.
        std::size_t skip = 0;
        while (skip + 1 < pending.size() &&
               !(pending[skip] == (kFrameMagic >> 8) && pending[skip + 1] == (kFrameMagic & 0xFF))) {
            skip++;
        }
        if (skip > 0) {
            drop(skip);
            continue;
        }
        if (pending.size() < kFrameHeaderSize) break;

        auto [header, error] = parse_header(pending);
        if (error) {
            // Magic matched by accident inside other data. Move one byte
            // forward and keep searching.
            drop(1);
            continue;
        }

        const std::size_t total = kFrameOverhead + header.payload_size;
        if (pending.size() < total) break;  // wait for the rest

        auto frame = decode_frame(pending.first(total));
        if (!frame) {
            crc_errors_++;
            drop(1);
            continue;
        }
        start_ += total;
        result = std::move(*frame);
    }

    // Compact once the consumed prefix outweighs the live data.
    if (start_ > 0 && start_ >= buffer_.size() - start_) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(start_));
        start_ = 0;
    }
    return result;
}

}  // namespace sigflow
