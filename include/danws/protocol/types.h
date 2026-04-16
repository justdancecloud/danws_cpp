#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <stdexcept>
#include <chrono>

namespace danws {

// Control characters
constexpr uint8_t DLE = 0x10;
constexpr uint8_t STX = 0x02;
constexpr uint8_t ETX = 0x03;
constexpr uint8_t ENQ = 0x05;

enum class DataType : uint8_t {
    Null       = 0x00,
    Bool       = 0x01,
    Uint8      = 0x02,
    Uint16     = 0x03,
    Uint32     = 0x04,
    Uint64     = 0x05,
    Int32      = 0x06,
    Int64      = 0x07,
    Float32    = 0x08,
    Float64    = 0x09,
    String     = 0x0A,
    Binary     = 0x0B,
    Timestamp  = 0x0C,
    VarInteger = 0x0D,
    VarDouble  = 0x0E,
    VarFloat   = 0x0F,
};

enum class FrameType : uint8_t {
    ServerKeyRegistration = 0x00,
    ServerValue           = 0x01,
    ClientKeyRegistration = 0x02,
    ClientValue           = 0x03,
    ServerSync            = 0x04,
    ClientReady           = 0x05,
    ClientSync            = 0x06,
    ServerReady           = 0x07,
    Error                 = 0x08,
    ServerReset           = 0x09,
    ClientResyncReq       = 0x0A,
    ClientReset           = 0x0B,
    ServerResyncReq       = 0x0C,
    Identify              = 0x0D,
    Auth                  = 0x0E,
    AuthOk                = 0x0F,
    AuthFail              = 0x11,
    ArrayShiftLeft        = 0x20,
    ArrayShiftRight       = 0x21,
    ServerKeyDelete       = 0x22,
    ClientKeyRequest      = 0x23,
    ServerFlushEnd        = 0xFF,
};

/// Payload variant type — covers all possible wire values.
/// std::monostate represents Null.
using Payload = std::variant<
    std::monostate,                                    // Null
    bool,                                              // Bool
    uint8_t,                                           // Uint8 (disambiguated via DataType)
    uint16_t,                                          // Uint16
    uint32_t,                                          // Uint32
    uint64_t,                                          // Uint64
    int32_t,                                           // Int32
    int64_t,                                           // Int64, Timestamp (ms since epoch)
    float,                                             // Float32
    double,                                            // Float64, VarDouble, VarFloat
    std::string,                                       // String, key paths
    std::vector<uint8_t>                               // Binary
>;

struct Frame {
    FrameType frameType;
    uint32_t  keyId;
    DataType  dataType;
    Payload   payload;
};

/// Check if a frame type is a signal (no payload).
inline bool isSignalFrame(FrameType ft) {
    switch (ft) {
        case FrameType::ServerSync:
        case FrameType::ClientReady:
        case FrameType::ClientSync:
        case FrameType::ServerReady:
        case FrameType::ServerReset:
        case FrameType::ClientResyncReq:
        case FrameType::ClientReset:
        case FrameType::ServerResyncReq:
        case FrameType::AuthOk:
        case FrameType::ServerFlushEnd:
        case FrameType::ServerKeyDelete:
        case FrameType::ClientKeyRequest:
            return true;
        default:
            return false;
    }
}

/// Check if a frame type is a key registration.
inline bool isKeyRegistrationFrame(FrameType ft) {
    return ft == FrameType::ServerKeyRegistration ||
           ft == FrameType::ClientKeyRegistration;
}

/// Fixed byte sizes for each data type. -1 means variable length.
inline int dataTypeSize(DataType dt) {
    switch (dt) {
        case DataType::Null:       return 0;
        case DataType::Bool:       return 1;
        case DataType::Uint8:      return 1;
        case DataType::Uint16:     return 2;
        case DataType::Uint32:     return 4;
        case DataType::Uint64:     return 8;
        case DataType::Int32:      return 4;
        case DataType::Int64:      return 8;
        case DataType::Float32:    return 4;
        case DataType::Float64:    return 8;
        case DataType::String:     return -1;
        case DataType::Binary:     return -1;
        case DataType::Timestamp:  return 8;
        case DataType::VarInteger: return -1;
        case DataType::VarDouble:  return -1;
        case DataType::VarFloat:   return -1;
        default:                   return -1;
    }
}

} // namespace danws
