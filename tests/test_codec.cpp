#include "danws/protocol/codec.h"
#include "danws/protocol/serializer.h"
#include "danws/protocol/types.h"
#include "danws/protocol/error.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <cmath>
#include <vector>

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

// --- Codec roundtrip tests ---

TEST(null_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Null, std::monostate{} };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(decoded.size() == 1);
    assert(decoded[0].frameType == FrameType::ServerValue);
    assert(decoded[0].keyId == 1);
    assert(decoded[0].dataType == DataType::Null);
    assert(std::holds_alternative<std::monostate>(decoded[0].payload));
}

TEST(bool_true_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Bool, Payload(true) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(decoded.size() == 1);
    assert(std::get<bool>(decoded[0].payload) == true);
}

TEST(bool_false_roundtrip) {
    Frame f{ FrameType::ServerValue, 2, DataType::Bool, Payload(false) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(decoded.size() == 1);
    assert(std::get<bool>(decoded[0].payload) == false);
}

TEST(uint8_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Uint8, Payload(uint8_t(255)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<uint8_t>(decoded[0].payload) == 255);
}

TEST(uint16_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Uint16, Payload(uint16_t(65535)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<uint16_t>(decoded[0].payload) == 65535);
}

TEST(uint32_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Uint32, Payload(uint32_t(0xDEADBEEF)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<uint32_t>(decoded[0].payload) == 0xDEADBEEF);
}

TEST(uint64_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Uint64, Payload(uint64_t(0x123456789ABCDEF0ULL)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<uint64_t>(decoded[0].payload) == 0x123456789ABCDEF0ULL);
}

TEST(int32_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Int32, Payload(int32_t(-42)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<int32_t>(decoded[0].payload) == -42);
}

TEST(int64_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Int64, Payload(int64_t(-9876543210LL)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<int64_t>(decoded[0].payload) == -9876543210LL);
}

TEST(float32_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Float32, Payload(3.14f) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    float result = std::get<float>(decoded[0].payload);
    assert(std::abs(result - 3.14f) < 0.001f);
}

TEST(float64_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::Float64, Payload(3.141592653589793) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    double result = std::get<double>(decoded[0].payload);
    assert(std::abs(result - 3.141592653589793) < 1e-15);
}

TEST(string_roundtrip) {
    Frame f{ FrameType::ServerValue, 1, DataType::String, Payload(std::string("Hello, World!")) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<std::string>(decoded[0].payload) == "Hello, World!");
}

TEST(binary_roundtrip) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x10, 0xFF, 0x00};
    Frame f{ FrameType::ServerValue, 1, DataType::Binary, Payload(data) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<std::vector<uint8_t>>(decoded[0].payload) == data);
}

TEST(timestamp_roundtrip) {
    int64_t ts = 1713273600000LL; // 2024-04-16T12:00:00Z
    Frame f{ FrameType::ServerValue, 1, DataType::Timestamp, Payload(ts) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<int64_t>(decoded[0].payload) == ts);
}

TEST(varinteger_positive) {
    Frame f{ FrameType::ServerValue, 3, DataType::VarInteger, Payload(int32_t(42)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<int32_t>(decoded[0].payload) == 42);
}

TEST(varinteger_negative) {
    Frame f{ FrameType::ServerValue, 3, DataType::VarInteger, Payload(int32_t(-42)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<int32_t>(decoded[0].payload) == -42);
}

TEST(varinteger_zero) {
    Frame f{ FrameType::ServerValue, 3, DataType::VarInteger, Payload(int32_t(0)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<int32_t>(decoded[0].payload) == 0);
}

TEST(varinteger_large) {
    Frame f{ FrameType::ServerValue, 3, DataType::VarInteger, Payload(int32_t(100000)) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(std::get<int32_t>(decoded[0].payload) == 100000);
}

TEST(vardouble_simple) {
    Frame f{ FrameType::ServerValue, 4, DataType::VarDouble, Payload(3.14) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    double result = std::get<double>(decoded[0].payload);
    assert(std::abs(result - 3.14) < 1e-10);
}

TEST(vardouble_negative) {
    Frame f{ FrameType::ServerValue, 4, DataType::VarDouble, Payload(-7.5) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    double result = std::get<double>(decoded[0].payload);
    assert(std::abs(result - (-7.5)) < 1e-10);
}

TEST(vardouble_small) {
    Frame f{ FrameType::ServerValue, 4, DataType::VarDouble, Payload(0.001) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    double result = std::get<double>(decoded[0].payload);
    assert(std::abs(result - 0.001) < 1e-10);
}

TEST(signal_frame) {
    Frame f{ FrameType::ServerSync, 0, DataType::Null, std::monostate{} };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(decoded.size() == 1);
    assert(decoded[0].frameType == FrameType::ServerSync);
    assert(std::holds_alternative<std::monostate>(decoded[0].payload));
}

TEST(key_registration_frame) {
    Frame f{ FrameType::ServerKeyRegistration, 5, DataType::String,
             Payload(std::string("user.name")) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(decoded.size() == 1);
    assert(decoded[0].frameType == FrameType::ServerKeyRegistration);
    assert(decoded[0].keyId == 5);
    assert(std::get<std::string>(decoded[0].payload) == "user.name");
}

TEST(dle_escape_in_keyid) {
    // KeyID 0x00000010 contains a DLE byte
    Frame f{ FrameType::ServerValue, 0x10, DataType::Bool, Payload(true) };
    auto encoded = encode(f);
    auto decoded = decode(encoded);
    assert(decoded.size() == 1);
    assert(decoded[0].keyId == 0x10);
    assert(std::get<bool>(decoded[0].payload) == true);
}

TEST(batch_encode_decode) {
    std::vector<Frame> frames;
    frames.push_back({ FrameType::ServerValue, 1, DataType::Bool, Payload(true) });
    frames.push_back({ FrameType::ServerValue, 2, DataType::String, Payload(std::string("test")) });
    frames.push_back({ FrameType::ServerSync, 0, DataType::Null, std::monostate{} });

    auto encoded = encodeBatch(frames);
    auto decoded = decode(encoded);
    assert(decoded.size() == 3);
    assert(std::get<bool>(decoded[0].payload) == true);
    assert(std::get<std::string>(decoded[1].payload) == "test");
    assert(decoded[2].frameType == FrameType::ServerSync);
}

TEST(heartbeat_encoding) {
    auto hb = encodeHeartbeat();
    assert(hb.size() == 2);
    assert(hb[0] == DLE);
    assert(hb[1] == ENQ);
}

// --- VarInt specific tests ---

TEST(varint_encode_decode) {
    auto encoded = encodeVarInt(0);
    size_t offset = 0;
    assert(decodeVarInt(encoded.data(), encoded.size(), offset) == 0);

    encoded = encodeVarInt(127);
    offset = 0;
    assert(decodeVarInt(encoded.data(), encoded.size(), offset) == 127);
    assert(encoded.size() == 1);

    encoded = encodeVarInt(128);
    offset = 0;
    assert(decodeVarInt(encoded.data(), encoded.size(), offset) == 128);
    assert(encoded.size() == 2);

    encoded = encodeVarInt(300);
    offset = 0;
    assert(decodeVarInt(encoded.data(), encoded.size(), offset) == 300);
}

// --- Wire compatibility tests (from protocol spec) ---

TEST(wire_bool_true_keyid1) {
    // From spec: 10 02 01 00 00 00 01 01 01 10 03
    uint8_t expected[] = { 0x10, 0x02, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x10, 0x03 };
    Frame f{ FrameType::ServerValue, 1, DataType::Bool, Payload(true) };
    auto encoded = encode(f);
    assert(encoded.size() == sizeof(expected));
    assert(std::memcmp(encoded.data(), expected, sizeof(expected)) == 0);
}

TEST(wire_signal_serversync) {
    // From spec: 10 02 04 00 00 00 00 00 10 03
    uint8_t expected[] = { 0x10, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03 };
    Frame f{ FrameType::ServerSync, 0, DataType::Null, std::monostate{} };
    auto encoded = encode(f);
    assert(encoded.size() == sizeof(expected));
    assert(std::memcmp(encoded.data(), expected, sizeof(expected)) == 0);
}

TEST(wire_varinteger_42) {
    // From spec: 10 02 01 00 00 00 03 0d 54 10 03
    // zigzag(42) = 84 = 0x54
    uint8_t expected[] = { 0x10, 0x02, 0x01, 0x00, 0x00, 0x00, 0x03, 0x0D, 0x54, 0x10, 0x03 };
    Frame f{ FrameType::ServerValue, 3, DataType::VarInteger, Payload(int32_t(42)) };
    auto encoded = encode(f);
    assert(encoded.size() == sizeof(expected));
    assert(std::memcmp(encoded.data(), expected, sizeof(expected)) == 0);
}

TEST(wire_vardouble_3_14) {
    // From spec: 10 02 01 00 00 00 04 0e 02 ba 02 10 03
    // scale=2, mantissa=314, varint(314) = ba 02
    uint8_t expected[] = { 0x10, 0x02, 0x01, 0x00, 0x00, 0x00, 0x04, 0x0E, 0x02, 0xBA, 0x02, 0x10, 0x03 };
    Frame f{ FrameType::ServerValue, 4, DataType::VarDouble, Payload(3.14) };
    auto encoded = encode(f);
    assert(encoded.size() == sizeof(expected));
    assert(std::memcmp(encoded.data(), expected, sizeof(expected)) == 0);
}

int main() {
    std::cout << "Codec tests:\n";
    // Tests run via static initialization

    std::cout << "\n" << tests_passed << " passed, " << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
