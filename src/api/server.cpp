#include "danws/api/server.h"
#include "danws/protocol/codec.h"
#include "danws/protocol/serializer.h"

#include <sstream>
#include <iomanip>
#include <regex>

namespace danws {

// --- UUID helpers ---

std::string DanWebSocketServer::bytesToUuid(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 16) return "";
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

// --- Constructor ---

DanWebSocketServer::DanWebSocketServer(
    std::shared_ptr<IWebSocketServer> transport,
    ServerMode mode, long ttlMs)
    : mode_(mode)
    , transport_(std::move(transport))
    , ttlMs_(ttlMs) {

    if (!isTopicMode()) {
        principals_._setOnNewPrincipal([this](PrincipalTX& ptx) {
            bindPrincipalTX(ptx);
        });
    }
}

DanWebSocketServer::~DanWebSocketServer() {
    close();
}

bool DanWebSocketServer::isTopicMode() const {
    return mode_ == ServerMode::SessionTopic ||
           mode_ == ServerMode::SessionPrincipalTopic;
}

void DanWebSocketServer::assertMode(const std::string& expected, const std::string& method) const {
    bool ok = false;
    if (expected == "broadcast") ok = (mode_ == ServerMode::Broadcast);
    else if (expected == "principal") ok = (mode_ == ServerMode::Principal || mode_ == ServerMode::SessionPrincipalTopic);
    if (!ok) {
        throw DanWSError("INVALID_MODE",
            "server." + method + "() is only available in " + expected + " mode.");
    }
}

// --- Broadcast API ---

void DanWebSocketServer::set(const std::string& key, const Payload& value) {
    assertMode("broadcast", "set");
    principals_.principal(BROADCAST_PRINCIPAL).set(key, value);
}

Payload DanWebSocketServer::get(const std::string& key) const {
    if (mode_ != ServerMode::Broadcast) return std::monostate{};
    // const_cast needed because principal() may create
    auto& self = const_cast<DanWebSocketServer&>(*this);
    return self.principals_.principal(BROADCAST_PRINCIPAL).get(key);
}

std::vector<std::string> DanWebSocketServer::keys() const {
    if (mode_ != ServerMode::Broadcast) return {};
    auto& self = const_cast<DanWebSocketServer&>(*this);
    return self.principals_.principal(BROADCAST_PRINCIPAL).keys();
}

void DanWebSocketServer::clear() {
    assertMode("broadcast", "clear");
    principals_.principal(BROADCAST_PRINCIPAL).clear();
}

void DanWebSocketServer::clear(const std::string& key) {
    assertMode("broadcast", "clear");
    principals_.principal(BROADCAST_PRINCIPAL).clear(key);
}

// --- Principal API ---

PrincipalTX& DanWebSocketServer::principal(const std::string& name) {
    if (mode_ != ServerMode::Principal && mode_ != ServerMode::SessionPrincipalTopic) {
        throw DanWSError("INVALID_MODE",
            "server.principal() is only available in principal/session_principal_topic mode.");
    }
    return principals_.principal(name);
}

// --- Auth ---

void DanWebSocketServer::enableAuthorization(bool enabled, long timeoutMs) {
    authEnabled_ = enabled;
    authTimeoutMs_ = timeoutMs;
}

void DanWebSocketServer::authorize(const std::string& clientUuid,
                                    const std::string& /*token*/,
                                    const std::string& principal) {
    if (principal.empty()) {
        throw DanWSError("INVALID_PRINCIPAL",
            "authorize(): principal must not be empty. Call reject() to deny.");
    }

    auto it = tmpSessions_.find(clientUuid);
    if (it == tmpSessions_.end()) return;

    auto internal = it->second;
    tmpSessions_.erase(it);

    internal->session->_authorize(principal);

    sendFrame(*internal, { FrameType::AuthOk, 0, DataType::Null, std::monostate{} });

    sessions_[clientUuid] = internal;
    activateSession(internal, principal);
}

void DanWebSocketServer::reject(const std::string& clientUuid, const std::string& reason) {
    auto it = tmpSessions_.find(clientUuid);
    if (it == tmpSessions_.end()) return;

    auto internal = it->second;
    tmpSessions_.erase(it);

    sendFrame(*internal, { FrameType::AuthFail, 0, DataType::String, Payload(reason) });

    if (internal->ws) {
        // Small delay not practical without async; close immediately
        try { internal->ws->close(); } catch (...) {}
    }
}

// --- Limits ---

void DanWebSocketServer::setMaxConnections(int max) { maxConnections_ = max; }
void DanWebSocketServer::setMaxFramesPerSec(int max) { maxFramesPerSec_ = max; }

Metrics DanWebSocketServer::metrics() const {
    Metrics m;
    m.activeSessions = static_cast<int>(sessions_.size());
    m.pendingSessions = static_cast<int>(tmpSessions_.size());
    m.principalCount = static_cast<int>(principals_.size());
    m.framesIn = framesIn_;
    m.framesOut = framesOut_;
    return m;
}

// --- Session access ---

DanWebSocketSession* DanWebSocketServer::getSession(const std::string& uuid) {
    auto it = sessions_.find(uuid);
    if (it != sessions_.end()) return it->second->session.get();
    return nullptr;
}

bool DanWebSocketServer::isConnected(const std::string& uuid) const {
    auto it = sessions_.find(uuid);
    if (it != sessions_.end()) return it->second->session->connected();
    return false;
}

// --- Events ---

void DanWebSocketServer::onConnection(std::function<void(DanWebSocketSession&)> cb) {
    onConnection_.push_back(std::move(cb));
}

void DanWebSocketServer::onAuthorize(std::function<void(const std::string&, const std::string&)> cb) {
    onAuthorize_.push_back(std::move(cb));
}

// --- Lifecycle ---

void DanWebSocketServer::start(int port, const std::string& path) {
    transport_->onConnection([this](std::shared_ptr<IWebSocketConnection> ws) {
        handleConnection(std::move(ws));
    });
    transport_->start(port, path);
}

void DanWebSocketServer::close() {
    // Collect all sessions before iterating, since closing connections
    // can trigger callbacks that modify sessions_/tmpSessions_
    std::vector<std::shared_ptr<InternalSession>> allSessions;
    for (auto& [uuid, internal] : sessions_) {
        allSessions.push_back(internal);
    }
    for (auto& [uuid, internal] : tmpSessions_) {
        allSessions.push_back(internal);
    }

    // Clear maps first to prevent re-entrant modification
    sessions_.clear();
    tmpSessions_.clear();
    principalIndex_.clear();

    // Now safely clean up each session
    for (auto& internal : allSessions) {
        internal->session->_disposeAllTopicHandles();
        internal->session->_handleDisconnect();
        internal->heartbeat.stop();
        internal->bulkQueue.clear();
        if (internal->ws) {
            // Clear callbacks to prevent re-entrant calls during close
            internal->ws->onMessage(nullptr);
            internal->ws->onClose(nullptr);
            try { internal->ws->close(); } catch (...) {}
            internal->ws = nullptr;
        }
    }

    transport_->stop();
}

// --- Internal: Principal binding ---

void DanWebSocketServer::bindPrincipalTX(PrincipalTX& ptx) {
    ptx._onValue([this, &ptx](const Frame& frame) {
        auto indexIt = principalIndex_.find(ptx.name());
        if (indexIt == principalIndex_.end()) return;
        for (auto* internal : indexIt->second) {
            if (internal->session->state() == SessionState::Ready &&
                internal->ws && internal->ws->isOpen()) {
                internal->bulkQueue.enqueue(frame);
            }
        }
    });

    ptx._onResync([this, &ptx]() {
        auto keyFrames = ptx._buildKeyFrames();
        auto indexIt = principalIndex_.find(ptx.name());
        if (indexIt == principalIndex_.end()) return;
        for (auto* internal : indexIt->second) {
            if (internal->session->connected() &&
                internal->ws && internal->ws->isOpen()) {
                internal->bulkQueue.enqueue({
                    FrameType::ServerReset, 0, DataType::Null, std::monostate{}
                });
                for (const auto& f : keyFrames) {
                    internal->bulkQueue.enqueue(f);
                }
            }
        }
    });
}

// --- Internal: Connection handling ---

void DanWebSocketServer::handleConnection(std::shared_ptr<IWebSocketConnection> ws) {
    auto parser = std::make_shared<StreamParser>();
    auto identified = std::make_shared<bool>(false);
    auto clientUuidPtr = std::make_shared<std::string>();

    // Prevent captures from preventing cleanup - use weak_ptr for ws
    auto wsWeak = std::weak_ptr<IWebSocketConnection>(ws);

    parser->onFrame([this, identified, clientUuidPtr, wsWeak](const Frame& frame) {
        framesIn_++;

        // Rate limiting
        if (maxFramesPerSec_ > 0 && !clientUuidPtr->empty()) {
            auto it = sessions_.find(*clientUuidPtr);
            auto tmpIt = tmpSessions_.find(*clientUuidPtr);
            InternalSession* internal = nullptr;
            if (it != sessions_.end()) internal = it->second.get();
            else if (tmpIt != tmpSessions_.end()) internal = tmpIt->second.get();

            if (internal) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - internal->windowStart).count();
                if (elapsed >= 1000) {
                    internal->frameCount = 0;
                    internal->windowStart = now;
                }
                if (++internal->frameCount > maxFramesPerSec_) {
                    auto ws = wsWeak.lock();
                    if (ws) ws->close();
                    return;
                }
            }
        }

        if (!*identified) {
            if (frame.frameType != FrameType::Identify) {
                auto ws = wsWeak.lock();
                if (ws) ws->close();
                return;
            }

            auto* binPayload = std::get_if<std::vector<uint8_t>>(&frame.payload);
            if (!binPayload || (binPayload->size() != 16 && binPayload->size() != 18)) {
                auto ws = wsWeak.lock();
                if (ws) ws->close();
                return;
            }

            // Check protocol version if 18 bytes
            if (binPayload->size() == 18) {
                uint8_t clientMajor = (*binPayload)[16];
                if (clientMajor != PROTOCOL_MAJOR) {
                    auto ws = wsWeak.lock();
                    if (ws) ws->close();
                    return;
                }
            }

            *clientUuidPtr = bytesToUuid(*binPayload);
            *identified = true;

            auto ws = wsWeak.lock();
            if (ws) handleIdentified(ws, *clientUuidPtr);
            return;
        }

        // Auth frame
        if (frame.frameType == FrameType::Auth) {
            auto tmpIt = tmpSessions_.find(*clientUuidPtr);
            if (tmpIt != tmpSessions_.end() && authEnabled_) {
                auto* token = std::get_if<std::string>(&frame.payload);
                std::string tokenStr = token ? *token : "";
                for (const auto& cb : onAuthorize_) {
                    try { cb(*clientUuidPtr, tokenStr); } catch (...) {}
                }
            }
            return;
        }

        // Topic frames
        if (isTopicMode()) {
            auto sessIt = sessions_.find(*clientUuidPtr);
            if (sessIt != sessions_.end()) {
                if (frame.frameType == FrameType::ClientReset ||
                    frame.frameType == FrameType::ClientKeyRegistration ||
                    frame.frameType == FrameType::ClientValue ||
                    frame.frameType == FrameType::ClientSync) {
                    handleClientTopicFrame(sessIt->second, frame);
                    return;
                }
            }
        }

        // Route to session handler
        auto sessIt = sessions_.find(*clientUuidPtr);
        if (sessIt != sessions_.end()) {
            sessIt->second->session->_handleFrame(frame);
        }
    });

    parser->onHeartbeat([this, clientUuidPtr]() {
        auto it = sessions_.find(*clientUuidPtr);
        if (it != sessions_.end()) {
            it->second->heartbeat.received();
        } else {
            auto tmpIt = tmpSessions_.find(*clientUuidPtr);
            if (tmpIt != tmpSessions_.end()) {
                tmpIt->second->heartbeat.received();
            }
        }
    });

    ws->onMessage([parser](const std::vector<uint8_t>& data) {
        parser->feed(data);
    });

    ws->onClose([this, clientUuidPtr]() {
        if (!clientUuidPtr->empty()) {
            handleSessionDisconnect(*clientUuidPtr);
        }
    });
}

void DanWebSocketServer::handleIdentified(
    std::shared_ptr<IWebSocketConnection> ws,
    const std::string& clientUuid) {

    // Max connections check
    int total = static_cast<int>(sessions_.size() + tmpSessions_.size());
    if (maxConnections_ > 0 && sessions_.find(clientUuid) == sessions_.end() &&
        total >= maxConnections_) {
        ws->close();
        return;
    }

    // Check for reconnecting session
    auto existIt = sessions_.find(clientUuid);
    if (existIt != sessions_.end()) {
        auto& existing = existIt->second;
        if (existing->ws && existing->ws->isOpen()) {
            try { existing->ws->close(); } catch (...) {}
        }
        existing->ws = ws;
        existing->session->_handleReconnect();
        existing->heartbeat.start();
        existing->bulkQueue.onFlush([ws](const std::vector<uint8_t>& data) {
            if (ws->isOpen()) ws->send(data);
        });

        if (authEnabled_) {
            tmpSessions_[clientUuid] = existing;
            sessions_.erase(existIt);
        } else {
            std::string p = existing->session->principal();
            if (p.empty()) p = (mode_ == ServerMode::Broadcast) ? BROADCAST_PRINCIPAL : "default";
            existing->session->_authorize(p);
            activateSession(existing, p);
        }
        return;
    }

    // New session
    auto session = std::make_shared<DanWebSocketSession>(clientUuid);
    auto internal = std::make_shared<InternalSession>();
    internal->session = session;
    internal->ws = ws;
    internal->windowStart = std::chrono::steady_clock::now();

    session->_setEnqueue([internal](const Frame& f) {
        internal->bulkQueue.enqueue(f);
    });

    internal->bulkQueue.onFlush([wsWeak = std::weak_ptr<IWebSocketConnection>(ws)]
                                (const std::vector<uint8_t>& data) {
        auto ws = wsWeak.lock();
        if (ws && ws->isOpen()) ws->send(data);
    });

    internal->heartbeat.onSend([wsWeak = std::weak_ptr<IWebSocketConnection>(ws)]
                               (const std::vector<uint8_t>& data) {
        auto ws = wsWeak.lock();
        if (ws && ws->isOpen()) ws->send(data);
    });
    internal->heartbeat.onTimeout([this, clientUuid]() {
        handleSessionDisconnect(clientUuid);
    });
    internal->heartbeat.start();

    if (authEnabled_) {
        tmpSessions_[clientUuid] = internal;
    } else {
        std::string defaultPrincipal =
            (mode_ == ServerMode::Broadcast) ? BROADCAST_PRINCIPAL : "default";
        session->_authorize(defaultPrincipal);
        sessions_[clientUuid] = internal;
        activateSession(internal, defaultPrincipal);
    }
}

void DanWebSocketServer::activateSession(
    std::shared_ptr<InternalSession> internal,
    const std::string& principal) {

    if (isTopicMode()) {
        internal->session->_bindSessionTX([internal](const Frame& f) {
            internal->bulkQueue.enqueue(f);
        });
        for (const auto& cb : onConnection_) {
            try { cb(*internal->session); } catch (...) {}
        }
        internal->bulkQueue.enqueue({
            FrameType::ServerSync, 0, DataType::Null, std::monostate{}
        });
    } else {
        std::string effectivePrincipal =
            (mode_ == ServerMode::Broadcast) ? BROADCAST_PRINCIPAL : principal;
        auto& ptx = principals_.principal(effectivePrincipal);
        principals_._addSession(effectivePrincipal);
        indexAddSession(effectivePrincipal, internal.get());

        internal->session->_setTxProviders(
            [&ptx]() { return ptx._buildKeyFrames(); },
            [&ptx]() { return ptx._buildValueFrames(); }
        );

        for (const auto& cb : onConnection_) {
            try { cb(*internal->session); } catch (...) {}
        }
        internal->session->_startSync();
    }
}

// --- Internal: Session disconnect ---

void DanWebSocketServer::handleSessionDisconnect(const std::string& uuid) {
    auto it = sessions_.find(uuid);
    if (it == sessions_.end()) {
        auto tmpIt = tmpSessions_.find(uuid);
        if (tmpIt != tmpSessions_.end()) {
            tmpIt->second->heartbeat.stop();
            tmpSessions_.erase(tmpIt);
        }
        return;
    }

    auto internal = it->second;  // copy shared_ptr to keep alive
    if (!internal->session->connected()) return;

    internal->session->_disposeAllTopicHandles();
    internal->session->_handleDisconnect();
    internal->heartbeat.stop();
    internal->bulkQueue.clear();
    internal->ws = nullptr;

    std::string p = internal->session->principal();
    std::string effectivePrincipal =
        (mode_ == ServerMode::Broadcast) ? BROADCAST_PRINCIPAL : p;

    // Remove from principal index before erasing from sessions
    if (!p.empty() && !isTopicMode()) {
        indexRemoveSession(effectivePrincipal, internal.get());
        principals_._removeSession(effectivePrincipal);
    }

    sessions_.erase(it);
}

// --- Internal: Topic frames ---

void DanWebSocketServer::handleClientTopicFrame(
    std::shared_ptr<InternalSession> internal, const Frame& frame) {

    switch (frame.frameType) {
        case FrameType::ClientReset:
            internal->clientRegistry = std::make_unique<KeyRegistry>();
            internal->clientValues.clear();
            break;

        case FrameType::ClientKeyRegistration: {
            if (!internal->clientRegistry)
                internal->clientRegistry = std::make_unique<KeyRegistry>();
            auto* pathPtr = std::get_if<std::string>(&frame.payload);
            if (pathPtr) {
                internal->clientRegistry->registerOne(frame.keyId, *pathPtr, frame.dataType);
            }
            break;
        }

        case FrameType::ClientValue:
            internal->clientValues[frame.keyId] = frame.payload;
            break;

        case FrameType::ClientSync:
            processTopicSync(internal);
            break;

        default:
            break;
    }
}

void DanWebSocketServer::processTopicSync(std::shared_ptr<InternalSession> internal) {
    auto& session = *internal->session;

    // Parse topics from client registry
    std::map<std::string, std::map<std::string, Payload>> newTopics;
    std::map<std::string, int> nameToIndex;

    if (internal->clientRegistry) {
        std::map<std::string, std::string> indexToName;

        for (const auto& path : internal->clientRegistry->paths()) {
            auto* entry = internal->clientRegistry->getByPath(path);
            if (!entry) continue;

            // "topic.<idx>.name"
            if (path.find("topic.") == 0 && path.size() > 6) {
                auto namePos = path.rfind(".name");
                if (namePos != std::string::npos && namePos == path.size() - 5) {
                    std::string idxStr = path.substr(6, namePos - 6);
                    auto valIt = internal->clientValues.find(entry->keyId);
                    if (valIt != internal->clientValues.end()) {
                        auto* topicName = std::get_if<std::string>(&valIt->second);
                        if (topicName && !topicName->empty()) {
                            indexToName[idxStr] = *topicName;
                            nameToIndex[*topicName] = std::stoi(idxStr);
                            if (newTopics.find(*topicName) == newTopics.end()) {
                                newTopics[*topicName] = {};
                            }
                        }
                    }
                    continue;
                }
            }

            // "topic.<idx>.param.<key>"
            if (path.find("topic.") == 0) {
                auto paramPos = path.find(".param.");
                if (paramPos != std::string::npos) {
                    std::string idxStr = path.substr(6, paramPos - 6);
                    std::string paramKey = path.substr(paramPos + 7);
                    auto nameIt = indexToName.find(idxStr);
                    if (nameIt != indexToName.end()) {
                        auto valIt = internal->clientValues.find(entry->keyId);
                        if (valIt != internal->clientValues.end()) {
                            newTopics[nameIt->second][paramKey] = valIt->second;
                        }
                    }
                }
            }
        }
    }

    // Diff: unsubscribed
    auto oldTopics = session.topics();
    for (const auto& oldName : oldTopics) {
        if (newTopics.find(oldName) == newTopics.end()) {
            auto* handle = session.getTopicHandle(oldName);
            if (handle) {
                for (const auto& cb : topicNamespace_.onUnsubscribeCbs) {
                    try { cb(session, *handle); } catch (...) {}
                }
            }
            session._removeTopicHandle(oldName);
        }
    }

    // Diff: new / changed
    for (const auto& [name, params] : newTopics) {
        auto* existingHandle = session.getTopicHandle(name);
        if (!existingHandle) {
            // New subscription
            auto idxIt = nameToIndex.find(name);
            int clientIdx = (idxIt != nameToIndex.end()) ? idxIt->second : session._nextTopicIndex();
            auto& handle = session._createTopicHandle(name, params, clientIdx);
            for (const auto& cb : topicNamespace_.onSubscribeCbs) {
                try { cb(session, handle); } catch (...) {}
            }
        }
        // Params change detection omitted for simplicity in this implementation
    }
}

// --- Internal: Index management ---

void DanWebSocketServer::indexAddSession(const std::string& principal, InternalSession* internal) {
    principalIndex_[principal].insert(internal);
}

void DanWebSocketServer::indexRemoveSession(const std::string& principal, InternalSession* internal) {
    auto it = principalIndex_.find(principal);
    if (it != principalIndex_.end()) {
        it->second.erase(internal);
        if (it->second.empty()) principalIndex_.erase(it);
    }
}

void DanWebSocketServer::sendFrame(InternalSession& internal, const Frame& frame) {
    if (internal.ws && internal.ws->isOpen()) {
        framesOut_++;
        internal.ws->send(encode(frame));
    }
}

} // namespace danws
