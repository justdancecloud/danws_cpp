#pragma once

#include "../protocol/types.h"
#include "../protocol/error.h"
#include "flat_state.h"
#include "topic_handle.h"
#include <string>
#include <functional>
#include <vector>
#include <map>
#include <memory>

namespace danws {

enum class SessionState {
    Pending,
    Authorized,
    Synchronizing,
    Ready,
    Disconnected,
};

/// Server-side representation of a connected client session.
class DanWebSocketSession {
public:
    explicit DanWebSocketSession(const std::string& clientUuid);

    const std::string& id() const { return id_; }
    const std::string& principal() const { return principal_; }
    bool authorized() const { return authorized_; }
    bool connected() const { return connected_; }
    SessionState state() const { return state_; }

    // Event registration
    void onReady(std::function<void()> cb);
    void onDisconnect(std::function<void()> cb);

    // Session-level data (topic modes)
    void set(const std::string& key, const Payload& value);
    Payload get(const std::string& key) const;
    std::vector<std::string> keys() const;
    void clearKey(const std::string& key);
    void clearKey();

    // Topic handles
    TopicHandle* getTopicHandle(const std::string& name);
    std::vector<std::string> topics() const;

    // Internal methods (used by DanWebSocketServer)
    void _setEnqueue(std::function<void(const Frame&)> fn);
    void _setTxProviders(std::function<std::vector<Frame>()> keyFrames,
                         std::function<std::vector<Frame>()> valueFrames);
    void _authorize(const std::string& principal);
    void _startSync();
    void _handleFrame(const Frame& frame);
    void _handleDisconnect();
    void _handleReconnect();

    // Session-level TX (topic modes)
    void _bindSessionTX(std::function<void(const Frame&)> enqueue);

    // Topic management
    int _nextTopicIndex() const { return topicIndex_; }
    TopicHandle& _createTopicHandle(const std::string& name,
                                     const std::map<std::string, Payload>& params,
                                     int wireIndex = -1);
    void _removeTopicHandle(const std::string& name);
    void _disposeAllTopicHandles();

private:
    std::string id_;
    std::string principal_;
    bool authorized_ = false;
    bool connected_ = true;
    SessionState state_ = SessionState::Pending;

    std::function<void(const Frame&)> enqueueFrame_;

    // TX providers (broadcast/principal mode)
    std::function<std::vector<Frame>()> txKeyFrameProvider_;
    std::function<std::vector<Frame>()> txValueFrameProvider_;
    bool serverSyncSent_ = false;

    // Session-level flat TX (topic modes)
    uint32_t nextKeyId_ = 1;
    std::function<void(const Frame&)> sessionEnqueue_;
    bool sessionBound_ = false;
    std::unique_ptr<FlatState> flatState_;

    // Topic handles
    std::map<std::string, std::shared_ptr<TopicHandle>> topicHandles_;
    int topicIndex_ = 0;

    // Callbacks
    std::vector<std::function<void()>> onReady_;
    std::vector<std::function<void()>> onDisconnect_;

    void triggerSessionResync();
    void handleKeyRequest(uint32_t keyId);

    template<typename... Args>
    void emit(const std::vector<std::function<void(Args...)>>& callbacks, Args... args) {
        for (const auto& cb : callbacks) {
            try { cb(args...); } catch (...) {}
        }
    }
};

} // namespace danws
