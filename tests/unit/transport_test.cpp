#include "sigflow/transport.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace sigflow {
namespace {

using namespace std::chrono_literals;

Bytes frame_with(std::uint16_t id, const Bytes& payload) {
    auto frame = encode_frame(id, 0, payload);
    EXPECT_TRUE(frame.ok());
    return std::move(frame).value();
}

TEST(StreamTransport, DeliversFramesInOrder) {
    auto pair = StreamTransport::make_pair();
    ASSERT_TRUE(pair.ok());
    auto& [a, b] = *pair;

    for (std::uint16_t id = 1; id <= 5; id++) ASSERT_TRUE(a->send(frame_with(id, {0x01, 0x02})).ok());

    for (std::uint16_t id = 1; id <= 5; id++) {
        auto frame = b->receive(500ms);
        ASSERT_TRUE(frame.ok()) << frame.error().message();
        ASSERT_TRUE(frame->has_value());
        EXPECT_EQ((*frame)->header.message_id, id);
    }
}

TEST(StreamTransport, LargeFrameSurvivesPartialReads) {
    // Bigger than the 4 KiB read chunk, so it arrives in several pieces.
    auto pair = StreamTransport::make_pair();
    ASSERT_TRUE(pair.ok());
    auto& [a, b] = *pair;

    const Bytes payload(20000, 0x5A);
    ASSERT_TRUE(a->send(frame_with(9, payload)).ok());

    auto frame = b->receive(500ms);
    ASSERT_TRUE(frame.ok());
    ASSERT_TRUE(frame->has_value());
    EXPECT_EQ((*frame)->payload, payload);
}

TEST(StreamTransport, ReportsClosedPeer) {
    auto pair = StreamTransport::make_pair();
    ASSERT_TRUE(pair.ok());
    pair->first.reset();

    auto frame = pair->second->receive(500ms);
    ASSERT_FALSE(frame.ok());
    EXPECT_EQ(frame.error().code, Errc::io_error);
}

TEST(UdpTransport, ClientAndServerExchangeFrames) {
    auto server = UdpTransport::bind(0);
    ASSERT_TRUE(server.ok()) << server.error().message();
    const auto port = (*server)->local_port();
    ASSERT_NE(port, 0);

    auto client = UdpTransport::connect("127.0.0.1", port);
    ASSERT_TRUE(client.ok()) << client.error().message();

    // The server cannot send before it heard from someone.
    EXPECT_FALSE((*server)->send(frame_with(1, {})).ok());

    ASSERT_TRUE((*client)->send(frame_with(0x0101, {0xAA})).ok());
    auto request = (*server)->receive(1s);
    ASSERT_TRUE(request.ok());
    ASSERT_TRUE(request->has_value());
    EXPECT_EQ((*request)->header.message_id, 0x0101);

    // Now the server knows the client's address and can answer.
    ASSERT_TRUE((*server)->send(frame_with(0x0102, {0xBB})).ok());
    auto response = (*client)->receive(1s);
    ASSERT_TRUE(response.ok());
    ASSERT_TRUE(response->has_value());
    EXPECT_EQ((*response)->header.message_id, 0x0102);
}

TEST(UdpTransport, DropsInvalidDatagrams) {
    auto server = UdpTransport::bind(0);
    ASSERT_TRUE(server.ok());

    // Send junk with a raw socket, then a valid frame from a real client.
    FileDescriptor raw(::socket(AF_INET, SOCK_DGRAM, 0));
    ASSERT_TRUE(raw.valid());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((*server)->local_port());
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const std::uint8_t junk[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_EQ(::sendto(raw.get(), junk, sizeof(junk), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)),
              static_cast<ssize_t>(sizeof(junk)));

    auto client = UdpTransport::connect("127.0.0.1", (*server)->local_port());
    ASSERT_TRUE(client.ok());
    ASSERT_TRUE((*client)->send(frame_with(5, {})).ok());

    auto frame = (*server)->receive(1s);
    ASSERT_TRUE(frame.ok());
    ASSERT_TRUE(frame->has_value());
    EXPECT_EQ((*frame)->header.message_id, 5);
    EXPECT_EQ((*server)->rejected_datagrams(), 1u);
}

TEST(UdpTransport, ConnectFailsForUnknownHost) {
    auto client = UdpTransport::connect("does-not-exist.invalid", 1234);
    ASSERT_FALSE(client.ok());
    EXPECT_EQ(client.error().code, Errc::io_error);
}

}  // namespace
}  // namespace sigflow
