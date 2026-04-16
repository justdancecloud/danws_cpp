#include "danws/connection/reconnect_engine.h"
#include <cmath>
#include <random>

namespace danws {

ReconnectEngine::ReconnectEngine(const ReconnectOptions& options)
    : options_(options) {}

ReconnectEngine::~ReconnectEngine() {
    dispose();
}

void ReconnectEngine::onReconnect(std::function<void(int, int)> callback) {
    onReconnect_ = std::move(callback);
}

void ReconnectEngine::onExhausted(std::function<void()> callback) {
    onExhausted_ = std::move(callback);
}

void ReconnectEngine::onAttempt(std::function<void()> callback) {
    onAttempt_ = std::move(callback);
}

int ReconnectEngine::calculateDelay(int attempt) const {
    double raw = options_.baseDelayMs * std::pow(options_.backoffMultiplier, attempt - 1);
    int capped = static_cast<int>(std::min(raw, static_cast<double>(options_.maxDelayMs)));

    if (options_.jitter) {
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<double> dist(0.5, 1.5);
        return static_cast<int>(capped * dist(gen));
    }
    return capped;
}

void ReconnectEngine::start() {
    if (!options_.enabled || active_) return;
    active_ = true;
    attempt_ = 0;
    stopRequested_ = false;
    scheduleNext();
}

void ReconnectEngine::stop() {
    stopRequested_ = true;
    active_ = false;
    attempt_ = 0;
    cv_.notify_all();
    if (timerThread_.joinable()) {
        timerThread_.join();
    }
}

void ReconnectEngine::retry() {
    if (active_) {
        scheduleNext();
    }
}

void ReconnectEngine::scheduleNext() {
    attempt_++;

    if (options_.maxRetries > 0 && attempt_ > options_.maxRetries) {
        active_ = false;
        if (onExhausted_) onExhausted_();
        return;
    }

    int delay = calculateDelay(attempt_);

    if (onReconnect_) {
        onReconnect_(attempt_.load(), delay);
    }

    // Wait for previous timer to finish
    if (timerThread_.joinable()) {
        timerThread_.join();
    }

    timerThread_ = std::thread([this, delay]() { timerLoop(delay); });
}

void ReconnectEngine::timerLoop(int delayMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(delayMs), [this]() {
        return stopRequested_.load();
    });

    if (!stopRequested_ && onAttempt_) {
        onAttempt_();
    }
}

void ReconnectEngine::dispose() {
    stop();
    onReconnect_ = nullptr;
    onExhausted_ = nullptr;
    onAttempt_ = nullptr;
}

} // namespace danws
