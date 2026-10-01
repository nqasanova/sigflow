#include "sigflow/transport.hpp"

namespace sigflow {

std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>> LoopbackTransport::make_pair() {
    auto a_to_b = std::make_shared<Queue>();
    auto b_to_a = std::make_shared<Queue>();
    // The constructor is private, so make_unique cannot be used here.
    return {std::unique_ptr<LoopbackTransport>(new LoopbackTransport(b_to_a, a_to_b)),
            std::unique_ptr<LoopbackTransport>(new LoopbackTransport(a_to_b, b_to_a))};
}

Status LoopbackTransport::send(ByteView frame) {
    {
        std::lock_guard lock(outbox_->mutex);
        outbox_->frames.emplace_back(frame.begin(), frame.end());
    }
    outbox_->ready.notify_one();
    return Status::success();
}

Result<std::optional<Frame>> LoopbackTransport::receive(std::chrono::milliseconds timeout) {
    Bytes raw;
    {
        std::unique_lock lock(inbox_->mutex);
        if (!inbox_->ready.wait_for(lock, timeout, [this] { return !inbox_->frames.empty(); })) {
            return std::optional<Frame>{};
        }
        raw = std::move(inbox_->frames.front());
        inbox_->frames.pop_front();
    }

    // Frames still go through the real decoder, so tests that use this
    // transport exercise the same code path as the network ones.
    auto frame = decode_frame(raw);
    if (!frame) return std::move(frame).error();
    return std::optional<Frame>(std::move(*frame));
}

}  // namespace sigflow
