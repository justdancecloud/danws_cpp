#pragma once

#include "../protocol/types.h"
#include "flat_state.h"
#include <string>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>

namespace danws {

class DanWebSocketSession;

enum class TopicEventType {
    Subscribe,
    ChangedParams,
    DelayedTask,
};

/// Scoped payload store for a single topic within a session.
class TopicPayload {
public:
    TopicPayload(int index,
                 std::function<uint32_t()> allocateKeyId);

    void _bind(std::function<void(const Frame&)> enqueue,
               std::function<void()> onResync);

    void set(const std::string& key, const Payload& value);
    Payload get(const std::string& key) const;
    std::vector<std::string> keys() const;
    void clear();
    void clear(const std::string& key);

    std::vector<Frame> _buildKeyFrames() const;
    std::vector<Frame> _buildValueFrames() const;

    int _idx() const { return index_; }
    size_t _size() const;

private:
    int index_;
    std::function<uint32_t()> allocateKeyId_;
    std::unique_ptr<FlatState> flatState_;
};

using TopicCallback = std::function<void(TopicEventType, class TopicHandle&, DanWebSocketSession&)>;

/// Server-side handle for a single client topic subscription.
class TopicHandle {
public:
    TopicHandle(const std::string& name,
                const std::map<std::string, Payload>& params,
                std::shared_ptr<TopicPayload> payload,
                DanWebSocketSession& session);

    const std::string& name() const { return name_; }
    const std::map<std::string, Payload>& params() const { return params_; }
    TopicPayload& payload() { return *payload_; }

    void setCallback(TopicCallback fn);
    void setDelayedTask(int intervalMs);
    void clearDelayedTask();

    // Internal
    void _updateParams(const std::map<std::string, Payload>& newParams);
    void _dispose();

private:
    std::string name_;
    std::map<std::string, Payload> params_;
    std::shared_ptr<TopicPayload> payload_;
    DanWebSocketSession& session_;
    TopicCallback callback_;

    // Delayed task
    std::atomic<bool> taskRunning_{false};
    std::atomic<bool> taskStop_{false};
    int taskIntervalMs_ = 0;
    std::thread taskThread_;

    void taskLoop(int intervalMs);
};

/// Namespace for topic subscribe/unsubscribe event registration.
struct TopicNamespace {
    std::vector<std::function<void(DanWebSocketSession&, TopicHandle&)>> onSubscribeCbs;
    std::vector<std::function<void(DanWebSocketSession&, TopicHandle&)>> onUnsubscribeCbs;

    void onSubscribe(std::function<void(DanWebSocketSession&, TopicHandle&)> cb) {
        onSubscribeCbs.push_back(std::move(cb));
    }
    void onUnsubscribe(std::function<void(DanWebSocketSession&, TopicHandle&)> cb) {
        onUnsubscribeCbs.push_back(std::move(cb));
    }
};

} // namespace danws
