#pragma once

#include "../protocol/types.h"
#include "../protocol/error.h"
#include "../protocol/stream_parser.h"
#include "../connection/heartbeat_manager.h"
#include "../connection/bulk_queue.h"
#include "../state/key_registry.h"
#include "server_transport.h"
#include "session.h"
#include "principal_tx.h"
#include "topic_handle.h"

#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <mutex>
#include <atomic>

namespace danws {

enum class ServerMode {
    Broadcast,
    Principal,
    SessionTopic,
    SessionPrincipalTopic,
};

struct Metrics {
    int activeSessions = 0;
    int pendingSessions = 0;
    int principalCount = 0;
    long framesIn = 0;
    long framesOut = 0;
};

class DanWebSocketServer {
public:
    static constexpr int PROTOCOL_MAJOR = 3;
    static constexpr int PROTOCOL_MINOR = 5;

    DanWebSocketServer(std::shared_ptr<IWebSocketServer> transport,
                       ServerMode mode = ServerMode::Principal,
                       long ttlMs = 600000);
    ~DanWebSocketServer();

    ServerMode mode() const { return mode_; }

    // Broadcast mode API
    void set(const std::string& key, const Payload& value);
    Payload get(const std::string& key) const;
    std::vector<std::string> keys() const;
    void clear();
    void clear(const std::string& key);

    // Principal mode API
    PrincipalTX& principal(const std::string& name);

    // Auth
    void enableAuthorization(bool enabled, long timeoutMs = 5000);
    void authorize(const std::string& clientUuid, const std::string& token,
                   const std::string& principal);
    void reject(const std::string& clientUuid, const std::string& reason = "Rejected");

    // Limits
    void setMaxConnections(int max);
    void setMaxFramesPerSec(int max);
    Metrics metrics() const;

    // Topic namespace
    TopicNamespace& topic() { return topicNamespace_; }

    // Events
    void onConnection(std::function<void(DanWebSocketSession&)> cb);
    void onAuthorize(std::function<void(const std::string&, const std::string&)> cb);

    // Lifecycle
    void start(int port, const std::string& path = "/");
    void close();

    // Session access
    DanWebSocketSession* getSession(const std::string& uuid);
    bool isConnected(const std::string& uuid) const;

private:
    static constexpr const char* BROADCAST_PRINCIPAL = "__broadcast__";

    ServerMode mode_;
    std::shared_ptr<IWebSocketServer> transport_;
    long ttlMs_;
    bool authEnabled_ = false;
    long authTimeoutMs_ = 5000;
    int maxConnections_ = 0;
    int maxFramesPerSec_ = 0;

    std::atomic<long> framesIn_{0};
    std::atomic<long> framesOut_{0};

    PrincipalManager principals_;
    TopicNamespace topicNamespace_;

    struct InternalSession {
        std::shared_ptr<DanWebSocketSession> session;
        std::shared_ptr<IWebSocketConnection> ws;
        BulkQueue bulkQueue;
        HeartbeatManager heartbeat;
        std::unique_ptr<KeyRegistry> clientRegistry;
        std::unordered_map<uint32_t, Payload> clientValues;
        // Frame rate tracking
        int frameCount = 0;
        std::chrono::steady_clock::time_point windowStart;
    };

    std::unordered_map<std::string, std::shared_ptr<InternalSession>> sessions_;
    std::unordered_map<std::string, std::shared_ptr<InternalSession>> tmpSessions_;
    std::unordered_map<std::string, std::set<InternalSession*>> principalIndex_;

    // Callbacks
    std::vector<std::function<void(DanWebSocketSession&)>> onConnection_;
    std::vector<std::function<void(const std::string&, const std::string&)>> onAuthorize_;

    bool isTopicMode() const;
    void assertMode(const std::string& expected, const std::string& method) const;

    void handleConnection(std::shared_ptr<IWebSocketConnection> ws);
    void handleIdentified(std::shared_ptr<IWebSocketConnection> ws, const std::string& clientUuid);
    void activateSession(std::shared_ptr<InternalSession> internal, const std::string& principal);
    void handleSessionDisconnect(const std::string& uuid);

    void handleClientTopicFrame(std::shared_ptr<InternalSession> internal, const Frame& frame);
    void processTopicSync(std::shared_ptr<InternalSession> internal);

    void bindPrincipalTX(PrincipalTX& ptx);

    void indexAddSession(const std::string& principal, InternalSession* internal);
    void indexRemoveSession(const std::string& principal, InternalSession* internal);

    void sendFrame(InternalSession& internal, const Frame& frame);

    /// Parse 16-byte UUID from binary payload to string.
    static std::string bytesToUuid(const std::vector<uint8_t>& bytes);
};

} // namespace danws
