#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "sigflow/message.hpp"
#include "sigflow/result.hpp"
#include "sigflow/transport.hpp"

namespace sigflow {

struct EndpointStats {
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t decode_errors = 0;
    std::uint64_t unhandled = 0;
};

// Ties a transport to a set of typed message handlers.
//
//   Endpoint endpoint(transport);
//   endpoint.on<AttachRequest>([&](const AttachRequest& request) { ... });
//   while (running) endpoint.poll(100ms);
//
// The handler table is keyed by message id. Each entry is a small lambda
// that decodes the payload into the concrete type and only then calls the
// user handler, so handlers never see raw bytes.
class Endpoint {
public:
    using FrameCallback = std::function<void(const Frame&)>;
    using ErrorCallback = std::function<void(const Frame&, const Error&)>;

    explicit Endpoint(Transport& transport) noexcept : transport_(transport) {}

    template <Message T, typename Handler>
        requires std::is_invocable_v<Handler&, const T&>
    void on(Handler handler) {
        // Registering twice is a bug in the calling code, not a runtime
        // condition, so this throws instead of returning a Status.
        auto [it, inserted] =
            handlers_.try_emplace(T::kMessageId, [handler = std::move(handler)](const Frame& frame) mutable -> Status {
                auto message = decode_message<T>(frame);
                if (!message) return std::move(message).error().with_context(T::kName);
                handler(*message);
                return Status::success();
            });
        if (!inserted) {
            throw std::logic_error("handler for " + std::string(T::kName) + " registered twice");
        }
    }

    // Called for frames whose id has no handler.
    void on_unhandled(FrameCallback callback) { on_unhandled_ = std::move(callback); }

    // Called when a frame arrives but its payload cannot be decoded.
    void on_decode_error(ErrorCallback callback) { on_decode_error_ = std::move(callback); }

    template <Message T>
    Status send(const T& message) {
        auto frame = encode_message(message, next_sequence_);
        if (!frame) return std::move(frame).error();
        SIGFLOW_TRY(transport_.send(*frame));
        next_sequence_++;  // wraps around at 65535, which is intended
        stats_.frames_sent++;
        return Status::success();
    }

    // Waits for at most one frame and dispatches it. Returns true if a frame
    // was received (handled or not) and false on timeout. Only transport
    // failures are reported as errors, a peer sending garbage should not
    // take this side down.
    Result<bool> poll(std::chrono::milliseconds timeout);

    // Polls until a message of type T arrives or the timeout expires.
    // Frames of other types that show up in the meantime are dispatched to
    // their handlers as usual. Meant for simple request/response flows.
    template <Message T>
    Result<std::optional<T>> wait_for(std::chrono::milliseconds timeout);

    [[nodiscard]] const EndpointStats& stats() const noexcept { return stats_; }

private:
    void dispatch(const Frame& frame);

    Transport& transport_;
    std::unordered_map<std::uint16_t, std::function<Status(const Frame&)>> handlers_;
    FrameCallback on_unhandled_;
    ErrorCallback on_decode_error_;
    std::uint16_t next_sequence_ = 0;
    EndpointStats stats_;
};

template <Message T>
Result<std::optional<T>> Endpoint::wait_for(std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + timeout;

    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) return std::optional<T>{};

        auto received = transport_.receive(left);
        if (!received) return std::move(received).error();
        if (!received->has_value()) return std::optional<T>{};

        const Frame& frame = **received;
        stats_.frames_received++;
        if (frame.header.message_id != T::kMessageId) {
            dispatch(frame);
            continue;
        }

        auto message = decode_message<T>(frame);
        if (!message) {
            stats_.decode_errors++;
            if (on_decode_error_) on_decode_error_(frame, message.error());
            continue;
        }
        return std::optional<T>(std::move(*message));
    }
}

}  // namespace sigflow
