// POSIX implementations of the socket based transports. Linux and macOS
// both work, Windows would need its own file because of Winsock.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

#include "sigflow/transport.hpp"

#ifndef MSG_NOSIGNAL
// macOS has no MSG_NOSIGNAL. SIGPIPE has to be ignored by the application
// there instead (the simulators do that in main).
#define MSG_NOSIGNAL 0
#endif

namespace sigflow {
namespace {

using Clock = std::chrono::steady_clock;

Error io_error(const std::string& what) {
    return make_error(Errc::io_error, what + ": " + std::strerror(errno));
}

// Waits until fd is readable. Returns false on timeout. An interrupted
// wait (EINTR) also counts as a timeout, so a signal handler that sets a
// stop flag gets noticed by the caller's loop right away.
Result<bool> wait_readable(int fd, std::chrono::milliseconds timeout) {
    pollfd entry{fd, POLLIN, 0};
    const int ready = ::poll(&entry, 1, static_cast<int>(timeout.count()));
    if (ready < 0) {
        if (errno == EINTR) return false;
        return io_error("poll");
    }
    return ready > 0;
}

std::chrono::milliseconds time_left(Clock::time_point deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return left.count() > 0 ? left : std::chrono::milliseconds(0);
}

}  // namespace

// FileDescriptor --------------------------------------------------------

FileDescriptor::~FileDescriptor() {
    if (fd_ >= 0) ::close(fd_);
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

// UdpTransport ----------------------------------------------------------

struct UdpTransport::Peer {
    sockaddr_in address{};
    bool known = false;
    bool learn_from_traffic = false;  // server mode, see header
};

UdpTransport::UdpTransport(FileDescriptor socket, std::unique_ptr<Peer> peer)
    : socket_(std::move(socket)), peer_(std::move(peer)) {}

UdpTransport::~UdpTransport() = default;

Result<std::unique_ptr<UdpTransport>> UdpTransport::bind(std::uint16_t port) {
    FileDescriptor socket(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!socket.valid()) return io_error("socket");

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        return io_error("bind to port " + std::to_string(port));
    }

    auto peer = std::make_unique<Peer>();
    peer->learn_from_traffic = true;
    return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(socket), std::move(peer)));
}

Result<std::unique_ptr<UdpTransport>> UdpTransport::connect(const std::string& host, std::uint16_t port) {
    // getaddrinfo instead of inet_pton so host names work too, which is
    // what makes the docker compose setup (host "enb") possible.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &found); rc != 0) {
        return make_error(Errc::io_error, "cannot resolve '" + host + "': " + ::gai_strerror(rc));
    }
    auto peer = std::make_unique<Peer>();
    std::memcpy(&peer->address, found->ai_addr, sizeof(sockaddr_in));
    ::freeaddrinfo(found);
    peer->address.sin_port = htons(port);
    peer->known = true;

    FileDescriptor socket(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!socket.valid()) return io_error("socket");
    return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(socket), std::move(peer)));
}

Status UdpTransport::send(ByteView frame) {
    if (!peer_->known) return make_error(Errc::io_error, "no peer to send to yet");

    const auto sent = ::sendto(socket_.get(), frame.data(), frame.size(), 0,
                               reinterpret_cast<const sockaddr*>(&peer_->address), sizeof(peer_->address));
    if (sent < 0) return io_error("sendto");
    if (static_cast<std::size_t>(sent) != frame.size()) {
        return make_error(Errc::io_error, "datagram was cut short");
    }
    return Status::success();
}

Result<std::optional<Frame>> UdpTransport::receive(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    // One byte bigger than the largest valid frame, so an oversized
    // datagram shows up as too long instead of being silently truncated.
    std::array<std::uint8_t, kFrameOverhead + kMaxPayloadSize + 1> buffer{};

    while (true) {
        auto readable = wait_readable(socket_.get(), time_left(deadline));
        if (!readable) return std::move(readable).error();
        if (!*readable) return std::optional<Frame>{};

        sockaddr_in sender{};
        socklen_t sender_len = sizeof(sender);
        const auto size = ::recvfrom(socket_.get(), buffer.data(), buffer.size(), 0,
                                     reinterpret_cast<sockaddr*>(&sender), &sender_len);
        if (size < 0) {
            if (errno == EINTR) return std::optional<Frame>{};
            return io_error("recvfrom");
        }

        auto frame = decode_frame(ByteView(buffer.data(), static_cast<std::size_t>(size)));
        if (!frame) {
            // A broken datagram is dropped and the wait goes on. Only valid
            // frames may change who the server talks to.
            rejected_++;
            continue;
        }
        if (peer_->learn_from_traffic) {
            peer_->address = sender;
            peer_->known = true;
        }
        return std::optional<Frame>(std::move(*frame));
    }
}

std::uint16_t UdpTransport::local_port() const {
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (::getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&local), &length) != 0) return 0;
    return ntohs(local.sin_port);
}

// StreamTransport -------------------------------------------------------

Result<std::pair<std::unique_ptr<StreamTransport>, std::unique_ptr<StreamTransport>>> StreamTransport::make_pair() {
    std::array<int, 2> fds{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) return io_error("socketpair");
    return std::make_pair(std::make_unique<StreamTransport>(FileDescriptor(fds[0])),
                          std::make_unique<StreamTransport>(FileDescriptor(fds[1])));
}

Status StreamTransport::send(ByteView frame) {
    // A stream write may accept only part of the data, so keep going until
    // everything is out.
    std::size_t offset = 0;
    while (offset < frame.size()) {
        auto written = ::send(fd_.get(), frame.data() + offset, frame.size() - offset, MSG_NOSIGNAL);
        if (written < 0 && errno == ENOTSOCK) {
            written = ::write(fd_.get(), frame.data() + offset, frame.size() - offset);
        }
        if (written < 0) {
            if (errno == EINTR) continue;
            return io_error("write");
        }
        offset += static_cast<std::size_t>(written);
    }
    return Status::success();
}

Result<std::optional<Frame>> StreamTransport::receive(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::array<std::uint8_t, 4096> chunk{};

    while (true) {
        // A previous read may already hold more than one frame.
        if (auto frame = assembler_.next()) return frame;

        auto readable = wait_readable(fd_.get(), time_left(deadline));
        if (!readable) return std::move(readable).error();
        if (!*readable) return std::optional<Frame>{};

        const auto size = ::read(fd_.get(), chunk.data(), chunk.size());
        if (size < 0) {
            if (errno == EINTR) return std::optional<Frame>{};
            return io_error("read");
        }
        if (size == 0) return make_error(Errc::io_error, "peer closed the connection");
        assembler_.feed(ByteView(chunk.data(), static_cast<std::size_t>(size)));
    }
}

}  // namespace sigflow
