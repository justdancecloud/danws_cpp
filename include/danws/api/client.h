#pragma once

#include "../protocol/types.h"
#include "../protocol/error.h"
#include "../protocol/stream_parser.h"
#include "../state/key_registry.h"
#include "../connection/heartbeat_manager.h"
#include "../connection/reconnect_engine.h"
#include "../connection/bulk_queue.h"
#include "topic_client_handle.h"

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>

namespace danws {

enum class ClientState {
    Disconnected,
    Connecting,
    Identifying,
    Authorizing,
    Synchronizing,
    Ready,
    Reconnecting,
};

struct ClientOptions {
    ReconnectOptions reconnect;
    bool debug = false;
};

/// Abstract WebSocket interface. Users provide a platform-specific
/// implementation (e.g., Boost.Beast, IXWebSocket, Unreal Engine WebSocket).
class IWebSocket {
public:
    virtual ~IWebSocket() = default;

    /// Connect to the URL. Should call onOpen/onClose/onMessage/onError
    /// via the provided callbacks.
    virtual void connect(const std::string& url) = 0;

    /// Send binary data.
    virtual void send(const std::vector<uint8_t>& data) = 0;

    /// Close the connection.
    virtual void close() = 0;

    /// Check if the connection is open.
    virtual bool isOpen() const = 0;

    // Callbacks set by the client
    std::function<void()> onOpen;
    std::function<void()> onClose;
    std::function<void(const std::vector<uint8_t>&)> onMessage;
    std::function<void(const std::string&)> onError;
};

/// Factory function type for creating WebSocket instances.
using WebSocketFactory = std::function<std::unique_ptr<IWebSocket>()>;

/// DanProtocol v3.5 client.
///
/// Usage:
///   auto client = DanWebSocketClient("ws://localhost:8080", factory);
///   client.onReady([](){ ... });
///   client.onReceive([](const std::string& key, const Payload& val){ ... });
///   client.connect();
class DanWebSocketClient {
public:
    /// Construct with URL and a WebSocket factory.
    DanWebSocketClient(const std::string& url,
                       WebSocketFactory wsFactory,
                       const ClientOptions& options = {});
    ~DanWebSocketClient();

    /// Unique client ID (UUIDv7).
    const std::string& id() const { return id_; }

    /// Current connection state.
    ClientState state() const { return state_; }

    /// Connect to the server.
    void connect();

    /// Intentionally disconnect.
    void disconnect();

    /// Send an auth token.
    void authorize(const std::string& token);

    /// Get the current value for a server-registered key.
    Payload get(const std::string& key) const;

    /// List all server-registered key paths.
    std::vector<std::string> keys() const;

    /// Subscribe to a topic with optional parameters.
    void subscribe(const std::string& topicName,
                   const std::map<std::string, Payload>& params = {});

    /// Unsubscribe from a topic.
    void unsubscribe(const std::string& topicName);

    /// Get a handle for scoped topic data access.
    TopicClientHandle* topic(const std::string& name);

    /// List subscribed topic names.
    std::vector<std::string> topics() const;

    // Event registration (returns unsubscribe function)
    std::function<void()> onConnect(std::function<void()> cb);
    std::function<void()> onDisconnect(std::function<void()> cb);
    std::function<void()> onReady(std::function<void()> cb);
    std::function<void()> onReceive(std::function<void(const std::string&, const Payload&)> cb);
    std::function<void()> onUpdate(std::function<void()> cb);
    std::function<void()> onReconnecting(std::function<void(int, int)> cb);
    std::function<void()> onReconnect(std::function<void()> cb);
    std::function<void()> onReconnectFailed(std::function<void()> cb);
    std::function<void()> onError(std::function<void(const DanWSError&)> cb);

private:
    std::string id_;
    std::string url_;
    ClientState state_ = ClientState::Disconnected;
    bool intentionalDisconnect_ = false;

    WebSocketFactory wsFactory_;
    std::unique_ptr<IWebSocket> ws_;

    KeyRegistry registry_;
    std::unordered_map<uint32_t, Payload> store_;
    std::unordered_map<uint32_t, Frame> pendingValues_;

    // Topic state
    std::map<std::string, std::map<std::string, Payload>> subscriptions_;
    bool topicDirty_ = false;
    std::map<std::string, std::unique_ptr<TopicClientHandle>> topicHandles_;
    std::map<std::string, int> topicIndexMap_;
    std::map<int, std::string> indexToTopic_;

    // Connection layers
    BulkQueue bulkQueue_;
    HeartbeatManager heartbeat_;
    ReconnectEngine reconnectEngine_;
    StreamParser parser_;
    bool debug_ = false;

    // Callbacks
    std::vector<std::function<void()>> onConnect_;
    std::vector<std::function<void()>> onDisconnect_;
    std::vector<std::function<void()>> onReady_;
    std::vector<std::function<void(const std::string&, const Payload&)>> onReceive_;
    std::vector<std::function<void()>> onUpdate_;
    std::vector<std::function<void(int, int)>> onReconnecting_;
    std::vector<std::function<void()>> onReconnect_;
    std::vector<std::function<void()>> onReconnectFailed_;
    std::vector<std::function<void(const DanWSError&)>> onError_;

    void setupInternals();
    void handleOpen();
    void handleClose();
    void handleFrame(const Frame& frame);
    void sendTopicSync();
    void sendFrame(const Frame& frame);
    void sendRaw(const std::vector<uint8_t>& data);
    void cleanup();
    void log(const std::string& msg);

    static std::string generateUUIDv7();

    // Helper to detect data type for a Payload value
    static DataType detectDataType(const Payload& value);

    // Topic key parsing
    struct TopicInfo { int topicIdx; std::string userKey; };
    std::unordered_map<uint32_t, std::optional<TopicInfo>> topicKeyCache_;
    const TopicInfo* getTopicInfo(uint32_t keyId, const std::string& path);

    template<typename Cb, typename... Args>
    void emit(const std::vector<Cb>& callbacks, Args&&... args) {
        for (const auto& cb : callbacks) {
            try { cb(std::forward<Args>(args)...); }
            catch (...) { log("Callback error"); }
        }
    }

    template<typename Cb>
    std::function<void()> addCallback(std::vector<Cb>& vec, Cb cb) {
        vec.push_back(std::move(cb));
        auto* ptr = &vec;
        auto idx = vec.size() - 1;
        return [ptr, idx]() {
            // Simple removal — callbacks are rarely unsubscribed
            if (idx < ptr->size()) {
                ptr->erase(ptr->begin() + static_cast<ptrdiff_t>(idx));
            }
        };
    }
};

} // namespace danws
