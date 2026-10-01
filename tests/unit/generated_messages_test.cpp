// Tests for the code that msgc generates from schemas/*.msg. These go
// through the public API only, the same way an application would.

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "mobility_messages.hpp"
#include "session_messages.hpp"
#include "sigflow/message.hpp"
#include "sigflow/tlv.hpp"

namespace {

using namespace ran;
using sigflow::Bytes;
using sigflow::ByteWriter;
using sigflow::Errc;

Bytes from_hex(std::string_view hex) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

session::AttachRequest sample_request() {
    session::AttachRequest request;
    request.imsi = 262011234567890ULL;
    request.category = session::UeCategory::kCat4;
    request.device_name = "pixel";
    request.supported_bands = {1, 3, 20};
    request.requested_bandwidth_mhz = 20;
    return request;
}

template <typename T>
Bytes encode_body(const T& message) {
    ByteWriter out;
    EXPECT_TRUE(message.encode(out).ok());
    return std::move(out).take();
}

TEST(GeneratedMessages, AttachRequestRoundTrip) {
    const auto original = sample_request();
    auto decoded = session::AttachRequest::decode(encode_body(original));
    ASSERT_TRUE(decoded.ok()) << decoded.error().message();
    EXPECT_EQ(*decoded, original);
}

TEST(GeneratedMessages, MatchesBytesProducedByPython) {
    // Produced by the generated Python module for the same message:
    //   wire.encode_message(AttachRequest(...), 7).hex()
    // If this test breaks, the two code generators no longer agree on the
    // wire format.
    const Bytes python_frame = from_hex(
        "53460100010100070000002b0100080000ee4c3e9046d202000104030005706978656c"
        "040002000104000200030400020014050002001457ea");

    auto frame = sigflow::encode_message(sample_request(), 7);
    ASSERT_TRUE(frame.ok());
    EXPECT_EQ(*frame, python_frame);

    auto decoded_frame = sigflow::decode_frame(python_frame);
    ASSERT_TRUE(decoded_frame.ok());
    auto message = sigflow::decode_message<session::AttachRequest>(*decoded_frame);
    ASSERT_TRUE(message.ok());
    EXPECT_EQ(*message, sample_request());
}

TEST(GeneratedMessages, OptionalFieldsCanBeAbsent) {
    session::AttachResponse response;
    response.result = session::AttachResult::kRejectedUnknownNetwork;

    auto decoded = session::AttachResponse::decode(encode_body(response));
    ASSERT_TRUE(decoded.ok());
    EXPECT_FALSE(decoded->ue_id.has_value());
    EXPECT_FALSE(decoded->retry_after_ms.has_value());
    EXPECT_EQ(decoded->result, session::AttachResult::kRejectedUnknownNetwork);
}

TEST(GeneratedMessages, NestedStructsRoundTrip) {
    mobility::MeasurementReport report;
    report.ue_id = 17;
    report.serving = {101, -955, -105};
    report.neighbours = {{102, -900, -80}, {103, -1200, -150}};

    auto decoded = mobility::MeasurementReport::decode(encode_body(report));
    ASSERT_TRUE(decoded.ok()) << decoded.error().message();
    EXPECT_EQ(*decoded, report);
}

TEST(GeneratedMessages, MissingRequiredFieldIsReported) {
    ByteWriter out;
    sigflow::TlvWriter writer(out);
    ASSERT_TRUE(writer.put(1, std::uint32_t{5}).ok());  // ue_id only, cause missing

    auto decoded = session::DetachRequest::decode(out.view());
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code, Errc::missing_field);
    EXPECT_EQ(decoded.error().detail, "DetachRequest.cause");
}

TEST(GeneratedMessages, DuplicateFieldIsRejected) {
    ByteWriter out;
    sigflow::TlvWriter writer(out);
    ASSERT_TRUE(writer.put(1, std::uint32_t{5}).ok());
    ASSERT_TRUE(writer.put(1, std::uint32_t{6}).ok());

    auto decoded = session::DetachAccept::decode(out.view());
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code, Errc::duplicate_field);
}

TEST(GeneratedMessages, UnknownFieldsAreSkipped) {
    // Simulates a newer peer that added tag 99 to DetachAccept.
    session::DetachAccept accept{42};
    ByteWriter out;
    ASSERT_TRUE(accept.encode(out).ok());
    sigflow::TlvWriter writer(out);
    ASSERT_TRUE(writer.put(99, std::string("from the future")).ok());

    auto decoded = session::DetachAccept::decode(out.view());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded->ue_id, 42u);
}

TEST(GeneratedMessages, ErrorsInNestedStructsCarryThePath) {
    ByteWriter nested;
    sigflow::TlvWriter nested_writer(nested);
    ASSERT_TRUE(nested_writer.put(1, std::uint16_t{101}).ok());  // cell_id should be u32

    ByteWriter out;
    sigflow::TlvWriter writer(out);
    ASSERT_TRUE(writer.put(1, std::uint32_t{1}).ok());
    ASSERT_TRUE(writer.put(2, std::move(nested).take()).ok());

    auto decoded = mobility::MeasurementReport::decode(out.view());
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code, Errc::bad_field_length);
    EXPECT_NE(decoded.error().detail.find("MeasurementReport.serving: CellMeasurement.cell_id"), std::string::npos)
        << decoded.error().detail;
}

TEST(GeneratedMessages, DecodeMessageChecksTheId) {
    auto frame = sigflow::encode_message(session::DetachAccept{1}, 0);
    ASSERT_TRUE(frame.ok());
    auto decoded_frame = sigflow::decode_frame(*frame);
    ASSERT_TRUE(decoded_frame.ok());

    auto wrong = sigflow::decode_message<session::AttachRequest>(*decoded_frame);
    ASSERT_FALSE(wrong.ok());
    EXPECT_EQ(wrong.error().code, Errc::unexpected_message);
}

TEST(GeneratedMessages, EnumHelpers) {
    EXPECT_EQ(session::to_string(session::AttachResult::kRejectedCongestion), "REJECTED_CONGESTION");
    EXPECT_FALSE(session::is_valid(static_cast<session::UeCategory>(2)));
    EXPECT_EQ(session::message_name(session::AttachRequest::kMessageId), "AttachRequest");
    EXPECT_TRUE(session::message_name(0xFFFF).empty());
}

TEST(GeneratedMessages, RandomInputNeverCrashes) {
    // Cheap fuzzing: throw random bytes and mutated valid messages at the
    // decoders. The only requirement is that they return an error or a
    // value without crashing. Under ASan/UBSan in CI this also catches
    // out of bounds reads and overflow.
    std::mt19937 rng(42);
    const Bytes valid = encode_body(sample_request());

    for (int i = 0; i < 5000; i++) {
        Bytes input;
        if (i % 2 == 0) {
            input.resize(rng() % 64);
            for (auto& byte : input) byte = static_cast<std::uint8_t>(rng());
        } else {
            input = valid;
            input[rng() % input.size()] = static_cast<std::uint8_t>(rng());
        }
        (void)session::AttachRequest::decode(input);
        (void)mobility::MeasurementReport::decode(input);
        (void)sigflow::decode_frame(input);
    }
    SUCCEED();
}

}  // namespace
