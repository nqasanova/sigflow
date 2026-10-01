#include "sigflow/tlv.hpp"

#include <gtest/gtest.h>

#include <limits>

#include "session_messages.hpp"

namespace sigflow {
namespace {

// Encodes a single field and parses it back, the common setup for most
// tests in this file.
template <typename T>
Bytes encode_one(std::uint8_t tag, const T& value) {
    ByteWriter out;
    TlvWriter writer(out);
    EXPECT_TRUE(writer.put(tag, value).ok());
    return std::move(out).take();
}

TlvField only_field(const Bytes& encoded) {
    auto fields = parse_tlv(encoded);
    EXPECT_TRUE(fields.ok());
    EXPECT_EQ(fields->size(), 1u);
    return fields->front();
}

TEST(Tlv, IntegerLayout) {
    const Bytes encoded = encode_one<std::uint16_t>(7, 0x0102);
    const Bytes expected = {0x07, 0x00, 0x02, 0x01, 0x02};
    EXPECT_EQ(encoded, expected);
}

TEST(Tlv, SignedValuesUseTwosComplement) {
    const Bytes encoded = encode_one<std::int16_t>(1, -2);
    const Bytes expected = {0x01, 0x00, 0x02, 0xFF, 0xFE};
    EXPECT_EQ(encoded, expected);

    EXPECT_EQ(read_value<std::int16_t>(only_field(encoded)).value(), -2);
}

TEST(Tlv, IntegerExtremesRoundTrip) {
    for (auto value : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, std::int64_t{0},
                       std::numeric_limits<std::int64_t>::max()}) {
        const Bytes encoded = encode_one(3, value);
        EXPECT_EQ(read_value<std::int64_t>(only_field(encoded)).value(), value);
    }
}

TEST(Tlv, StringAndBytesRoundTrip) {
    const std::string text("hello\0world", 11);  // embedded zero is fine
    EXPECT_EQ(read_value<std::string>(only_field(encode_one(1, text))).value(), text);

    const Bytes blob = {0x00, 0xFF, 0x10};
    EXPECT_EQ(read_value<Bytes>(only_field(encode_one(2, blob))).value(), blob);
}

TEST(Tlv, RejectsWrongLength) {
    // A u16 field read as u32 means the peers disagree on the schema.
    auto value = read_value<std::uint32_t>(only_field(encode_one<std::uint16_t>(1, 5)));
    ASSERT_FALSE(value.ok());
    EXPECT_EQ(value.error().code, Errc::bad_field_length);
}

TEST(Tlv, RejectsBoolOtherThanZeroOrOne) {
    auto value = read_value<bool>(only_field(encode_one<std::uint8_t>(1, 2)));
    ASSERT_FALSE(value.ok());
    EXPECT_EQ(value.error().code, Errc::invalid_value);
}

TEST(Tlv, RejectsUnknownEnumValue) {
    // 3 is not a declared AttachResult.
    auto value = read_value<ran::session::AttachResult>(only_field(encode_one<std::uint8_t>(1, 3)));
    ASSERT_FALSE(value.ok());
    EXPECT_EQ(value.error().code, Errc::invalid_value);
}

TEST(Tlv, RejectsOversizedField) {
    ByteWriter out;
    TlvWriter writer(out);
    const Bytes huge(kMaxFieldSize + 1, 0xAB);
    auto status = writer.put(1, huge);
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code, Errc::field_too_large);
    EXPECT_EQ(out.size(), 0u) << "nothing should be written on failure";
}

TEST(Tlv, ParseDetectsTruncatedHeaderAndValue) {
    const Bytes half_header = {0x01, 0x00};
    EXPECT_EQ(parse_tlv(half_header).error().code, Errc::truncated);

    const Bytes short_value = {0x01, 0x00, 0x04, 0xAA, 0xBB};
    EXPECT_EQ(parse_tlv(short_value).error().code, Errc::truncated);
}

TEST(Tlv, EmptyPayloadHasNoFields) {
    auto fields = parse_tlv({});
    ASSERT_TRUE(fields.ok());
    EXPECT_TRUE(fields->empty());
}

}  // namespace
}  // namespace sigflow
