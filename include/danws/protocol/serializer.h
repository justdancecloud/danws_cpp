#pragma once

#include "types.h"
#include <vector>
#include <cstdint>

namespace danws {

/// Serialize a typed value into raw payload bytes.
std::vector<uint8_t> serialize(DataType dataType, const Payload& value);

/// Deserialize raw payload bytes into a typed value.
Payload deserialize(DataType dataType, const uint8_t* data, size_t length);

// --- VarInt helpers (exposed for testing) ---

/// Encode an unsigned integer as a protobuf-style VarInt.
std::vector<uint8_t> encodeVarInt(uint64_t value);

/// Decode a protobuf-style VarInt from a buffer. Returns the decoded value.
/// `offset` is updated to point past the consumed bytes.
uint64_t decodeVarInt(const uint8_t* data, size_t length, size_t& offset);

} // namespace danws
