#include "danws/api/session.h"

namespace danws {

DanWebSocketSession::DanWebSocketSession(const std::string& clientUuid)
    : id_(clientUuid) {}

void DanWebSocketSession::onReady(std::function<void()> cb) {
    onReady_.push_back(std::move(cb));
}

void DanWebSocketSession::onDisconnect(std::function<void()> cb) {
    onDisconnect_.push_back(std::move(cb));
}

// --- Session-level data (topic modes) ---

void DanWebSocketSession::set(const std::string& key, const Payload& value) {
    if (!sessionBound_ || !flatState_) {
        throw DanWSError("INVALID_MODE", "session.set() is only available in topic modes.");
    }
    flatState_->set(key, value);
}

Payload DanWebSocketSession::get(const std::string& key) const {
    if (!flatState_) return std::monostate{};
    return flatState_->get(key);
}

std::vector<std::string> DanWebSocketSession::keys() const {
    if (!flatState_) return {};
    return flatState_->keys();
}

void DanWebSocketSession::clearKey(const std::string& key) {
    if (!sessionBound_ || !flatState_) return;
    flatState_->clear(key);
}

void DanWebSocketSession::clearKey() {
    if (!sessionBound_ || !flatState_) return;
    flatState_->clear();
}

// --- Topic handles ---

TopicHandle* DanWebSocketSession::getTopicHandle(const std::string& name) {
    auto it = topicHandles_.find(name);
    if (it != topicHandles_.end()) return it->second.get();
    return nullptr;
}

std::vector<std::string> DanWebSocketSession::topics() const {
    std::vector<std::string> result;
    for (const auto& pair : topicHandles_) {
        result.push_back(pair.first);
    }
    return result;
}

// --- Internal ---

void DanWebSocketSession::_setEnqueue(std::function<void(const Frame&)> fn) {
    enqueueFrame_ = std::move(fn);
}

void DanWebSocketSession::_setTxProviders(
    std::function<std::vector<Frame>()> keyFrames,
    std::function<std::vector<Frame>()> valueFrames) {
    txKeyFrameProvider_ = std::move(keyFrames);
    txValueFrameProvider_ = std::move(valueFrames);
}

void DanWebSocketSession::_authorize(const std::string& principal) {
    principal_ = principal;
    authorized_ = true;
    state_ = SessionState::Authorized;
}

void DanWebSocketSession::_startSync() {
    state_ = SessionState::Synchronizing;
    serverSyncSent_ = false;

    if (txKeyFrameProvider_ && enqueueFrame_) {
        auto frames = txKeyFrameProvider_();
        if (!frames.empty()) {
            for (const auto& f : frames) enqueueFrame_(f);
            serverSyncSent_ = true;
        } else {
            enqueueFrame_({ FrameType::ServerSync, 0, DataType::Null, std::monostate{} });
            serverSyncSent_ = true;
        }
    } else {
        state_ = SessionState::Ready;
        emit(onReady_);
    }
}

void DanWebSocketSession::_handleFrame(const Frame& frame) {
    switch (frame.frameType) {
        case FrameType::ClientReady:
            if (state_ == SessionState::Ready) return;
            if (txValueFrameProvider_ && enqueueFrame_) {
                for (const auto& vf : txValueFrameProvider_()) {
                    enqueueFrame_(vf);
                }
            }
            if (serverSyncSent_) {
                state_ = SessionState::Ready;
                emit(onReady_);
            }
            break;

        case FrameType::ClientResyncReq:
            if (txKeyFrameProvider_ && enqueueFrame_) {
                enqueueFrame_({ FrameType::ServerReset, 0, DataType::Null, std::monostate{} });
                for (const auto& f : txKeyFrameProvider_()) {
                    enqueueFrame_(f);
                }
            }
            break;

        case FrameType::ClientKeyRequest:
            handleKeyRequest(frame.keyId);
            break;

        default:
            break;
    }
}

void DanWebSocketSession::_handleDisconnect() {
    connected_ = false;
    state_ = SessionState::Disconnected;
    emit(onDisconnect_);
}

void DanWebSocketSession::_handleReconnect() {
    connected_ = true;
    state_ = SessionState::Authorized;
}

void DanWebSocketSession::_bindSessionTX(std::function<void(const Frame&)> enqueue) {
    sessionEnqueue_ = std::move(enqueue);
    sessionBound_ = true;
    flatState_ = std::make_unique<FlatState>(FlatState::Callbacks{
        [this]() -> uint32_t { return nextKeyId_++; },
        sessionEnqueue_,
        [this]() { triggerSessionResync(); },
        ""  // no prefix for session-level flat state
    });
}

TopicHandle& DanWebSocketSession::_createTopicHandle(
    const std::string& name,
    const std::map<std::string, Payload>& params,
    int wireIndex) {
    int index = (wireIndex >= 0) ? wireIndex : topicIndex_++;
    if (index >= topicIndex_) topicIndex_ = index + 1;

    auto payload = std::make_shared<TopicPayload>(
        index, [this]() -> uint32_t { return nextKeyId_++; });

    if (sessionEnqueue_) {
        payload->_bind(sessionEnqueue_, [this]() { triggerSessionResync(); });
    }

    auto handle = std::make_shared<TopicHandle>(name, params, payload, *this);
    topicHandles_[name] = handle;
    return *handle;
}

void DanWebSocketSession::_removeTopicHandle(const std::string& name) {
    auto it = topicHandles_.find(name);
    if (it != topicHandles_.end()) {
        it->second->_dispose();
        topicHandles_.erase(it);
        triggerSessionResync();
    }
}

void DanWebSocketSession::_disposeAllTopicHandles() {
    for (auto& pair : topicHandles_) {
        pair.second->_dispose();
    }
    topicHandles_.clear();
}

void DanWebSocketSession::triggerSessionResync() {
    if (!sessionEnqueue_) return;

    // ServerReset
    sessionEnqueue_({ FrameType::ServerReset, 0, DataType::Null, std::monostate{} });

    // Flat state key frames
    std::vector<Frame> flatValueFrames;
    if (flatState_) {
        auto keyFrames = flatState_->buildKeyFrames();
        for (const auto& f : keyFrames) sessionEnqueue_(f);
        flatValueFrames = flatState_->buildValueFrames();
    }

    // Topic payload key frames
    for (auto& [name, handle] : topicHandles_) {
        for (const auto& f : handle->payload()._buildKeyFrames()) {
            sessionEnqueue_(f);
        }
    }

    // ServerSync
    sessionEnqueue_({ FrameType::ServerSync, 0, DataType::Null, std::monostate{} });

    // Flat state value frames
    for (const auto& f : flatValueFrames) {
        sessionEnqueue_(f);
    }

    // Topic payload value frames
    for (auto& [name, handle] : topicHandles_) {
        for (const auto& f : handle->payload()._buildValueFrames()) {
            sessionEnqueue_(f);
        }
    }
}

void DanWebSocketSession::handleKeyRequest(uint32_t keyId) {
    if (!enqueueFrame_) return;

    Frame syncFrame{ FrameType::ServerSync, 0, DataType::Null, std::monostate{} };

    // Search TX providers (broadcast/principal mode)
    if (txKeyFrameProvider_ && txValueFrameProvider_) {
        auto keyFrames = txKeyFrameProvider_();
        for (const auto& f : keyFrames) {
            if (f.frameType == FrameType::ServerKeyRegistration && f.keyId == keyId) {
                enqueueFrame_(f);
                enqueueFrame_(syncFrame);
                auto valueFrames = txValueFrameProvider_();
                for (const auto& vf : valueFrames) {
                    if (vf.keyId == keyId) { enqueueFrame_(vf); break; }
                }
                return;
            }
        }
    }

    // Search session-level flat state
    if (flatState_) {
        auto* entry = flatState_->getByKeyId(keyId);
        if (entry) {
            std::string wirePath = flatState_->getPathByKeyId(keyId);
            enqueueFrame_({ FrameType::ServerKeyRegistration, entry->keyId,
                           entry->type, Payload(wirePath) });
            enqueueFrame_(syncFrame);
            enqueueFrame_({ FrameType::ServerValue, entry->keyId,
                           entry->type, entry->value });
            return;
        }
    }

    // Search topic payloads
    for (auto& [name, handle] : topicHandles_) {
        for (const auto& f : handle->payload()._buildKeyFrames()) {
            if (f.keyId == keyId) {
                enqueueFrame_(f);
                enqueueFrame_(syncFrame);
                for (const auto& vf : handle->payload()._buildValueFrames()) {
                    if (vf.keyId == keyId) { enqueueFrame_(vf); break; }
                }
                return;
            }
        }
    }
}

} // namespace danws
