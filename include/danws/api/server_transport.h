#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace danws {

/// Abstract interface for a single client connection on the server side.
/// Users implement this with their chosen WebSocket library.
class IWebSocketConnection {
public:
    virtual ~IWebSocketConnection() = default;

    /// Send binary data to the client.
    virtual void send(const std::vector<uint8_t>& data) = 0;

    /// Close the connection.
    virtual void close() = 0;

    /// Check if the connection is still open.
    virtual bool isOpen() const = 0;

    /// Register callback for incoming binary messages.
    virtual void onMessage(std::function<void(const std::vector<uint8_t>&)> cb) = 0;

    /// Register callback for connection close.
    virtual void onClose(std::function<void()> cb) = 0;
};

/// Abstract interface for a WebSocket server transport.
/// Users implement this with Boost.Beast, uWebSockets, etc.
class IWebSocketServer {
public:
    virtual ~IWebSocketServer() = default;

    /// Start listening on the given port and path.
    virtual void start(int port, const std::string& path) = 0;

    /// Stop the server and close all connections.
    virtual void stop() = 0;

    /// Register callback for new client connections.
    virtual void onConnection(std::function<void(std::shared_ptr<IWebSocketConnection>)> cb) = 0;
};

} // namespace danws
