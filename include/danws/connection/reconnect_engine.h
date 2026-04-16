#pragma once

#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace danws {

struct ReconnectOptions {
    bool enabled           = true;
    int  maxRetries        = 10;       // 0 = unlimited
    int  baseDelayMs       = 1000;
    int  maxDelayMs        = 30000;
    double backoffMultiplier = 2.0;
    bool jitter            = true;
};

/// Manages reconnection attempts with exponential backoff and jitter.
class ReconnectEngine {
public:
    explicit ReconnectEngine(const ReconnectOptions& options = {});
    ~ReconnectEngine();

    /// Called before each reconnect attempt with (attempt, delayMs).
    void onReconnect(std::function<void(int, int)> callback);

    /// Called when all retry attempts are exhausted.
    void onExhausted(std::function<void()> callback);

    /// Called when the timer fires and a reconnect attempt should be made.
    void onAttempt(std::function<void()> callback);

    int attempt() const { return attempt_; }
    bool isActive() const { return active_; }

    /// Start the reconnection cycle.
    void start();

    /// Stop the reconnection cycle.
    void stop();

    /// Called when a reconnect attempt fails; schedules the next attempt.
    void retry();

    /// Calculate delay for a given attempt (1-indexed).
    int calculateDelay(int attempt) const;

    void dispose();

private:
    ReconnectOptions options_;
    std::atomic<int> attempt_{0};
    std::atomic<bool> active_{false};
    std::atomic<bool> stopRequested_{false};

    std::function<void(int, int)> onReconnect_;
    std::function<void()> onExhausted_;
    std::function<void()> onAttempt_;

    std::thread timerThread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    void scheduleNext();
    void timerLoop(int delayMs);
};

} // namespace danws
