#include "danws/protocol/codec.h"
#include "danws/protocol/serializer.h"
#include "danws/protocol/error.h"
#include <sstream>

namespace danws {

// --- DLE encode/decode ---

static std::vector<uint8_t> dleEncode(const std::vector<uint8_t>& payload) {
    // Count DLE bytes
    size_t dleCount = 0;
    for (uint8_t b : payload) {
        if (b == DLE) ++dleCount;
    }
    if (dleCount == 0) return payload;

    std::vector<uint8_t> out;
    out.reserve(payload.size() + dleCount);
    for (uint8_t b : payload) {
        out.push_back(b);
        if (b == DLE) out.push_back(DLE);
    }
    return out;
}

static std::vector<uint8_t> dleDecode(const uint8_t* data, size_t length) {
    // Count DLE pairs
    size_t dleCount = 0;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == DLE) {
            ++i; // skip next byte (should be DLE)
            ++dleCount;
        }
    }
    if (dleCount == 0) {
        return std::vector<uint8_t>(data, data + length);
    }

    std::vector<uint8_t> out;
    out.reserve(length - dleCount);
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == DLE) {
            ++i; // skip the doubled DLE, output one
        }
        if (i < length) {
            out.push_back(data[i]);
        }
    }
    return out;
}

// --- Encode ---

std::vector<uint8_t> encode(const Frame& frame) {
    // Serialize payload based on frame type
    std::vector<uint8_t> rawPayload;

    if (isKeyRegistrationFrame(frame.frameType)) {
        // Key registration: payload is UTF-8 keyPath string
        const auto& path = std::get<std::string>(frame.payload);
        rawPayload.assign(path.begin(), path.end());
    } else if (isSignalFrame(frame.frameType)) {
        // Signal frames: no payload
    } else {
        // Data/auth frames: typed value
        rawPayload = serialize(frame.dataType, frame.payload);
    }

    // Build raw body: [FrameType:1] [KeyID:4] [DataType:1] [Payload:N]
    std::vector<uint8_t> rawBody;
    rawBody.reserve(6 + rawPayload.size());
    rawBody.push_back(static_cast<uint8_t>(frame.frameType));
    rawBody.push_back(static_cast<uint8_t>((frame.keyId >> 24) & 0xFF));
    rawBody.push_back(static_cast<uint8_t>((frame.keyId >> 16) & 0xFF));
    rawBody.push_back(static_cast<uint8_t>((frame.keyId >> 8) & 0xFF));
    rawBody.push_back(static_cast<uint8_t>(frame.keyId & 0xFF));
    rawBody.push_back(static_cast<uint8_t>(frame.dataType));
    rawBody.insert(rawBody.end(), rawPayload.begin(), rawPayload.end());

    // DLE-escape the entire body
    auto escapedBody = dleEncode(rawBody);

    // Wrap with DLE STX ... DLE ETX
    std::vector<uint8_t> result;
    result.reserve(2 + escapedBody.size() + 2);
    result.push_back(DLE);
    result.push_back(STX);
    result.insert(result.end(), escapedBody.begin(), escapedBody.end());
    result.push_back(DLE);
    result.push_back(ETX);

    return result;
}

std::vector<uint8_t> encodeBatch(const std::vector<Frame>& frames) {
    std::vector<uint8_t> result;
    for (const auto& frame : frames) {
        auto encoded = encode(frame);
        result.insert(result.end(), encoded.begin(), encoded.end());
    }
    return result;
}

std::vector<uint8_t> encodeHeartbeat() {
    return { DLE, ENQ };
}

// --- Decode ---

std::vector<Frame> decode(const uint8_t* data, size_t length) {
    std::vector<Frame> frames;
    size_t i = 0;

    while (i < length) {
        // Expect DLE STX
        if (i + 1 >= length) {
            throw DanWSError("FRAME_PARSE_ERROR", "Unexpected end of data");
        }
        if (data[i] != DLE || data[i + 1] != STX) {
            std::ostringstream oss;
            oss << "Expected DLE STX at offset " << i
                << ", got 0x" << std::hex << static_cast<int>(data[i])
                << " 0x" << static_cast<int>(data[i + 1]);
            throw DanWSError("FRAME_PARSE_ERROR", oss.str());
        }
        i += 2; // skip DLE STX

        // Find DLE ETX
        size_t bodyStart = i;
        size_t bodyEnd = 0;
        bool found = false;

        while (i < length) {
            if (data[i] == DLE) {
                if (i + 1 >= length) {
                    throw DanWSError("FRAME_PARSE_ERROR", "Unexpected end after DLE");
                }
                if (data[i + 1] == ETX) {
                    bodyEnd = i;
                    i += 2; // skip DLE ETX
                    found = true;
                    break;
                } else if (data[i + 1] == DLE) {
                    i += 2; // escaped DLE
                } else {
                    throw DanWSError("INVALID_DLE_SEQUENCE", "Invalid DLE sequence in frame");
                }
            } else {
                i++;
            }
        }

        if (!found) {
            throw DanWSError("FRAME_PARSE_ERROR", "Missing DLE ETX terminator");
        }

        // DLE-decode the body
        auto decoded = dleDecode(data + bodyStart, bodyEnd - bodyStart);

        if (decoded.size() < 6) {
            throw DanWSError("FRAME_PARSE_ERROR",
                "Frame body too short: " + std::to_string(decoded.size()) + " bytes");
        }

        FrameType frameType = static_cast<FrameType>(decoded[0]);
        uint32_t keyId = (static_cast<uint32_t>(decoded[1]) << 24) |
                         (static_cast<uint32_t>(decoded[2]) << 16) |
                         (static_cast<uint32_t>(decoded[3]) << 8) |
                         static_cast<uint32_t>(decoded[4]);
        DataType dataType = static_cast<DataType>(decoded[5]);

        const uint8_t* payloadData = decoded.data() + 6;
        size_t payloadLen = decoded.size() - 6;

        Payload payload;
        if (isKeyRegistrationFrame(frameType)) {
            payload = std::string(reinterpret_cast<const char*>(payloadData), payloadLen);
        } else if (isSignalFrame(frameType)) {
            payload = std::monostate{};
        } else {
            payload = deserialize(dataType, payloadData, payloadLen);
        }

        frames.push_back(Frame{ frameType, keyId, dataType, std::move(payload) });
    }

    return frames;
}

} // namespace danws
