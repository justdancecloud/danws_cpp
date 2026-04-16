#include "danws/protocol/stream_parser.h"
#include "danws/protocol/codec.h"
#include "danws/protocol/types.h"

#include <cassert>
#include <iostream>
#include <vector>
#include <string>

using namespace danws;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct TestReg_##name { \
        TestReg_##name() { \
            std::cout << "  " #name "... "; \
            try { test_##name(); tests_passed++; std::cout << "OK\n"; } \
            catch (const std::exception& e) { tests_failed++; std::cout << "FAIL: " << e.what() << "\n"; } \
        } \
    } test_reg_##name; \
    static void test_##name()

TEST(single_frame) {
    StreamParser parser;
    std::vector<Frame> received;
    parser.onFrame([&](const Frame& f) { received.push_back(f); });

    Frame f{ FrameType::ServerValue, 1, DataType::Bool, Payload(true) };
    auto data = encode(f);
    parser.feed(data);

    assert(received.size() == 1);
    assert(received[0].frameType == FrameType::ServerValue);
    assert(std::get<bool>(received[0].payload) == true);
}

TEST(multiple_frames) {
    StreamParser parser;
    std::vector<Frame> received;
    parser.onFrame([&](const Frame& f) { received.push_back(f); });

    std::vector<Frame> frames = {
        { FrameType::ServerValue, 1, DataType::Bool, Payload(true) },
        { FrameType::ServerValue, 2, DataType::String, Payload(std::string("hello")) },
        { FrameType::ServerSync, 0, DataType::Null, std::monostate{} },
    };
    auto data = encodeBatch(frames);
    parser.feed(data);

    assert(received.size() == 3);
    assert(std::get<bool>(received[0].payload) == true);
    assert(std::get<std::string>(received[1].payload) == "hello");
    assert(received[2].frameType == FrameType::ServerSync);
}

TEST(partial_feed) {
    StreamParser parser;
    std::vector<Frame> received;
    parser.onFrame([&](const Frame& f) { received.push_back(f); });

    Frame f{ FrameType::ServerValue, 1, DataType::String, Payload(std::string("test")) };
    auto data = encode(f);

    // Feed byte by byte
    for (size_t i = 0; i < data.size(); ++i) {
        parser.feed(&data[i], 1);
    }

    assert(received.size() == 1);
    assert(std::get<std::string>(received[0].payload) == "test");
}

TEST(partial_feed_split) {
    StreamParser parser;
    std::vector<Frame> received;
    parser.onFrame([&](const Frame& f) { received.push_back(f); });

    Frame f{ FrameType::ServerValue, 1, DataType::Int32, Payload(int32_t(12345)) };
    auto data = encode(f);

    // Feed in two halves
    size_t mid = data.size() / 2;
    parser.feed(data.data(), mid);
    assert(received.empty()); // Frame not complete yet

    parser.feed(data.data() + mid, data.size() - mid);
    assert(received.size() == 1);
    assert(std::get<int32_t>(received[0].payload) == 12345);
}

TEST(heartbeat_detection) {
    StreamParser parser;
    int heartbeatCount = 0;
    parser.onHeartbeat([&]() { heartbeatCount++; });

    auto hb = encodeHeartbeat();
    parser.feed(hb);

    assert(heartbeatCount == 1);
}

TEST(heartbeat_between_frames) {
    StreamParser parser;
    std::vector<Frame> received;
    int heartbeatCount = 0;
    parser.onFrame([&](const Frame& f) { received.push_back(f); });
    parser.onHeartbeat([&]() { heartbeatCount++; });

    Frame f1{ FrameType::ServerValue, 1, DataType::Bool, Payload(true) };
    Frame f2{ FrameType::ServerValue, 2, DataType::Bool, Payload(false) };

    auto d1 = encode(f1);
    auto hb = encodeHeartbeat();
    auto d2 = encode(f2);

    // Concatenate: frame1 + heartbeat + frame2
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), d1.begin(), d1.end());
    combined.insert(combined.end(), hb.begin(), hb.end());
    combined.insert(combined.end(), d2.begin(), d2.end());

    parser.feed(combined);

    assert(received.size() == 2);
    assert(heartbeatCount == 1);
}

TEST(dle_escape_handling) {
    StreamParser parser;
    std::vector<Frame> received;
    parser.onFrame([&](const Frame& f) { received.push_back(f); });

    // KeyID containing DLE byte (0x10)
    Frame f{ FrameType::ServerValue, 0x10, DataType::Bool, Payload(true) };
    auto data = encode(f);
    parser.feed(data);

    assert(received.size() == 1);
    assert(received[0].keyId == 0x10);
    assert(std::get<bool>(received[0].payload) == true);
}

TEST(reset_clears_state) {
    StreamParser parser;
    std::vector<Frame> received;
    parser.onFrame([&](const Frame& f) { received.push_back(f); });

    Frame f{ FrameType::ServerValue, 1, DataType::String, Payload(std::string("test")) };
    auto data = encode(f);

    // Feed partial data
    parser.feed(data.data(), data.size() / 2);
    assert(received.empty());

    // Reset mid-parse
    parser.reset();

    // Feed a complete new frame
    Frame f2{ FrameType::ServerSync, 0, DataType::Null, std::monostate{} };
    auto data2 = encode(f2);
    parser.feed(data2);

    assert(received.size() == 1);
    assert(received[0].frameType == FrameType::ServerSync);
}

TEST(error_on_invalid_dle) {
    StreamParser parser;
    std::vector<std::string> errors;
    parser.onError([&](const std::string& msg) { errors.push_back(msg); });

    // DLE followed by invalid byte
    uint8_t invalid[] = { 0x10, 0x99 };
    parser.feed(invalid, 2);

    assert(errors.size() == 1);
}

int main() {
    std::cout << "StreamParser tests:\n";
    // Tests run via static initialization

    std::cout << "\n" << tests_passed << " passed, " << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
