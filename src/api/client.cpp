#include "danws/api/client.h"
#include "danws/protocol/codec.h"
#include "danws/protocol/serializer.h"

#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring>

namespace danws {

// --- UUIDv7 generation ---

std::string DanWebSocketClient::generateUUIDv7() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    uint8_t bytes[16];
    // First 6 bytes: timestamp
    bytes[0] = static_cast<uint8_t>((ms >> 40) & 0xFF);
    bytes[1] = static_cast<uint8_t>((ms >> 32) & 0xFF);
    bytes[2] = static_cast<uint8_t>((ms >> 24) & 0xFF);
    bytes[3] = static_cast<uint8_t>((ms >> 16) & 0xFF);
    bytes[4] = static_cast<uint8_t>((ms >> 8) & 0xFF);
    bytes[5] = static_cast<uint8_t>(ms & 0xFF);

    // Remaining 10 bytes: random
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (int i = 6; i < 16; ++i) {
        bytes[i] = static_cast<uint8_t>(dist(gen));
    }

    // Set version (7) and variant (10xx)
    bytes[6] = (bytes[6] & 0x0F) | 0x70;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as UUID string
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

// --- DataType detection ---

DataType DanWebSocketClient::detectDataType(const Payload& value) {
    return std::visit([](const auto& v) -> DataType {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) return DataType::Null;
        else if constexpr (std::is_same_v<T, bool>) return DataType::Bool;
        else if constexpr (std::is_same_v<T, uint8_t>) return DataType::Uint8;
        else if constexpr (std::is_same_v<T, uint16_t>) return DataType::Uint16;
        else if constexpr (std::is_same_v<T, uint32_t>) return DataType::Uint32;
        else if constexpr (std::is_same_v<T, uint64_t>) return DataType::Uint64;
        else if constexpr (std::is_same_v<T, int32_t>) return DataType::VarInteger;
        else if constexpr (std::is_same_v<T, int64_t>) return DataType::Int64;
        else if constexpr (std::is_same_v<T, float>) return DataType::VarFloat;
        else if constexpr (std::is_same_v<T, double>) return DataType::VarDouble;
        else if constexpr (std::is_same_v<T, std::string>) return DataType::String;
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) return DataType::Binary;
        else return DataType::Null;
    }, value);
}

// --- Constructor ---

DanWebSocketClient::DanWebSocketClient(const std::string& url,
                                       WebSocketFactory wsFactory,
                                       const ClientOptions& options)
    : id_(generateUUIDv7())
    , url_(url)
    , wsFactory_(std::move(wsFactory))
    , reconnectEngine_(options.reconnect)
    , debug_(options.debug) {

    parser_.onFrame([this](const Frame& frame) { handleFrame(frame); });
    parser_.onHeartbeat([this]() { heartbeat_.received(); });
    parser_.onError([this](const std::string& msg) { log("Stream parser error: " + msg); });

    setupInternals();
}

DanWebSocketClient::~DanWebSocketClient() {
    cleanup();
    reconnectEngine_.dispose();
}

void DanWebSocketClient::setupInternals() {
    bulkQueue_.onFlush([this](const std::vector<uint8_t>& data) { sendRaw(data); });

    heartbeat_.onSend([this](const std::vector<uint8_t>& data) { sendRaw(data); });
    heartbeat_.onTimeout([this]() {
        log("Heartbeat timeout");
        for (const auto& cb : onError_) {
            try { cb(DanWSError("HEARTBEAT_TIMEOUT", "No heartbeat received within 15 seconds")); }
            catch (...) {}
        }
        handleClose();
    });

    reconnectEngine_.onReconnect([this](int attempt, int delay) {
        emit(onReconnecting_, attempt, delay);
    });
    reconnectEngine_.onAttempt([this]() {
        connect();
    });
    reconnectEngine_.onExhausted([this]() {
        state_ = ClientState::Disconnected;
        for (const auto& cb : onError_) {
            try { cb(DanWSError("RECONNECT_EXHAUSTED", "All reconnection attempts exhausted")); }
            catch (...) {}
        }
        emit(onReconnectFailed_);
    });
}

// --- Connection ---

void DanWebSocketClient::connect() {
    if (state_ != ClientState::Disconnected && state_ != ClientState::Reconnecting) return;
    intentionalDisconnect_ = false;
    state_ = ClientState::Connecting;

    try {
        ws_ = wsFactory_();
        ws_->onOpen = [this]() { handleOpen(); };
        ws_->onClose = [this]() { handleClose(); };
        ws_->onError = [this](const std::string& msg) { log("WebSocket error: " + msg); };
        ws_->onMessage = [this](const std::vector<uint8_t>& data) {
            parser_.feed(data);
        };
        ws_->connect(url_);
    } catch (const std::exception& e) {
        log(std::string("Connect failed: ") + e.what());
        handleClose();
    }
}

void DanWebSocketClient::disconnect() {
    intentionalDisconnect_ = true;
    reconnectEngine_.stop();
    cleanup();
    state_ = ClientState::Disconnected;
    emit(onDisconnect_);
}

void DanWebSocketClient::authorize(const std::string& token) {
    if (!ws_ || !ws_->isOpen()) return;
    Frame frame;
    frame.frameType = FrameType::Auth;
    frame.keyId = 0;
    frame.dataType = DataType::String;
    frame.payload = token;
    sendFrame(frame);
    state_ = ClientState::Authorizing;
}

// --- State access ---

Payload DanWebSocketClient::get(const std::string& key) const {
    const auto* entry = registry_.getByPath(key);
    if (!entry) return std::monostate{};
    auto it = store_.find(entry->keyId);
    if (it == store_.end()) return std::monostate{};
    return it->second;
}

std::vector<std::string> DanWebSocketClient::keys() const {
    return registry_.paths();
}

// --- Topic API ---

void DanWebSocketClient::subscribe(const std::string& topicName,
                                    const std::map<std::string, Payload>& params) {
    subscriptions_[topicName] = params;
    sendTopicSync();
}

void DanWebSocketClient::unsubscribe(const std::string& topicName) {
    if (subscriptions_.erase(topicName)) {
        topicHandles_.erase(topicName);
        sendTopicSync();
    }
}

TopicClientHandle* DanWebSocketClient::topic(const std::string& name) {
    auto it = topicHandles_.find(name);
    if (it != topicHandles_.end()) return it->second.get();

    int idx = -1;
    auto idxIt = topicIndexMap_.find(name);
    if (idxIt != topicIndexMap_.end()) idx = idxIt->second;

    auto handle = std::make_unique<TopicClientHandle>(
        name, idx, registry_,
        [this](uint32_t id) -> Payload {
            auto storeIt = store_.find(id);
            if (storeIt == store_.end()) return std::monostate{};
            return storeIt->second;
        });
    auto* ptr = handle.get();
    topicHandles_[name] = std::move(handle);
    return ptr;
}

std::vector<std::string> DanWebSocketClient::topics() const {
    std::vector<std::string> result;
    for (const auto& pair : subscriptions_) {
        result.push_back(pair.first);
    }
    return result;
}

// --- Event registration ---

std::function<void()> DanWebSocketClient::onConnect(std::function<void()> cb) {
    return addCallback(onConnect_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onDisconnect(std::function<void()> cb) {
    return addCallback(onDisconnect_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onReady(std::function<void()> cb) {
    return addCallback(onReady_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onReceive(
    std::function<void(const std::string&, const Payload&)> cb) {
    return addCallback(onReceive_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onUpdate(std::function<void()> cb) {
    return addCallback(onUpdate_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onReconnecting(std::function<void(int, int)> cb) {
    return addCallback(onReconnecting_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onReconnect(std::function<void()> cb) {
    return addCallback(onReconnect_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onReconnectFailed(std::function<void()> cb) {
    return addCallback(onReconnectFailed_, std::move(cb));
}
std::function<void()> DanWebSocketClient::onError(std::function<void(const DanWSError&)> cb) {
    return addCallback(onError_, std::move(cb));
}

// --- Internal handlers ---

void DanWebSocketClient::handleOpen() {
    state_ = ClientState::Identifying;
    heartbeat_.start();

    // Build IDENTIFY frame: 16-byte UUID + 2-byte protocol version (3.5)
    // Parse UUID string to 16 bytes
    std::string uuidClean;
    for (char c : id_) {
        if (c != '-') uuidClean += c;
    }

    std::vector<uint8_t> uuidBytes;
    for (size_t i = 0; i < uuidClean.size(); i += 2) {
        uuidBytes.push_back(static_cast<uint8_t>(
            std::stoi(uuidClean.substr(i, 2), nullptr, 16)));
    }
    // Append protocol version: 3.5 -> major=3, minor=5
    uuidBytes.push_back(3);
    uuidBytes.push_back(5);

    Frame identifyFrame;
    identifyFrame.frameType = FrameType::Identify;
    identifyFrame.keyId = 0;
    identifyFrame.dataType = DataType::Binary;
    identifyFrame.payload = uuidBytes;
    sendFrame(identifyFrame);

    emit(onConnect_);

    if (topicDirty_ && !subscriptions_.empty()) {
        sendTopicSync();
    }
}

void DanWebSocketClient::handleClose() {
    heartbeat_.stop();
    bulkQueue_.clear();

    if (ws_) {
        ws_->onOpen = nullptr;
        ws_->onClose = nullptr;
        ws_->onError = nullptr;
        ws_->onMessage = nullptr;
        try { ws_->close(); } catch (...) {}
        ws_.reset();
    }

    if (intentionalDisconnect_) return;

    emit(onDisconnect_);

    if (reconnectEngine_.isActive()) {
        state_ = ClientState::Reconnecting;
        reconnectEngine_.retry();
    } else {
        state_ = ClientState::Reconnecting;
        reconnectEngine_.start();
    }
}

const DanWebSocketClient::TopicInfo* DanWebSocketClient::getTopicInfo(
    uint32_t keyId, const std::string& path) {
    auto cacheIt = topicKeyCache_.find(keyId);
    if (cacheIt != topicKeyCache_.end()) {
        return cacheIt->second.has_value() ? &cacheIt->second.value() : nullptr;
    }

    // Parse "t.<idx>.<userKey>" pattern
    if (path.size() > 2 && path[0] == 't' && path[1] == '.') {
        auto secondDot = path.find('.', 2);
        if (secondDot != std::string::npos) {
            try {
                int idx = std::stoi(path.substr(2, secondDot - 2));
                TopicInfo info{ idx, path.substr(secondDot + 1) };
                topicKeyCache_[keyId] = info;
                return &topicKeyCache_[keyId].value();
            } catch (...) {}
        }
    }

    topicKeyCache_[keyId] = std::nullopt;
    return nullptr;
}

void DanWebSocketClient::handleFrame(const Frame& frame) {
    switch (frame.frameType) {
        case FrameType::AuthOk:
            state_ = ClientState::Synchronizing;
            break;

        case FrameType::AuthFail: {
            intentionalDisconnect_ = true;
            auto msg = std::get_if<std::string>(&frame.payload);
            DanWSError err("AUTH_REJECTED", msg ? *msg : "Authentication failed");
            for (const auto& cb : onError_) {
                try { cb(err); } catch (...) {}
            }
            cleanup();
            state_ = ClientState::Disconnected;
            emit(onDisconnect_);
            break;
        }

        case FrameType::ServerKeyRegistration: {
            if (state_ == ClientState::Identifying) {
                state_ = ClientState::Synchronizing;
            }
            auto* pathPtr = std::get_if<std::string>(&frame.payload);
            if (!pathPtr) break;
            const std::string& keyPath = *pathPtr;
            registry_.registerOne(frame.keyId, keyPath, frame.dataType);

            // Apply any pending value
            auto pendIt = pendingValues_.find(frame.keyId);
            if (pendIt != pendingValues_.end()) {
                store_[frame.keyId] = pendIt->second.payload;
                auto* topicInfo = getTopicInfo(frame.keyId, keyPath);
                if (topicInfo) {
                    auto topicIt = indexToTopic_.find(topicInfo->topicIdx);
                    if (topicIt != indexToTopic_.end()) {
                        auto handleIt = topicHandles_.find(topicIt->second);
                        if (handleIt != topicHandles_.end()) {
                            handleIt->second->_notify(topicInfo->userKey, pendIt->second.payload);
                        }
                    }
                } else {
                    emit(onReceive_, keyPath, pendIt->second.payload);
                }
                pendingValues_.erase(pendIt);
            }
            break;
        }

        case FrameType::ServerSync: {
            if (state_ == ClientState::Identifying) {
                state_ = ClientState::Synchronizing;
            }
            if (state_ != ClientState::Ready) {
                Frame readyFrame;
                readyFrame.frameType = FrameType::ClientReady;
                readyFrame.keyId = 0;
                readyFrame.dataType = DataType::Null;
                readyFrame.payload = std::monostate{};
                bulkQueue_.enqueue(readyFrame);
            }

            if (registry_.size() == 0) {
                state_ = ClientState::Ready;
                emit(onReady_);
                if (reconnectEngine_.isActive()) {
                    reconnectEngine_.stop();
                    emit(onReconnect_);
                }
                if (!subscriptions_.empty()) {
                    sendTopicSync();
                }
            }
            break;
        }

        case FrameType::ServerValue: {
            if (!registry_.hasKeyId(frame.keyId)) {
                // Request this specific key
                Frame req;
                req.frameType = FrameType::ClientKeyRequest;
                req.keyId = frame.keyId;
                req.dataType = DataType::Null;
                req.payload = std::monostate{};
                bulkQueue_.enqueue(req);
                pendingValues_[frame.keyId] = frame;
                break;
            }

            store_[frame.keyId] = frame.payload;

            const auto* entry = registry_.getByKeyId(frame.keyId);
            if (entry) {
                auto* topicInfo = getTopicInfo(frame.keyId, entry->path);
                if (topicInfo) {
                    auto topicIt = indexToTopic_.find(topicInfo->topicIdx);
                    if (topicIt != indexToTopic_.end()) {
                        auto handleIt = topicHandles_.find(topicIt->second);
                        if (handleIt != topicHandles_.end()) {
                            handleIt->second->_notify(topicInfo->userKey, frame.payload);
                        }
                    }
                } else {
                    emit(onReceive_, entry->path, frame.payload);
                }
            }

            if (state_ == ClientState::Synchronizing) {
                state_ = ClientState::Ready;
                emit(onReady_);
                if (reconnectEngine_.isActive()) {
                    reconnectEngine_.stop();
                    emit(onReconnect_);
                }
                if (!subscriptions_.empty()) {
                    sendTopicSync();
                }
            }
            break;
        }

        case FrameType::ArrayShiftLeft: {
            const auto* lengthEntry = registry_.getByKeyId(frame.keyId);
            if (!lengthEntry) break;

            const std::string& lengthPath = lengthEntry->path;
            auto* topicInfo = getTopicInfo(frame.keyId, lengthPath);
            std::string prefix = lengthPath.substr(0, lengthPath.size() - 7); // remove ".length"

            auto* shiftPtr = std::get_if<int32_t>(&frame.payload);
            int rawShift = shiftPtr ? *shiftPtr : 0;

            auto storeIt = store_.find(frame.keyId);
            int currentLength = 0;
            if (storeIt != store_.end()) {
                if (auto* v = std::get_if<int32_t>(&storeIt->second)) currentLength = *v;
            }

            int shiftCount = std::max(0, std::min(rawShift, currentLength));

            for (int i = 0; i < currentLength - shiftCount; ++i) {
                const auto* src = registry_.getByPath(prefix + "." + std::to_string(i + shiftCount));
                const auto* dst = registry_.getByPath(prefix + "." + std::to_string(i));
                if (src && dst) {
                    auto srcIt = store_.find(src->keyId);
                    if (srcIt != store_.end()) {
                        store_[dst->keyId] = srcIt->second;
                    }
                }
            }

            int32_t newLength = currentLength - shiftCount;
            store_[frame.keyId] = newLength;

            if (topicInfo) {
                auto topicIt = indexToTopic_.find(topicInfo->topicIdx);
                if (topicIt != indexToTopic_.end()) {
                    auto handleIt = topicHandles_.find(topicIt->second);
                    if (handleIt != topicHandles_.end()) {
                        std::string userPrefix = topicInfo->userKey.substr(
                            0, topicInfo->userKey.size() - 7);
                        handleIt->second->_notify(userPrefix + ".length", Payload(newLength));
                    }
                }
            } else {
                Payload lengthPayload(newLength);
                emit(onReceive_, prefix + ".length", lengthPayload);
            }
            break;
        }

        case FrameType::ArrayShiftRight: {
            const auto* lengthEntry = registry_.getByKeyId(frame.keyId);
            if (!lengthEntry) break;

            const std::string& lengthPath = lengthEntry->path;
            auto* topicInfo = getTopicInfo(frame.keyId, lengthPath);
            std::string prefix = lengthPath.substr(0, lengthPath.size() - 7);

            auto* shiftPtr = std::get_if<int32_t>(&frame.payload);
            int rawShift = shiftPtr ? *shiftPtr : 0;

            auto storeIt = store_.find(frame.keyId);
            int currentLength = 0;
            if (storeIt != store_.end()) {
                if (auto* v = std::get_if<int32_t>(&storeIt->second)) currentLength = *v;
            }

            int shiftCount = std::max(0, std::min(rawShift, currentLength));

            for (int i = currentLength - 1; i >= 0; --i) {
                const auto* src = registry_.getByPath(prefix + "." + std::to_string(i));
                const auto* dst = registry_.getByPath(prefix + "." + std::to_string(i + shiftCount));
                if (src && dst) {
                    auto srcIt = store_.find(src->keyId);
                    if (srcIt != store_.end()) {
                        store_[dst->keyId] = srcIt->second;
                    }
                }
            }

            int32_t curLen = static_cast<int32_t>(currentLength);
            if (topicInfo) {
                auto topicIt = indexToTopic_.find(topicInfo->topicIdx);
                if (topicIt != indexToTopic_.end()) {
                    auto handleIt = topicHandles_.find(topicIt->second);
                    if (handleIt != topicHandles_.end()) {
                        std::string userPrefix = topicInfo->userKey.substr(
                            0, topicInfo->userKey.size() - 7);
                        handleIt->second->_notify(userPrefix + ".length", Payload(curLen));
                    }
                }
            } else {
                Payload lengthPayload(curLen);
                emit(onReceive_, prefix + ".length", lengthPayload);
            }
            break;
        }

        case FrameType::ServerFlushEnd: {
            emit(onUpdate_);
            for (auto& pair : topicHandles_) {
                pair.second->_flushUpdate();
            }
            break;
        }

        case FrameType::ServerReady:
            // No action needed
            break;

        case FrameType::ServerKeyDelete: {
            const auto* deletedEntry = registry_.getByKeyId(frame.keyId);
            std::string deletedPath;
            const TopicInfo* deletedTopicInfo = nullptr;
            if (deletedEntry) {
                deletedPath = deletedEntry->path;
                deletedTopicInfo = getTopicInfo(frame.keyId, deletedPath);
            }
            registry_.removeByKeyId(frame.keyId);
            store_.erase(frame.keyId);
            topicKeyCache_.erase(frame.keyId);

            if (!deletedPath.empty()) {
                if (deletedTopicInfo) {
                    auto topicIt = indexToTopic_.find(deletedTopicInfo->topicIdx);
                    if (topicIt != indexToTopic_.end()) {
                        auto handleIt = topicHandles_.find(topicIt->second);
                        if (handleIt != topicHandles_.end()) {
                            handleIt->second->_notify(deletedTopicInfo->userKey,
                                                      Payload(std::monostate{}));
                        }
                    }
                } else {
                    Payload nullPayload(std::monostate{});
                    emit(onReceive_, deletedPath, nullPayload);
                }
            }
            break;
        }

        case FrameType::ServerReset:
            registry_.clear();
            store_.clear();
            pendingValues_.clear();
            topicKeyCache_.clear();
            state_ = ClientState::Synchronizing;
            break;

        case FrameType::Error: {
            auto* msg = std::get_if<std::string>(&frame.payload);
            DanWSError err("REMOTE_ERROR", msg ? *msg : "Unknown error");
            for (const auto& cb : onError_) {
                try { cb(err); } catch (...) {}
            }
            break;
        }

        default:
            break;
    }
}

void DanWebSocketClient::sendTopicSync() {
    if (!ws_ || !ws_->isOpen()) {
        topicDirty_ = true;
        return;
    }

    topicIndexMap_.clear();
    indexToTopic_.clear();

    struct TopicEntry { std::string path; Payload value; };
    std::vector<TopicEntry> entries;

    int idx = 0;
    for (const auto& [topicName, params] : subscriptions_) {
        topicIndexMap_[topicName] = idx;
        indexToTopic_[idx] = topicName;

        auto handleIt = topicHandles_.find(topicName);
        if (handleIt != topicHandles_.end()) {
            handleIt->second->_setIndex(idx);
        } else {
            auto handle = std::make_unique<TopicClientHandle>(
                topicName, idx, registry_,
                [this](uint32_t id) -> Payload {
                    auto it = store_.find(id);
                    return it != store_.end() ? it->second : Payload(std::monostate{});
                });
            topicHandles_[topicName] = std::move(handle);
        }

        entries.push_back({ "topic." + std::to_string(idx) + ".name",
                           Payload(topicName) });
        for (const auto& [paramKey, paramValue] : params) {
            entries.push_back({
                "topic." + std::to_string(idx) + ".param." + paramKey,
                paramValue });
        }
        idx++;
    }

    // Send ClientReset
    sendFrame({ FrameType::ClientReset, 0, DataType::Null, std::monostate{} });

    // Send key registrations and collect keyIds
    struct KeyIdEntry { uint32_t id; Payload value; DataType dataType; };
    std::vector<KeyIdEntry> keyIds;
    uint32_t keyId = 1;
    for (const auto& entry : entries) {
        DataType dt = detectDataType(entry.value);
        sendFrame({ FrameType::ClientKeyRegistration, keyId, dt,
                    Payload(entry.path) });
        keyIds.push_back({ keyId, entry.value, dt });
        keyId++;
    }

    // Send values
    for (const auto& entry : keyIds) {
        sendFrame({ FrameType::ClientValue, entry.id, entry.dataType, entry.value });
    }

    // Send ClientSync
    sendFrame({ FrameType::ClientSync, 0, DataType::Null, std::monostate{} });

    topicDirty_ = false;
}

void DanWebSocketClient::sendFrame(const Frame& frame) {
    sendRaw(encode(frame));
}

void DanWebSocketClient::sendRaw(const std::vector<uint8_t>& data) {
    if (ws_ && ws_->isOpen()) {
        ws_->send(data);
    }
}

void DanWebSocketClient::cleanup() {
    heartbeat_.stop();
    bulkQueue_.clear();
    if (ws_) {
        ws_->onOpen = nullptr;
        ws_->onClose = nullptr;
        ws_->onError = nullptr;
        ws_->onMessage = nullptr;
        try { ws_->close(); } catch (...) {}
        ws_.reset();
    }
}

void DanWebSocketClient::log(const std::string& msg) {
    if (debug_) {
        // Simple stderr logging
        fprintf(stderr, "[dan-ws client] %s\n", msg.c_str());
    }
}

} // namespace danws
