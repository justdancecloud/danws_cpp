#include "danws/protocol/stream_parser.h"
#include "danws/protocol/serializer.h"
#include "danws/protocol/error.h"
#include <sstream>

namespace danws {

StreamParser::StreamParser(size_t maxBufferSize)
    : maxBufferSize_(maxBufferSize) {
    buffer_.reserve(4096);
}

void StreamParser::onFrame(std::function<void(const Frame&)> callback) {
    frameCallbacks_.push_back(std::move(callback));
}

void StreamParser::onHeartbeat(std::function<void()> callback) {
    heartbeatCallbacks_.push_back(std::move(callback));
}

void StreamParser::onError(std::function<void(const std::string&)> callback) {
    errorCallbacks_.push_back(std::move(callback));
}

void StreamParser::reset() {
    state_ = State::Idle;
    buffer_.clear();
}

void StreamParser::emitFrame(const Frame& frame) {
    for (const auto& cb : frameCallbacks_) cb(frame);
}

void StreamParser::emitHeartbeat() {
    for (const auto& cb : heartbeatCallbacks_) cb();
}

void StreamParser::emitError(const std::string& msg) {
    for (const auto& cb : errorCallbacks_) cb(msg);
}

Frame StreamParser::parseFrame(const std::vector<uint8_t>& body) {
    if (body.size() < 6) {
        throw DanWSError("FRAME_PARSE_ERROR",
            "Frame body too short: " + std::to_string(body.size()) + " bytes");
    }

    FrameType frameType = static_cast<FrameType>(body[0]);
    uint32_t keyId = (static_cast<uint32_t>(body[1]) << 24) |
                     (static_cast<uint32_t>(body[2]) << 16) |
                     (static_cast<uint32_t>(body[3]) << 8) |
                     static_cast<uint32_t>(body[4]);
    DataType dataType = static_cast<DataType>(body[5]);

    const uint8_t* rawPayload = body.data() + 6;
    size_t payloadLen = body.size() - 6;

    Payload payload;
    if (isKeyRegistrationFrame(frameType)) {
        payload = std::string(reinterpret_cast<const char*>(rawPayload), payloadLen);
    } else if (isSignalFrame(frameType)) {
        payload = std::monostate{};
    } else {
        payload = deserialize(dataType, rawPayload, payloadLen);
    }

    return Frame{ frameType, keyId, dataType, std::move(payload) };
}

void StreamParser::feed(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = data[i];

        switch (state_) {
            case State::Idle:
                if (byte == DLE) {
                    state_ = State::AfterDLE;
                } else {
                    std::ostringstream oss;
                    oss << "Unexpected byte 0x" << std::hex << static_cast<int>(byte)
                        << " outside frame";
                    emitError(oss.str());
                }
                break;

            case State::AfterDLE:
                if (byte == STX) {
                    state_ = State::InFrame;
                    buffer_.clear();
                } else if (byte == ENQ) {
                    emitHeartbeat();
                    state_ = State::Idle;
                } else {
                    std::ostringstream oss;
                    oss << "Invalid DLE sequence: 0x10 0x" << std::hex << static_cast<int>(byte);
                    emitError(oss.str());
                    state_ = State::Idle;
                }
                break;

            case State::InFrame:
                if (byte == DLE) {
                    state_ = State::InFrameAfterDLE;
                } else {
                    if (buffer_.size() >= maxBufferSize_) {
                        emitError("Frame exceeds maximum buffer size");
                        buffer_.clear();
                        state_ = State::Idle;
                    } else {
                        buffer_.push_back(byte);
                    }
                }
                break;

            case State::InFrameAfterDLE:
                if (byte == ETX) {
                    // Frame complete
                    try {
                        auto frame = parseFrame(buffer_);
                        emitFrame(frame);
                    } catch (const std::exception& e) {
                        emitError(e.what());
                    }
                    buffer_.clear();
                    state_ = State::Idle;
                } else if (byte == DLE) {
                    // Escaped DLE — decode immediately, store single 0x10
                    buffer_.push_back(DLE);
                    state_ = State::InFrame;
                } else {
                    std::ostringstream oss;
                    oss << "Invalid DLE sequence in frame: 0x10 0x"
                        << std::hex << static_cast<int>(byte);
                    emitError(oss.str());
                    buffer_.clear();
                    state_ = State::Idle;
                }
                break;
        }
    }
}

} // namespace danws
