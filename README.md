# dan-websocket C++ Client

A C++ client library for the **DanProtocol v3.5** real-time state synchronization protocol. Designed for Unreal Engine, game engines, and native C++ applications.

## Why?

DanProtocol is a lightweight binary protocol that pushes real-time state from a server to connected clients over WebSocket. This C++ library lets native applications and game engines participate in DanProtocol networks alongside TypeScript and Java clients.

## What does it do?

- **Full DanProtocol v3.5 codec**: encode/decode all 16 data types including VarInteger and VarDouble
- **DLE-based framing**: self-synchronizing stream parser handles partial data, byte-stuffing, and heartbeat detection
- **Key registry**: bidirectional keyId-to-path mapping with validation
- **Topic subscriptions**: subscribe to server-defined topics with parameters
- **Heartbeat management**: automatic send/receive with timeout detection
- **Reconnection engine**: exponential backoff with jitter
- **Platform-agnostic**: bring your own WebSocket implementation via `IWebSocket` interface

## Use cases

- Unreal Engine game clients receiving real-time state
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

## Usage

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
        } else if (auto* dbl = std::get_if<double>(&value)) {
            std::cout << key << " = " << *dbl << "\n";
        }
    });

    client.onUpdate([]() {
        std::cout << "Batch update received\n";
    });

    client.connect();

    // Your application loop...

    return 0;
}
```

### 3. Topic subscriptions

```cpp
// Subscribe to a topic
client.subscribe("board", {{"roomId", danws::Payload(std::string("abc"))}});

// Access topic data
auto* handle = client.topic("board");
handle->onReceive([](const std::string& key, const danws::Payload& value) {
    std::cout << "board." << key << " updated\n";
});

// Unsubscribe
client.unsubscribe("board");
```

### 4. Authentication

```cpp
client.onConnect([&client]() {
    client.authorize("my-auth-token");
});
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
    types.h          - DataType, FrameType enums, Frame struct, Payload variant
    error.h          - DanWSError exception
    codec.h          - encode/decode frames
    serializer.h     - serialize/deserialize typed values
    stream_parser.h  - streaming DLE-based parser
  state/
    key_registry.h   - bidirectional keyId <-> path registry
  connection/
    heartbeat_manager.h  - heartbeat send/receive timing
    reconnect_engine.h   - exponential backoff reconnection
    bulk_queue.h         - frame batching queue
  api/
    client.h             - DanWebSocketClient
    topic_client_handle.h - scoped topic data access
  danws.h                - main include (includes everything)
```

## License

MIT
