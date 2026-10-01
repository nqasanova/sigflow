#include "sigflow/byte_io.hpp"

#include <gtest/gtest.h>

namespace sigflow {
namespace {

TEST(ByteWriter, WritesBigEndian) {
    ByteWriter out;
    out.put_be<std::uint16_t>(0x1234);
    out.put_be<std::uint32_t>(0xA1B2C3D4);
    out.put_u8(0xFF);

    const Bytes expected = {0x12, 0x34, 0xA1, 0xB2, 0xC3, 0xD4, 0xFF};
    EXPECT_EQ(Bytes(out.view().begin(), out.view().end()), expected);
}

TEST(ByteWriter, PatchOverwritesEarlierValue) {
    ByteWriter out;
    out.put_be<std::uint16_t>(0);  // placeholder
    out.put_u8(0xAA);
    out.patch_be<std::uint16_t>(0, 0xBEEF);

    const Bytes expected = {0xBE, 0xEF, 0xAA};
    EXPECT_EQ(std::move(out).take(), expected);
}

TEST(ByteReader, RoundTripsEveryWidth) {
    ByteWriter out;
    out.put_be<std::uint8_t>(0x01);
    out.put_be<std::uint16_t>(0x0203);
    out.put_be<std::uint32_t>(0x04050607);
    out.put_be<std::uint64_t>(0x08090A0B0C0D0E0FULL);

    ByteReader in(out.view());
    EXPECT_EQ(in.read_be<std::uint8_t>().value(), 0x01);
    EXPECT_EQ(in.read_be<std::uint16_t>().value(), 0x0203);
    EXPECT_EQ(in.read_be<std::uint32_t>().value(), 0x04050607u);
    EXPECT_EQ(in.read_be<std::uint64_t>().value(), 0x08090A0B0C0D0E0FULL);
    EXPECT_TRUE(in.empty());
}

TEST(ByteReader, ReportsTruncationWithoutConsuming) {
    const Bytes data = {0x01, 0x02, 0x03};
    ByteReader in(data);

    auto value = in.read_be<std::uint32_t>();
    ASSERT_FALSE(value.ok());
    EXPECT_EQ(value.error().code, Errc::truncated);
    // A failed read must leave the position alone, so the caller can still
    // read what is actually there.
    EXPECT_EQ(in.position(), 0u);
    EXPECT_EQ(in.read_be<std::uint16_t>().value(), 0x0102);
}

TEST(ByteReader, ReadBytesReturnsViewIntoInput) {
    const Bytes data = {0xDE, 0xAD, 0xBE, 0xEF};
    ByteReader in(data);

    auto view = in.read_bytes(2);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view->data(), data.data());
    EXPECT_EQ(view->size(), 2u);
    EXPECT_FALSE(in.read_bytes(3).ok());
}

}  // namespace
}  // namespace sigflow
