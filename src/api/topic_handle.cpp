#include "danws/api/topic_handle.h"
#include "danws/api/session.h"

namespace danws {

// --- TopicPayload ---

TopicPayload::TopicPayload(int index, std::function<uint32_t()> allocateKeyId)
    : index_(index), allocateKeyId_(std::move(allocateKeyId)) {
    flatState_ = std::make_unique<FlatState>(FlatState::Callbacks{
        allocateKeyId_,
        [](const Frame&) {},  // noop enqueue before bind
        []() {},              // noop resync before bind
        "t." + std::to_string(index) + "."
    });
}

void TopicPayload::_bind(std::function<void(const Frame&)> enqueue,
                          std::function<void()> onResync) {
    flatState_ = std::make_unique<FlatState>(FlatState::Callbacks{
        allocateKeyId_,
        std::move(enqueue),
        std::move(onResync),
        "t." + std::to_string(index_) + "."
    });
}

void TopicPayload::set(const std::string& key, const Payload& value) {
    flatState_->set(key, value);
}

Payload TopicPayload::get(const std::string& key) const {
    return flatState_->get(key);
}

std::vector<std::string> TopicPayload::keys() const {
    return flatState_->keys();
}

void TopicPayload::clear() {
    flatState_->clear();
}

void TopicPayload::clear(const std::string& key) {
    flatState_->clear(key);
}

std::vector<Frame> TopicPayload::_buildKeyFrames() const {
    return flatState_->buildKeyFrames();
}

std::vector<Frame> TopicPayload::_buildValueFrames() const {
    return flatState_->buildValueFrames();
}

size_t TopicPayload::_size() const {
    return flatState_->size();
}

// --- TopicHandle ---

TopicHandle::TopicHandle(const std::string& name,
                         const std::map<std::string, Payload>& params,
                         std::shared_ptr<TopicPayload> payload,
                         DanWebSocketSession& session)
    : name_(name), params_(params), payload_(std::move(payload)), session_(session) {}

void TopicHandle::setCallback(TopicCallback fn) {
    callback_ = std::move(fn);
    if (callback_) {
        try { callback_(TopicEventType::Subscribe, *this, session_); }
        catch (...) {}
    }
}

void TopicHandle::setDelayedTask(int intervalMs) {
    clearDelayedTask();
    taskIntervalMs_ = intervalMs;
    taskStop_ = false;
    taskRunning_ = true;
    taskThread_ = std::thread([this, intervalMs]() { taskLoop(intervalMs); });
}

void TopicHandle::clearDelayedTask() {
    if (taskRunning_) {
        taskStop_ = true;
        if (taskThread_.joinable()) {
            taskThread_.join();
        }
        taskRunning_ = false;
    }
}

void TopicHandle::taskLoop(int intervalMs) {
    while (!taskStop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        if (taskStop_) break;
        if (callback_) {
            try { callback_(TopicEventType::DelayedTask, *this, session_); }
            catch (...) {}
        }
    }
}

void TopicHandle::_updateParams(const std::map<std::string, Payload>& newParams) {
    params_ = newParams;
    bool hadTask = taskRunning_.load();
    int savedMs = taskIntervalMs_;

    clearDelayedTask();

    if (callback_) {
        try { callback_(TopicEventType::ChangedParams, *this, session_); }
        catch (...) {}
    }

    if (hadTask && savedMs > 0) {
        setDelayedTask(savedMs);
    }
}

void TopicHandle::_dispose() {
    clearDelayedTask();
    callback_ = nullptr;
}

} // namespace danws
