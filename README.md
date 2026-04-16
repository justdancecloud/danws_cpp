# dan-websocket C++

A C++ client and server library for the **DanProtocol v3.5** real-time state synchronization protocol. Designed for Unreal Engine, game engines, and native C++ applications.

## Why?

DanProtocol is a lightweight binary protocol that pushes real-time state over WebSocket. This C++ library lets native applications participate in DanProtocol networks alongside TypeScript and Java clients/servers.

## What does it do?

- **Full DanProtocol v3.5 codec**: encode/decode all 16 data types including VarInteger and VarDouble
- **DLE-based framing**: self-synchronizing stream parser handles partial data, byte-stuffing, and heartbeat detection
- **Key registry**: bidirectional keyId-to-path mapping with validation
- **Client**: full-featured client with topic subscriptions, auth, reconnection with exponential backoff
- **Server**: 4 server modes (Broadcast, Principal, SessionTopic, SessionPrincipalTopic)
- **Principal isolation**: each principal gets its own independent key-value namespace
- **Topic system**: per-session topic subscriptions with scoped payload stores
- **Auth flow**: optional authorization with accept/reject per-client
- **Rate limiting**: configurable max connections and max frames per second
- **Platform-agnostic**: bring your own WebSocket implementation via `IWebSocket`/`IWebSocketServer` interfaces
- **Heartbeat management**: automatic send/receive with timeout detection
- **Reconnection engine**: exponential backoff with jitter (client-side)

## Use cases

- Unreal Engine game clients receiving real-time state
- Embedded C++ game servers pushing state to connected clients
- Native C++ applications monitoring dashboards
- IoT devices with C++ runtime
- Any C++ application needing real-time state sync with a DanProtocol server

## Requirements

- C++17 or later
- CMake 3.16+
- A C++ compiler (GCC 7+, Clang 5+, MSVC 2017+)
- Threading support (pthreads on Linux/macOS, native on Windows)

## Build

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

Run tests:

```bash
cd build
ctest --output-on-failure
```

## Integration

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    danwebsocket
    GIT_REPOSITORY https://github.com/justdancecloud/danws_cpp.git
    GIT_TAG main
)
FetchContent_MakeAvailable(danwebsocket)

target_link_libraries(your_target PRIVATE danwebsocket)
```

### Manual

Copy the `include/danws/` and `src/` directories into your project and add them to your build system.

## Client Usage

### 1. Implement the WebSocket interface

The library does not bundle a WebSocket implementation. You provide one by implementing `danws::IWebSocket`:

```cpp
#include <danws/danws.h>

class MyWebSocket : public danws::IWebSocket {
public:
    void connect(const std::string& url) override {
        // Connect using your preferred WebSocket library
        // (Boost.Beast, IXWebSocket, libwebsockets, etc.)
        // Call onOpen() when connected
        // Call onMessage(data) when binary data arrives
        // Call onClose() when disconnected
    }
    void send(const std::vector<uint8_t>& data) override { /* ... */ }
    void close() override { /* ... */ }
    bool isOpen() const override { /* ... */ }
};
```

### 2. Create a client and connect

```cpp
#include <danws/danws.h>

int main() {
    auto factory = []() -> std::unique_ptr<danws::IWebSocket> {
        return std::make_unique<MyWebSocket>();
    };

    danws::DanWebSocketClient client("ws://localhost:8080", factory);

    client.onReady([]() {
        std::cout << "Connected and ready!\n";
    });

    client.onReceive([](const std::string& key, const danws::Payload& value) {
        if (auto* str = std::get_if<std::string>(&value)) {
            std::cout << key << " = " << *str << "\n";
        } else if (auto* num = std::get_if<int32_t>(&value)) {
            std::cout << key << " = " << *num << "\n";
        }
    });

    client.connect();
    // Your application loop...
    return 0;
}
```

### 3. Topic subscriptions (client)

```cpp
client.subscribe("board", {{"roomId", danws::Payload(std::string("abc"))}});

auto* handle = client.topic("board");
handle->onReceive([](const std::string& key, const danws::Payload& value) {
    std::cout << "board." << key << " updated\n";
});

client.unsubscribe("board");
```

### 4. Authentication (client)

```cpp
client.onConnect([&client]() {
    client.authorize("my-auth-token");
});
```

## Server Usage

### 1. Implement the server transport

```cpp
#include <danws/danws.h>

class MyServerConnection : public danws::IWebSocketConnection {
public:
    void send(const std::vector<uint8_t>& data) override { /* ... */ }
    void close() override { /* ... */ }
    bool isOpen() const override { /* ... */ }
    void onMessage(std::function<void(const std::vector<uint8_t>&)> cb) override { /* ... */ }
    void onClose(std::function<void()> cb) override { /* ... */ }
};

class MyWebSocketServer : public danws::IWebSocketServer {
public:
    void start(int port, const std::string& path) override {
        // Start listening, call onConnection callback for each new client
    }
    void stop() override { /* ... */ }
    void onConnection(std::function<void(std::shared_ptr<danws::IWebSocketConnection>)> cb) override {
        // Store the callback, invoke it when a new client connects
    }
};
```

### 2. Broadcast mode

All clients see the same state. Simplest mode.

```cpp
auto transport = std::make_shared<MyWebSocketServer>();
danws::DanWebSocketServer server(transport, danws::ServerMode::Broadcast);

server.onConnection([](danws::DanWebSocketSession& session) {
    std::cout << "Client connected: " << session.id() << "\n";
});

server.start(8080, "/");

// Set values — automatically broadcast to all connected clients
server.set("score", danws::Payload(int32_t(42)));
server.set("name", danws::Payload(std::string("Game Room")));

// Read back
auto val = server.get("score");  // Payload(42)
auto keys = server.keys();       // ["score", "name"]

// Clear
server.clear("score");  // remove one key
server.clear();         // remove all keys
```

### 3. Principal mode

Each principal has isolated state. Clients are assigned to a principal during auth.

```cpp
auto transport = std::make_shared<MyWebSocketServer>();
danws::DanWebSocketServer server(transport, danws::ServerMode::Principal);
server.enableAuthorization(true, 5000);

server.onAuthorize([&server](const std::string& clientUuid, const std::string& token) {
    if (token == "valid-token") {
        server.authorize(clientUuid, token, "player1");
    } else {
        server.reject(clientUuid, "Invalid token");
    }
});

server.start(8080, "/");

// Set per-principal state
server.principal("player1").set("hp", danws::Payload(int32_t(100)));
server.principal("player2").set("hp", danws::Payload(int32_t(80)));

// Each principal's data is isolated — player1 only sees player1's keys
```

### 4. Session Topic mode

Clients subscribe to topics. Each topic gets a scoped payload store.

```cpp
auto transport = std::make_shared<MyWebSocketServer>();
danws::DanWebSocketServer server(transport, danws::ServerMode::SessionTopic);

server.topic().onSubscribe([](danws::DanWebSocketSession& session, danws::TopicHandle& topic) {
    std::cout << "Client " << session.id() << " subscribed to " << topic.name() << "\n";

    // Set topic-scoped data — only this client's subscription sees it
    topic.payload().set("greeting", danws::Payload(std::string("Hello!")));
    topic.payload().set("count", danws::Payload(int32_t(0)));

    // Optional: periodic task
    topic.setDelayedTask(1000);  // fires every 1 second

    // Set callback for all events
    topic.setCallback([](danws::TopicEventType event, danws::TopicHandle& t, danws::DanWebSocketSession& s) {
        if (event == danws::TopicEventType::DelayedTask) {
            auto count = std::get<int32_t>(t.payload().get("count"));
            t.payload().set("count", danws::Payload(count + 1));
        }
    });
});

server.topic().onUnsubscribe([](danws::DanWebSocketSession& session, danws::TopicHandle& topic) {
    std::cout << "Client " << session.id() << " unsubscribed from " << topic.name() << "\n";
});

server.start(8080, "/");
```

### 5. Rate limiting and metrics

```cpp
server.setMaxConnections(100);    // 0 = unlimited
server.setMaxFramesPerSec(60);    // 0 = unlimited

auto m = server.metrics();
// m.activeSessions, m.pendingSessions, m.principalCount, m.framesIn, m.framesOut
```

## Protocol compatibility

Wire-compatible with:
- [dan-websocket (npm)](https://www.npmjs.com/package/dan-websocket) - TypeScript
- [io.github.justdancecloud:dan-websocket](https://central.sonatype.com/artifact/io.github.justdancecloud/dan-websocket) - Java

See [DanProtocol v3.5 Specification](https://github.com/justdancecloud/danws_typescript/blob/main/dan-protocol.md) for the full protocol specification.

## Architecture

```
include/danws/
  protocol/
    types.h              - DataType, FrameType enums, Frame struct, Payload variant
    error.h              - DanWSError exception
    codec.h              - encode/decode frames
    serializer.h         - serialize/deserialize typed values
    stream_parser.h      - streaming DLE-based parser
  state/
    key_registry.h       - bidirectional keyId <-> path registry
  connection/
    heartbeat_manager.h  - heartbeat send/receive timing
    reconnect_engine.h   - exponential backoff reconnection
    bulk_queue.h         - frame batching queue
  api/
    client.h             - DanWebSocketClient
    topic_client_handle.h - scoped topic data access (client)
    server_transport.h   - IWebSocketServer, IWebSocketConnection interfaces
    server.h             - DanWebSocketServer (4 modes)
    session.h            - DanWebSocketSession
    principal_tx.h       - PrincipalTX, PrincipalManager
    topic_handle.h       - TopicHandle, TopicPayload, TopicNamespace
    flat_state.h         - FlatState (server-side key-value store)
  danws.h                - main include (includes everything)
```

## License

MIT
