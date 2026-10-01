#include "sigflow/endpoint.hpp"

namespace sigflow {

Result<bool> Endpoint::poll(std::chrono::milliseconds timeout) {
    auto received = transport_.receive(timeout);
    if (!received) return std::move(received).error();
    if (!received->has_value()) return false;

    stats_.frames_received++;
    dispatch(**received);
    return true;
}

void Endpoint::dispatch(const Frame& frame) {
    auto handler = handlers_.find(frame.header.message_id);
    if (handler == handlers_.end()) {
        stats_.unhandled++;
        if (on_unhandled_) on_unhandled_(frame);
        return;
    }

    if (auto status = handler->second(frame); !status) {
        stats_.decode_errors++;
        if (on_decode_error_) on_decode_error_(frame, status.error());
    }
}

}  // namespace sigflow
