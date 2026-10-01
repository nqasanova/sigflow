#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "sigflow/byte_io.hpp"
#include "sigflow/frame.hpp"
#include "sigflow/result.hpp"

namespace sigflow {

// Moves complete frames between two peers. Implementations decide how the
// bytes travel, everything above this interface only sees frames.
class Transport {
public:
    virtual ~Transport() = default;

    virtual Status send(ByteView frame) = 0;

    // Waits up to `timeout` for one frame. An empty optional means the
    // timeout expired, which is a normal outcome and not an error.
    virtual Result<std::optional<Frame>> receive(std::chrono::milliseconds timeout) = 0;
};

// Owns a file descriptor and closes it exactly once. Small, but it makes
// every early return in the socket code safe.
class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// UDP transport. One datagram carries exactly one frame.
//
// A transport created with bind() acts as a server: it does not know its
// peer up front and replies to whoever sent the most recent valid frame.
// That is enough for the simulators in this repo. A real base station
// would keep one context per UE, which belongs in a layer above this one.
class UdpTransport final : public Transport {
public:
    static Result<std::unique_ptr<UdpTransport>> bind(std::uint16_t port);
    static Result<std::unique_ptr<UdpTransport>> connect(const std::string& host, std::uint16_t port);

    ~UdpTransport() override;

    Status send(ByteView frame) override;
    Result<std::optional<Frame>> receive(std::chrono::milliseconds timeout) override;

    // Port the socket ended up on, useful after binding to port 0.
    [[nodiscard]] std::uint16_t local_port() const;

    [[nodiscard]] std::size_t rejected_datagrams() const noexcept { return rejected_; }

private:
    struct Peer;
    UdpTransport(FileDescriptor socket, std::unique_ptr<Peer> peer);

    FileDescriptor socket_;
    std::unique_ptr<Peer> peer_;  // pimpl, keeps sockaddr out of this header
    std::size_t rejected_ = 0;
};

// Transport over any connected stream file descriptor: a TCP socket, a
// pipe, a serial port or a socketpair. Frames are recovered from the byte
// stream with a FrameAssembler.
class StreamTransport final : public Transport {
public:
    explicit StreamTransport(FileDescriptor fd) noexcept : fd_(std::move(fd)) {}

    // Creates two connected transports, handy for tests and for wiring two
    // components together inside one process.
    static Result<std::pair<std::unique_ptr<StreamTransport>, std::unique_ptr<StreamTransport>>> make_pair();

    Status send(ByteView frame) override;
    Result<std::optional<Frame>> receive(std::chrono::milliseconds timeout) override;

    [[nodiscard]] const FrameAssembler& assembler() const noexcept { return assembler_; }

private:
    FileDescriptor fd_;
    FrameAssembler assembler_;
};

// In memory transport for unit tests. Two linked instances hand frames to
// each other through a shared queue, no sockets involved. Thread safe, so
// both ends can live on different threads.
class LoopbackTransport final : public Transport {
public:
    static std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>> make_pair();

    Status send(ByteView frame) override;
    Result<std::optional<Frame>> receive(std::chrono::milliseconds timeout) override;

private:
    struct Queue {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Bytes> frames;
    };

    LoopbackTransport(std::shared_ptr<Queue> inbox, std::shared_ptr<Queue> outbox)
        : inbox_(std::move(inbox)), outbox_(std::move(outbox)) {}

    std::shared_ptr<Queue> inbox_;
    std::shared_ptr<Queue> outbox_;
};

}  // namespace sigflow
