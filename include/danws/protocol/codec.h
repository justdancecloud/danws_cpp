#pragma once

#include "types.h"
#include <vector>
#include <cstdint>

namespace danws {

/// Encode a single Frame into bytes with DLE STX/ETX framing and DLE escaping.
std::vector<uint8_t> encode(const Frame& frame);

/// Encode multiple frames into a single concatenated buffer.
std::vector<uint8_t> encodeBatch(const std::vector<Frame>& frames);

/// Encode a heartbeat (DLE ENQ, 2 bytes).
std::vector<uint8_t> encodeHeartbeat();

/// Decode a byte buffer containing one or more complete frames.
/// Returns a vector of decoded Frames.
std::vector<Frame> decode(const uint8_t* data, size_t length);

/// Convenience overload.
inline std::vector<Frame> decode(const std::vector<uint8_t>& data) {
    return decode(data.data(), data.size());
}

} // namespace danws
