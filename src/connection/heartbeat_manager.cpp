#include "danws/connection/heartbeat_manager.h"
#include "danws/protocol/codec.h"

namespace danws {

HeartbeatManager::HeartbeatManager() = default;

HeartbeatManager::~HeartbeatManager() {
    stop();
}

void HeartbeatManager::onSend(std::function<void(const std::vector<uint8_t>&)> callback) {
    onSend_ = std::move(callback);
}

void HeartbeatManager::onTimeout(std::function<void()> callback) {
    onTimeout_ = std::move(callback);
}

void HeartbeatManager::start() {
    stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        lastReceived_ = now;
        lastSent_ = now;
    }

    stopRequested_ = false;
    running_ = true;

    timerThread_ = std::thread([this]() { timerLoop(); });
}

void HeartbeatManager::received() {
    std::lock_guard<std::mutex> lock(mutex_);
    lastReceived_ = std::chrono::steady_clock::now();
}

void HeartbeatManager::stop() {
    if (!running_) return;
    stopRequested_ = true;
    running_ = false;
    if (timerThread_.joinable()) {
        timerThread_.join();
    }
}

void HeartbeatManager::timerLoop() {
    using namespace std::chrono;

    while (!stopRequested_) {
        std::this_thread::sleep_for(milliseconds(CHECK_INTERVAL_MS));
        if (stopRequested_) break;

        auto now = steady_clock::now();

        // Check timeout
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto elapsed = duration_cast<milliseconds>(now - lastReceived_).count();
            if (elapsed > TIMEOUT_THRESHOLD_MS) {
                running_ = false;
                if (onTimeout_) {
                    onTimeout_();
                }
                return;
            }
        }

        // Send heartbeat if interval has elapsed
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto sinceLastSend = duration_cast<milliseconds>(now - lastSent_).count();
            if (sinceLastSend >= SEND_INTERVAL_MS) {
                lastSent_ = now;
                if (onSend_) {
                    auto hb = encodeHeartbeat();
                    onSend_(hb);
                }
            }
        }
    }
}

} // namespace danws
