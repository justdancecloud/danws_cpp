#pragma once

#include "../protocol/types.h"
#include "../protocol/codec.h"
#include <vector>
#include <functional>
#include <mutex>

namespace danws {

/// Simple frame queue that batches frames for sending.
/// Unlike the server-side BulkQueue, this client-side version
/// flushes immediately (no timer) since clients typically send
/// small bursts during handshake/topic sync.
class BulkQueue {
public:
    BulkQueue() = default;

    /// Set callback for flushing encoded data.
    void onFlush(std::function<void(const std::vector<uint8_t>&)> callback) {
        onFlush_ = std::move(callback);
    }

    /// Enqueue a frame. Immediately encodes and flushes.
    void enqueue(const Frame& frame) {
        if (onFlush_) {
            auto data = encode(frame);
            onFlush_(data);
        }
    }

    /// Enqueue multiple frames as a single batch.
    void enqueueBatch(const std::vector<Frame>& frames) {
        if (onFlush_ && !frames.empty()) {
            auto data = encodeBatch(frames);
            onFlush_(data);
        }
    }

    /// Clear pending state (no-op for immediate flush mode).
    void clear() {}

private:
    std::function<void(const std::vector<uint8_t>&)> onFlush_;
};

} // namespace danws
