#include "sigflow/frame.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string_view>

namespace sigflow {
namespace {

Bytes make_frame(std::uint16_t id, std::uint16_t sequence, const Bytes& payload) {
    auto frame = encode_frame(id, sequence, payload);
    EXPECT_TRUE(frame.ok());
    return std::move(frame).value();
}

TEST(Crc16, MatchesStandardCheckValue) {
    // "123456789" is the usual check input, CRC-16/CCITT-FALSE gives 0x29B1.
    constexpr std::string_view input = "123456789";
    const Bytes data(input.begin(), input.end());
    EXPECT_EQ(crc16_ccitt(data), 0x29B1);
}

TEST(Frame, HeaderLayout) {
    const Bytes frame = make_frame(0x0102, 0x0304, {0xAA, 0xBB});
    ASSERT_EQ(frame.size(), kFrameOverhead + 2);

    const Bytes header(frame.begin(), frame.begin() + kFrameHeaderSize);
    const Bytes expected = {0x53, 0x46, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x02};
    EXPECT_EQ(header, expected);
}

TEST(Frame, RoundTrip) {
    const Bytes payload = {1, 2, 3, 4, 5};
    auto frame = decode_frame(make_frame(42, 7, payload));
    ASSERT_TRUE(frame.ok()) << frame.error().message();
    EXPECT_EQ(frame->header.message_id, 42);
    EXPECT_EQ(frame->header.sequence, 7);
    EXPECT_EQ(frame->payload, payload);
}

TEST(Frame, DetectsEverySingleBitFlip) {
    // CRC-16 catches all single bit errors, so flipping any bit anywhere
    // must make decoding fail one way or another.
    const Bytes original = make_frame(0x0101, 1, {0x10, 0x20, 0x30});
    for (std::size_t byte = 0; byte < original.size(); byte++) {
        for (int bit = 0; bit < 8; bit++) {
            Bytes corrupted = original;
            corrupted[byte] = static_cast<std::uint8_t>(corrupted[byte] ^ (1 << bit));
            EXPECT_FALSE(decode_frame(corrupted).ok()) << "byte " << byte << " bit " << bit;
        }
    }
}

TEST(Frame, RejectsBadMagicAndVersion) {
    Bytes frame = make_frame(1, 1, {});
    frame[0] = 0x00;
    EXPECT_EQ(decode_frame(frame).error().code, Errc::bad_magic);

    frame = make_frame(1, 1, {});
    frame[2] = 9;
    EXPECT_EQ(decode_frame(frame).error().code, Errc::unsupported_version);
}

TEST(Frame, RejectsTruncatedAndOversized) {
    Bytes frame = make_frame(1, 1, {1, 2, 3});
    frame.pop_back();
    EXPECT_EQ(decode_frame(frame).error().code, Errc::truncated);

    const Bytes too_big(kMaxPayloadSize + 1, 0);
    EXPECT_EQ(encode_frame(1, 1, too_big).error().code, Errc::payload_too_large);
}

// FrameAssembler -------------------------------------------------------------

TEST(FrameAssembler, HandlesOneByteAtATime) {
    const Bytes frame = make_frame(5, 9, {0xCA, 0xFE});
    FrameAssembler assembler;

    for (std::size_t i = 0; i + 1 < frame.size(); i++) {
        assembler.feed(ByteView(&frame[i], 1));
        EXPECT_FALSE(assembler.next().has_value()) << "frame complete too early at byte " << i;
    }
    assembler.feed(ByteView(&frame.back(), 1));

    auto result = assembler.next();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.message_id, 5);
    EXPECT_EQ(assembler.buffered(), 0u);
}

TEST(FrameAssembler, SplitsBackToBackFrames) {
    Bytes stream = make_frame(1, 0, {0x01});
    const Bytes second = make_frame(2, 1, {0x02, 0x02});
    stream.insert(stream.end(), second.begin(), second.end());

    FrameAssembler assembler;
    assembler.feed(stream);
    EXPECT_EQ(assembler.next()->header.message_id, 1);
    EXPECT_EQ(assembler.next()->header.message_id, 2);
    EXPECT_FALSE(assembler.next().has_value());
}

TEST(FrameAssembler, SkipsGarbageBeforeFrame) {
    Bytes stream = {0x00, 0x53, 0x11, 0x46, 0xFF};
    const Bytes frame = make_frame(3, 0, {0x33});
    stream.insert(stream.end(), frame.begin(), frame.end());

    FrameAssembler assembler;
    assembler.feed(stream);
    auto result = assembler.next();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.message_id, 3);
    EXPECT_EQ(assembler.dropped_bytes(), 5u);
}

TEST(FrameAssembler, RecoversAfterCorruptedFrame) {
    Bytes broken = make_frame(1, 0, {0x01, 0x02, 0x03});
    broken[kFrameHeaderSize + 1] ^= 0xFF;  // damage the payload
    const Bytes good = make_frame(2, 1, {0x04});

    FrameAssembler assembler;
    assembler.feed(broken);
    assembler.feed(good);

    auto result = assembler.next();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.message_id, 2);
    EXPECT_EQ(assembler.crc_errors(), 1u);
}

TEST(FrameAssembler, RandomChunkingNeverLosesFrames) {
    // Many frames cut into random sized pieces, like a TCP stream would
    // deliver them. Every frame has to come out exactly once and in order.
    std::mt19937 rng(1234);
    Bytes stream;
    constexpr int kFrames = 200;
    for (int i = 0; i < kFrames; i++) {
        Bytes payload(static_cast<std::size_t>(rng() % 64), static_cast<std::uint8_t>(i));
        const Bytes frame = make_frame(static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(i), payload);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    FrameAssembler assembler;
    int next_expected = 0;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const std::size_t chunk = std::min<std::size_t>(1 + rng() % 40, stream.size() - offset);
        assembler.feed(ByteView(stream.data() + offset, chunk));
        offset += chunk;
        while (auto frame = assembler.next()) {
            EXPECT_EQ(frame->header.message_id, next_expected);
            next_expected++;
        }
    }
    EXPECT_EQ(next_expected, kFrames);
    EXPECT_EQ(assembler.dropped_bytes(), 0u);
}

}  // namespace
}  // namespace sigflow
