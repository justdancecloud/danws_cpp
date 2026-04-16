#pragma once

#include "types.h"
#include <vector>
#include <functional>
#include <cstdint>

namespace danws {

/// Streaming protocol parser. Handles partial data, DLE escaping,
/// heartbeat detection, and frame assembly across multiple feed() calls.
class StreamParser {
public:
    explicit StreamParser(size_t maxBufferSize = 1048576);

    /// Feed raw bytes into the parser. Callbacks fire synchronously during feed().
    void feed(const uint8_t* data, size_t length);

    /// Convenience overload.
    void feed(const std::vector<uint8_t>& data) {
        feed(data.data(), data.size());
    }

    /// Register a callback for complete frames.
    void onFrame(std::function<void(const Frame&)> callback);

    /// Register a callback for heartbeats.
    void onHeartbeat(std::function<void()> callback);

    /// Register a callback for parse errors (non-fatal, parser resyncs).
    void onError(std::function<void(const std::string&)> callback);

    /// Reset parser state (e.g., after reconnect).
    void reset();

private:
    enum class State {
        Idle,
        AfterDLE,
        InFrame,
        InFrameAfterDLE,
    };

    State state_ = State::Idle;
    std::vector<uint8_t> buffer_;
    size_t maxBufferSize_;

    std::vector<std::function<void(const Frame&)>> frameCallbacks_;
    std::vector<std::function<void()>> heartbeatCallbacks_;
    std::vector<std::function<void(const std::string&)>> errorCallbacks_;

    void emitFrame(const Frame& frame);
    void emitHeartbeat();
    void emitError(const std::string& msg);
    Frame parseFrame(const std::vector<uint8_t>& body);
};

} // namespace danws
