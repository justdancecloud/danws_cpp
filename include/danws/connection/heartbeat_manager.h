#pragma once

#include <functional>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>

namespace danws {

/// Manages heartbeat send/receive timing.
/// Uses a background thread for timers (no dependency on Boost/Asio).
class HeartbeatManager {
public:
    HeartbeatManager();
    ~HeartbeatManager();

    /// Set callback to send heartbeat bytes.
    void onSend(std::function<void(const std::vector<uint8_t>&)> callback);

    /// Set callback when heartbeat timeout is detected.
    void onTimeout(std::function<void()> callback);

    /// Start heartbeat timers.
    void start();

    /// Mark that a message was received (resets timeout timer).
    void received();

    /// Stop heartbeat timers.
    void stop();

    bool isRunning() const { return running_; }

private:
    static constexpr int SEND_INTERVAL_MS     = 10000;
    static constexpr int TIMEOUT_THRESHOLD_MS = 15000;
    static constexpr int CHECK_INTERVAL_MS    = 5000;

    std::function<void(const std::vector<uint8_t>&)> onSend_;
    std::function<void()> onTimeout_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point lastReceived_;
    std::chrono::steady_clock::time_point lastSent_;
    std::mutex mutex_;
    std::thread timerThread_;

    void timerLoop();
};

} // namespace danws
