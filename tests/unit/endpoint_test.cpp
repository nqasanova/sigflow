#include "sigflow/endpoint.hpp"

#include <gtest/gtest.h>

#include <thread>

#include "mobility_messages.hpp"
#include "session_messages.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ran;

class EndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto [a, b] = sigflow::LoopbackTransport::make_pair();
        left_transport = std::move(a);
        right_transport = std::move(b);
        left = std::make_unique<sigflow::Endpoint>(*left_transport);
        right = std::make_unique<sigflow::Endpoint>(*right_transport);
    }

    std::unique_ptr<sigflow::LoopbackTransport> left_transport;
    std::unique_ptr<sigflow::LoopbackTransport> right_transport;
    std::unique_ptr<sigflow::Endpoint> left;
    std::unique_ptr<sigflow::Endpoint> right;
};

TEST_F(EndpointTest, DispatchesToTypedHandler) {
    std::optional<session::DetachRequest> received;
    right->on<session::DetachRequest>([&](const session::DetachRequest& m) { received = m; });

    ASSERT_TRUE(left->send(session::DetachRequest{7, session::DetachCause::kPowerOff}).ok());
    auto polled = right->poll(100ms);
    ASSERT_TRUE(polled.ok());
    EXPECT_TRUE(*polled);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->ue_id, 7u);
    EXPECT_EQ(received->cause, session::DetachCause::kPowerOff);
}

TEST_F(EndpointTest, PollTimesOutQuietly) {
    auto polled = right->poll(10ms);
    ASSERT_TRUE(polled.ok());
    EXPECT_FALSE(*polled);
}

TEST_F(EndpointTest, UnhandledFramesGoToFallback) {
    std::uint16_t unhandled_id = 0;
    right->on_unhandled([&](const sigflow::Frame& frame) { unhandled_id = frame.header.message_id; });

    ASSERT_TRUE(left->send(session::DetachAccept{1}).ok());
    ASSERT_TRUE(right->poll(100ms).ok());

    EXPECT_EQ(unhandled_id, session::DetachAccept::kMessageId);
    EXPECT_EQ(right->stats().unhandled, 1u);
}

TEST_F(EndpointTest, BadPayloadTriggersErrorCallback) {
    // A frame with the DetachAccept id but an empty body: the required
    // ue_id is missing, so decoding has to fail.
    bool handler_called = false;
    std::optional<sigflow::Error> error;
    right->on<session::DetachAccept>([&](const auto&) { handler_called = true; });
    right->on_decode_error([&](const sigflow::Frame&, const sigflow::Error& e) { error = e; });

    auto raw = sigflow::encode_frame(session::DetachAccept::kMessageId, 0, {});
    ASSERT_TRUE(raw.ok());
    ASSERT_TRUE(left_transport->send(*raw).ok());
    ASSERT_TRUE(right->poll(100ms).ok());

    EXPECT_FALSE(handler_called);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, sigflow::Errc::missing_field);
    EXPECT_EQ(right->stats().decode_errors, 1u);
}

TEST_F(EndpointTest, RegisteringTwiceThrows) {
    right->on<session::DetachAccept>([](const auto&) {});
    EXPECT_THROW(right->on<session::DetachAccept>([](const auto&) {}), std::logic_error);
}

TEST_F(EndpointTest, SequenceNumbersIncrease) {
    for (int i = 0; i < 3; i++) ASSERT_TRUE(left->send(session::DetachAccept{1}).ok());

    for (std::uint16_t expected = 0; expected < 3; expected++) {
        auto frame = right_transport->receive(100ms);
        ASSERT_TRUE(frame.ok());
        ASSERT_TRUE(frame->has_value());
        EXPECT_EQ((*frame)->header.sequence, expected);
    }
    EXPECT_EQ(left->stats().frames_sent, 3u);
}

TEST_F(EndpointTest, WaitForDispatchesOtherMessagesOnTheWay) {
    // A handover command arrives before the response that is waited for.
    // It must still reach its handler and not be thrown away.
    int handovers = 0;
    right->on<mobility::HandoverCommand>([&](const auto&) { handovers++; });

    ASSERT_TRUE(left->send(mobility::HandoverCommand{1, 102, 35}).ok());
    session::AttachResponse response;
    response.result = session::AttachResult::kAccepted;
    response.ue_id = 1;
    ASSERT_TRUE(left->send(response).ok());

    auto got = right->wait_for<session::AttachResponse>(200ms);
    ASSERT_TRUE(got.ok());
    ASSERT_TRUE(got->has_value());
    EXPECT_EQ((*got)->ue_id, 1u);
    EXPECT_EQ(handovers, 1);
}

TEST_F(EndpointTest, WaitForTimesOut) {
    auto got = right->wait_for<session::AttachResponse>(20ms);
    ASSERT_TRUE(got.ok());
    EXPECT_FALSE(got->has_value());
}

TEST_F(EndpointTest, WorksAcrossThreads) {
    // Request/response between two threads, like a real client and server.
    constexpr std::uint32_t kRequests = 50;
    std::thread server([&] {
        right->on<session::DetachRequest>([&](const session::DetachRequest& request) {
            ASSERT_TRUE(right->send(session::DetachAccept{request.ue_id}).ok());
        });
        for (std::uint32_t i = 0; i < kRequests; i++) ASSERT_TRUE(right->poll(1s).ok());
    });

    for (std::uint32_t i = 0; i < kRequests; i++) {
        ASSERT_TRUE(left->send(session::DetachRequest{i, session::DetachCause::kUserRequest}).ok());
        auto accept = left->wait_for<session::DetachAccept>(1s);
        ASSERT_TRUE(accept.ok());
        ASSERT_TRUE(accept->has_value());
        EXPECT_EQ((*accept)->ue_id, i);
    }
    server.join();
}

}  // namespace
