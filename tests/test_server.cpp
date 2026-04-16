#include "danws/danws.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <memory>
#include <functional>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>

using namespace danws;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name()

// ============================================================================
// Mock transport implementation
// ============================================================================

class MockConnection : public IWebSocketConnection {
public:
    bool open_ = true;
    std::function<void(const std::vector<uint8_t>&)> onMessage_;
    std::function<void()> onClose_;
    std::vector<std::vector<uint8_t>> sentData_;

    void send(const std::vector<uint8_t>& data) override {
        if (open_) sentData_.push_back(data);
    }

    void close() override {
        if (open_) {
            open_ = false;
            if (onClose_) onClose_();
        }
    }

    bool isOpen() const override { return open_; }

    void onMessage(std::function<void(const std::vector<uint8_t>&)> cb) override {
        onMessage_ = std::move(cb);
    }

    void onClose(std::function<void()> cb) override {
        onClose_ = std::move(cb);
    }

    /// Simulate receiving data from the client
    void injectMessage(const std::vector<uint8_t>& data) {
        if (onMessage_) onMessage_(data);
    }

    /// Simulate client disconnecting
    void simulateClose() {
        close();
    }

    /// Parse all frames sent by the server
    std::vector<Frame> parseSentFrames() {
        std::vector<Frame> all;
        for (const auto& data : sentData_) {
            auto frames = decode(data);
            for (auto& f : frames) all.push_back(std::move(f));
        }
        return all;
    }

    void clearSent() { sentData_.clear(); }
};

class MockServer : public IWebSocketServer {
public:
    std::function<void(std::shared_ptr<IWebSocketConnection>)> onConn_;

    void start(int /*port*/, const std::string& /*path*/) override {}
    void stop() override {}

    void onConnection(std::function<void(std::shared_ptr<IWebSocketConnection>)> cb) override {
        onConn_ = std::move(cb);
    }

    /// Simulate a new client connecting. Returns the mock connection.
    std::shared_ptr<MockConnection> simulateConnection() {
        auto conn = std::make_shared<MockConnection>();
        if (onConn_) onConn_(conn);
        return conn;
    }
};

// ============================================================================
// Helper: generate a UUIDv7 and build an IDENTIFY frame
// ============================================================================

static std::string generateUUIDv7() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    uint8_t bytes[16];
    bytes[0] = static_cast<uint8_t>((ms >> 40) & 0xFF);
    bytes[1] = static_cast<uint8_t>((ms >> 32) & 0xFF);
    bytes[2] = static_cast<uint8_t>((ms >> 24) & 0xFF);
    bytes[3] = static_cast<uint8_t>((ms >> 16) & 0xFF);
    bytes[4] = static_cast<uint8_t>((ms >> 8) & 0xFF);
    bytes[5] = static_cast<uint8_t>(ms & 0xFF);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (int i = 6; i < 16; ++i) bytes[i] = static_cast<uint8_t>(dist(gen));

    bytes[6] = (bytes[6] & 0x0F) | 0x70;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

static std::vector<uint8_t> uuidToBytes(const std::string& uuid) {
    std::string clean;
    for (char c : uuid) {
        if (c != '-') clean += c;
    }
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < clean.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoi(clean.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

static std::vector<uint8_t> buildIdentifyFrame(const std::string& uuid) {
    auto uuidBytes = uuidToBytes(uuid);
    // Append protocol version 3.5
    uuidBytes.push_back(3);
    uuidBytes.push_back(5);

    Frame f;
    f.frameType = FrameType::Identify;
    f.keyId = 0;
    f.dataType = DataType::Binary;
    f.payload = uuidBytes;
    return encode(f);
}

/// Helper: connect a mock client, send identify, return uuid
static std::string connectClient(DanWebSocketServer& server,
                                  std::shared_ptr<MockServer> transport,
                                  std::shared_ptr<MockConnection>& outConn) {
    std::string uuid = generateUUIDv7();
    auto conn = transport->simulateConnection();
    conn->injectMessage(buildIdentifyFrame(uuid));
    outConn = conn;
    return uuid;
}

// ============================================================================
// Tests
// ============================================================================

TEST(broadcast_set_get) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    server.set("score", Payload(int32_t(42)));
    server.set("name", Payload(std::string("hello")));

    auto score = server.get("score");
    assert(std::get<int32_t>(score) == 42);

    auto name = server.get("name");
    assert(std::get<std::string>(name) == "hello");

    auto k = server.keys();
    assert(k.size() == 2);

    server.close();
}

TEST(broadcast_clear_key) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    server.set("a", Payload(int32_t(1)));
    server.set("b", Payload(int32_t(2)));
    server.clear("a");

    assert(server.keys().size() == 1);

    server.close();
}

TEST(broadcast_clear_all) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    server.set("a", Payload(int32_t(1)));
    server.set("b", Payload(int32_t(2)));
    server.clear();

    assert(server.keys().empty());

    server.close();
}

TEST(broadcast_mode_guard) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.start(0);

    bool threw = false;
    try { server.set("x", Payload(int32_t(1))); }
    catch (const DanWSError&) { threw = true; }
    assert(threw);

    server.close();
}

TEST(principal_set_get) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.start(0);

    auto& ptx = server.principal("alice");
    ptx.set("hp", Payload(int32_t(100)));
    assert(std::get<int32_t>(ptx.get("hp")) == 100);

    auto& ptx2 = server.principal("bob");
    ptx2.set("hp", Payload(int32_t(200)));
    assert(std::get<int32_t>(ptx2.get("hp")) == 200);

    // Alice's data unchanged
    assert(std::get<int32_t>(ptx.get("hp")) == 100);

    server.close();
}

TEST(principal_isolation) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.start(0);

    server.principal("alice").set("x", Payload(int32_t(1)));
    server.principal("bob").set("x", Payload(int32_t(2)));

    assert(std::get<int32_t>(server.principal("alice").get("x")) == 1);
    assert(std::get<int32_t>(server.principal("bob").get("x")) == 2);

    server.close();
}

TEST(principal_mode_guard) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    bool threw = false;
    try { server.principal("alice"); }
    catch (const DanWSError&) { threw = true; }
    assert(threw);

    server.close();
}

TEST(auth_flow_authorize) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.enableAuthorization(true, 5000);
    server.start(0);

    std::string receivedUuid, receivedToken;
    server.onAuthorize([&](const std::string& uuid, const std::string& token) {
        receivedUuid = uuid;
        receivedToken = token;
    });

    bool connectionCalled = false;
    server.onConnection([&](DanWebSocketSession& s) {
        connectionCalled = true;
    });

    // Connect
    std::shared_ptr<MockConnection> conn;
    std::string uuid = connectClient(server, transport, conn);

    // Session should be in tmpSessions, not yet activated
    assert(!connectionCalled);

    // Send auth token
    Frame authFrame{ FrameType::Auth, 0, DataType::String, Payload(std::string("my-token")) };
    conn->injectMessage(encode(authFrame));

    assert(receivedUuid == uuid);
    assert(receivedToken == "my-token");

    // Authorize
    server.authorize(uuid, "my-token", "alice");
    assert(connectionCalled);

    // Check session exists
    auto* session = server.getSession(uuid);
    assert(session != nullptr);
    assert(session->principal() == "alice");
    assert(session->authorized());

    server.close();
}

TEST(auth_flow_reject) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.enableAuthorization(true, 5000);
    server.start(0);

    std::shared_ptr<MockConnection> conn;
    std::string uuid = connectClient(server, transport, conn);

    // Send auth token
    Frame authFrame{ FrameType::Auth, 0, DataType::String, Payload(std::string("bad-token")) };
    conn->injectMessage(encode(authFrame));

    // Reject
    server.reject(uuid, "Bad credentials");

    // Connection should have AUTH_FAIL frame and be closed
    auto frames = conn->parseSentFrames();
    bool hasAuthFail = false;
    for (const auto& f : frames) {
        if (f.frameType == FrameType::AuthFail) {
            hasAuthFail = true;
            auto* reason = std::get_if<std::string>(&f.payload);
            assert(reason && *reason == "Bad credentials");
        }
    }
    assert(hasAuthFail);
    assert(!conn->isOpen());

    server.close();
}

TEST(auth_authorize_null_throws) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.enableAuthorization(true);
    server.start(0);

    bool threw = false;
    try { server.authorize("some-uuid", "token", ""); }
    catch (const DanWSError& e) {
        threw = true;
        assert(std::string(e.code()) == "INVALID_PRINCIPAL");
    }
    assert(threw);

    server.close();
}

TEST(connection_no_auth_gets_default_principal) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.start(0);

    bool connectionCalled = false;
    std::string sessionPrincipal;
    server.onConnection([&](DanWebSocketSession& s) {
        connectionCalled = true;
        sessionPrincipal = s.principal();
    });

    std::shared_ptr<MockConnection> conn;
    std::string uuid = connectClient(server, transport, conn);

    assert(connectionCalled);
    assert(sessionPrincipal == "default");

    server.close();
}

TEST(broadcast_connection_syncs_data) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    // Set data before any client connects
    server.set("score", Payload(int32_t(42)));

    std::shared_ptr<MockConnection> conn;
    std::string uuid = connectClient(server, transport, conn);

    // Check that the server sent key registration + sync + value frames
    auto frames = conn->parseSentFrames();
    bool hasKeyReg = false;
    bool hasSync = false;
    bool hasValue = false;
    for (const auto& f : frames) {
        if (f.frameType == FrameType::ServerKeyRegistration) hasKeyReg = true;
        if (f.frameType == FrameType::ServerSync) hasSync = true;
        if (f.frameType == FrameType::ServerValue) hasValue = true;
    }
    // Key reg + sync should be sent during startSync
    assert(hasKeyReg);
    assert(hasSync);
    // Value is sent after client sends ClientReady (which we didn't send yet)

    server.close();
}

TEST(max_connections_rejects) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.setMaxConnections(1);
    server.start(0);

    // First connection should succeed
    std::shared_ptr<MockConnection> conn1;
    std::string uuid1 = connectClient(server, transport, conn1);
    assert(conn1->isOpen());

    // Second connection should be rejected (closed)
    std::shared_ptr<MockConnection> conn2;
    std::string uuid2 = connectClient(server, transport, conn2);
    assert(!conn2->isOpen());

    server.close();
}

TEST(metrics) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    auto m = server.metrics();
    assert(m.activeSessions == 0);

    std::shared_ptr<MockConnection> conn;
    connectClient(server, transport, conn);

    m = server.metrics();
    assert(m.activeSessions == 1);

    server.close();
}

TEST(identify_rejects_wrong_protocol_version) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    auto conn = transport->simulateConnection();

    // Build identify with wrong protocol major (99)
    auto uuidBytes = uuidToBytes(generateUUIDv7());
    uuidBytes.push_back(99);  // wrong major
    uuidBytes.push_back(0);

    Frame f;
    f.frameType = FrameType::Identify;
    f.keyId = 0;
    f.dataType = DataType::Binary;
    f.payload = uuidBytes;
    conn->injectMessage(encode(f));

    // Should close due to version mismatch
    assert(!conn->isOpen());

    server.close();
}

TEST(non_identify_first_frame_closes) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    auto conn = transport->simulateConnection();

    // Send a non-Identify frame
    Frame f{ FrameType::ClientReady, 0, DataType::Null, std::monostate{} };
    conn->injectMessage(encode(f));

    assert(!conn->isOpen());

    server.close();
}

TEST(session_disconnect) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    std::shared_ptr<MockConnection> conn;
    std::string uuid = connectClient(server, transport, conn);

    assert(server.metrics().activeSessions == 1);

    conn->simulateClose();

    assert(server.metrics().activeSessions == 0);

    server.close();
}

TEST(topic_subscribe) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::SessionTopic);
    server.start(0);

    bool subscribeCalled = false;
    std::string subscribedTopicName;
    server.topic().onSubscribe([&](DanWebSocketSession& s, TopicHandle& th) {
        subscribeCalled = true;
        subscribedTopicName = th.name();
        th.payload().set("greeting", Payload(std::string("hello")));
    });

    std::shared_ptr<MockConnection> conn;
    std::string uuid = connectClient(server, transport, conn);

    // Build client topic sync: subscribe to "board"
    // ClientReset -> ClientKeyRegistration(topic.0.name) -> ClientValue(topic.0.name="board") -> ClientSync
    std::vector<Frame> topicFrames;
    topicFrames.push_back({ FrameType::ClientReset, 0, DataType::Null, std::monostate{} });
    topicFrames.push_back({ FrameType::ClientKeyRegistration, 1, DataType::String,
                           Payload(std::string("topic.0.name")) });
    topicFrames.push_back({ FrameType::ClientValue, 1, DataType::String,
                           Payload(std::string("board")) });
    topicFrames.push_back({ FrameType::ClientSync, 0, DataType::Null, std::monostate{} });

    for (const auto& f : topicFrames) {
        conn->injectMessage(encode(f));
    }

    assert(subscribeCalled);
    assert(subscribedTopicName == "board");

    server.close();
}

TEST(topic_unsubscribe) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::SessionTopic);
    server.start(0);

    bool subscribeCalled = false;
    bool unsubscribeCalled = false;
    server.topic().onSubscribe([&](DanWebSocketSession&, TopicHandle&) {
        subscribeCalled = true;
    });
    server.topic().onUnsubscribe([&](DanWebSocketSession&, TopicHandle&) {
        unsubscribeCalled = true;
    });

    std::shared_ptr<MockConnection> conn;
    std::string uuid = connectClient(server, transport, conn);

    // Subscribe to "board"
    std::vector<Frame> subFrames;
    subFrames.push_back({ FrameType::ClientReset, 0, DataType::Null, std::monostate{} });
    subFrames.push_back({ FrameType::ClientKeyRegistration, 1, DataType::String,
                         Payload(std::string("topic.0.name")) });
    subFrames.push_back({ FrameType::ClientValue, 1, DataType::String,
                         Payload(std::string("board")) });
    subFrames.push_back({ FrameType::ClientSync, 0, DataType::Null, std::monostate{} });
    for (const auto& f : subFrames) conn->injectMessage(encode(f));

    assert(subscribeCalled);

    // Unsubscribe: send empty topic set
    std::vector<Frame> unsubFrames;
    unsubFrames.push_back({ FrameType::ClientReset, 0, DataType::Null, std::monostate{} });
    unsubFrames.push_back({ FrameType::ClientSync, 0, DataType::Null, std::monostate{} });
    for (const auto& f : unsubFrames) conn->injectMessage(encode(f));

    assert(unsubscribeCalled);

    server.close();
}

TEST(principal_build_key_value_frames) {
    PrincipalTX ptx("test");
    ptx.set("a", Payload(int32_t(10)));
    ptx.set("b", Payload(std::string("hello")));

    auto keyFrames = ptx._buildKeyFrames();
    // Should have 2 key regs + 1 ServerSync
    assert(keyFrames.size() == 3);

    int keyRegCount = 0;
    int syncCount = 0;
    for (const auto& f : keyFrames) {
        if (f.frameType == FrameType::ServerKeyRegistration) keyRegCount++;
        if (f.frameType == FrameType::ServerSync) syncCount++;
    }
    assert(keyRegCount == 2);
    assert(syncCount == 1);

    auto valueFrames = ptx._buildValueFrames();
    assert(valueFrames.size() == 2);
    for (const auto& f : valueFrames) {
        assert(f.frameType == FrameType::ServerValue);
    }
}

TEST(flat_state_set_get_keys) {
    uint32_t nextId = 1;
    std::vector<Frame> enqueued;

    FlatState state(FlatState::Callbacks{
        [&nextId]() -> uint32_t { return nextId++; },
        [&enqueued](const Frame& f) { enqueued.push_back(f); },
        []() {},
        ""
    });

    state.set("x", Payload(int32_t(42)));
    assert(std::get<int32_t>(state.get("x")) == 42);
    assert(state.keys().size() == 1);

    state.set("y", Payload(std::string("hi")));
    assert(state.keys().size() == 2);

    // First set of "x" should have produced key+sync+value frames
    assert(enqueued.size() >= 3);
}

TEST(flat_state_clear) {
    uint32_t nextId = 1;
    std::vector<Frame> enqueued;
    int resyncCount = 0;

    FlatState state(FlatState::Callbacks{
        [&nextId]() -> uint32_t { return nextId++; },
        [&enqueued](const Frame& f) { enqueued.push_back(f); },
        [&resyncCount]() { resyncCount++; },
        ""
    });

    state.set("a", Payload(int32_t(1)));
    state.set("b", Payload(int32_t(2)));
    state.clear();

    assert(state.keys().empty());
    assert(resyncCount == 1);
}

TEST(flat_state_clear_single_key) {
    uint32_t nextId = 1;
    std::vector<Frame> enqueued;

    FlatState state(FlatState::Callbacks{
        [&nextId]() -> uint32_t { return nextId++; },
        [&enqueued](const Frame& f) { enqueued.push_back(f); },
        []() {},
        ""
    });

    state.set("a", Payload(int32_t(1)));
    state.set("b", Payload(int32_t(2)));
    state.clear("a");

    assert(state.keys().size() == 1);
    assert(std::holds_alternative<std::monostate>(state.get("a")));
    assert(std::get<int32_t>(state.get("b")) == 2);

    // Should have sent a ServerKeyDelete
    bool hasDelete = false;
    for (const auto& f : enqueued) {
        if (f.frameType == FrameType::ServerKeyDelete) hasDelete = true;
    }
    assert(hasDelete);
}

TEST(flat_state_type_change) {
    uint32_t nextId = 1;
    std::vector<Frame> enqueued;

    FlatState state(FlatState::Callbacks{
        [&nextId]() -> uint32_t { return nextId++; },
        [&enqueued](const Frame& f) { enqueued.push_back(f); },
        []() {},
        ""
    });

    state.set("x", Payload(int32_t(42)));
    enqueued.clear();

    // Change type from int32 to string
    state.set("x", Payload(std::string("hello")));

    // Should see: ServerKeyDelete, ServerKeyRegistration, ServerSync, ServerValue
    assert(enqueued.size() == 4);
    assert(enqueued[0].frameType == FrameType::ServerKeyDelete);
    assert(enqueued[1].frameType == FrameType::ServerKeyRegistration);
    assert(enqueued[2].frameType == FrameType::ServerSync);
    assert(enqueued[3].frameType == FrameType::ServerValue);
}

TEST(flat_state_wire_prefix) {
    uint32_t nextId = 1;
    std::vector<Frame> enqueued;

    FlatState state(FlatState::Callbacks{
        [&nextId]() -> uint32_t { return nextId++; },
        [&enqueued](const Frame& f) { enqueued.push_back(f); },
        []() {},
        "t.0."
    });

    state.set("score", Payload(int32_t(10)));

    // Key registration should have wire path "t.0.score"
    bool foundKeyReg = false;
    for (const auto& f : enqueued) {
        if (f.frameType == FrameType::ServerKeyRegistration) {
            auto* path = std::get_if<std::string>(&f.payload);
            assert(path && *path == "t.0.score");
            foundKeyReg = true;
        }
    }
    assert(foundKeyReg);
}

TEST(topic_payload_set_get) {
    uint32_t nextId = 1;
    TopicPayload tp(0, [&nextId]() -> uint32_t { return nextId++; });

    std::vector<Frame> enqueued;
    tp._bind(
        [&enqueued](const Frame& f) { enqueued.push_back(f); },
        []() {}
    );

    tp.set("score", Payload(int32_t(100)));
    assert(std::get<int32_t>(tp.get("score")) == 100);

    auto kf = tp._buildKeyFrames();
    assert(kf.size() == 1);
    auto* path = std::get_if<std::string>(&kf[0].payload);
    assert(path && *path == "t.0.score");
}

TEST(session_state_transitions) {
    DanWebSocketSession session("test-uuid");
    assert(session.state() == SessionState::Pending);
    assert(session.connected());

    session._authorize("alice");
    assert(session.state() == SessionState::Authorized);
    assert(session.principal() == "alice");

    session._handleDisconnect();
    assert(session.state() == SessionState::Disconnected);
    assert(!session.connected());

    session._handleReconnect();
    assert(session.state() == SessionState::Authorized);
    assert(session.connected());
}

TEST(principal_clear) {
    PrincipalTX ptx("test");
    ptx.set("a", Payload(int32_t(1)));
    ptx.set("b", Payload(int32_t(2)));
    assert(ptx.keys().size() == 2);

    ptx.clear("a");
    assert(ptx.keys().size() == 1);

    ptx.clear();
    assert(ptx.keys().empty());
}

TEST(principal_manager) {
    PrincipalManager pm;

    auto& a = pm.principal("alice");
    a.set("x", Payload(int32_t(1)));
    assert(pm.has("alice"));
    assert(!pm.has("bob"));

    pm._addSession("alice");
    pm._addSession("alice");
    assert(pm._hasActiveSessions("alice"));

    assert(!pm._removeSession("alice"));  // count=1 still
    assert(pm._removeSession("alice"));   // count=0

    pm.remove("alice");
    assert(!pm.has("alice"));
}

TEST(max_connections_with_auth) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Principal);
    server.enableAuthorization(true);
    server.setMaxConnections(1);
    server.start(0);

    // First client
    std::shared_ptr<MockConnection> conn1;
    std::string uuid1 = connectClient(server, transport, conn1);
    assert(conn1->isOpen());

    // Second client should be rejected
    std::shared_ptr<MockConnection> conn2;
    std::string uuid2 = connectClient(server, transport, conn2);
    assert(!conn2->isOpen());

    server.close();
}

TEST(broadcast_update_value) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    server.set("x", Payload(int32_t(1)));
    assert(std::get<int32_t>(server.get("x")) == 1);

    server.set("x", Payload(int32_t(2)));
    assert(std::get<int32_t>(server.get("x")) == 2);

    server.close();
}

TEST(multiple_data_types) {
    auto transport = std::make_shared<MockServer>();
    DanWebSocketServer server(transport, ServerMode::Broadcast);
    server.start(0);

    server.set("bool_val", Payload(true));
    server.set("int_val", Payload(int32_t(-100)));
    server.set("double_val", Payload(3.14));
    server.set("str_val", Payload(std::string("test")));
    server.set("uint_val", Payload(uint32_t(999)));

    assert(std::get<bool>(server.get("bool_val")) == true);
    assert(std::get<int32_t>(server.get("int_val")) == -100);
    assert(std::abs(std::get<double>(server.get("double_val")) - 3.14) < 1e-10);
    assert(std::get<std::string>(server.get("str_val")) == "test");
    assert(std::get<uint32_t>(server.get("uint_val")) == 999);

    server.close();
}

#define RUN(name) \
    std::cerr << "  " #name "... "; \
    try { test_##name(); tests_passed++; std::cerr << "OK\n"; } \
    catch (const std::exception& e) { tests_failed++; std::cerr << "FAIL: " << e.what() << "\n"; }

int main() {
    std::cerr << "Server tests:\n";

    RUN(broadcast_set_get);
    RUN(broadcast_clear_key);
    RUN(broadcast_clear_all);
    RUN(broadcast_mode_guard);
    RUN(principal_set_get);
    RUN(principal_isolation);
    RUN(principal_mode_guard);
    RUN(auth_flow_authorize);
    RUN(auth_flow_reject);
    RUN(auth_authorize_null_throws);
    RUN(connection_no_auth_gets_default_principal);
    RUN(broadcast_connection_syncs_data);
    RUN(max_connections_rejects);
    RUN(metrics);
    RUN(identify_rejects_wrong_protocol_version);
    RUN(non_identify_first_frame_closes);
    RUN(session_disconnect);
    RUN(topic_subscribe);
    RUN(topic_unsubscribe);
    RUN(principal_build_key_value_frames);
    RUN(flat_state_set_get_keys);
    RUN(flat_state_clear);
    RUN(flat_state_clear_single_key);
    RUN(flat_state_type_change);
    RUN(flat_state_wire_prefix);
    RUN(topic_payload_set_get);
    RUN(session_state_transitions);
    RUN(principal_clear);
    RUN(principal_manager);
    RUN(max_connections_with_auth);
    RUN(broadcast_update_value);
    RUN(multiple_data_types);

    std::cerr << "\n" << tests_passed << " passed, " << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
