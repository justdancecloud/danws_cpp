#include "danws/protocol/serializer.h"
#include "danws/protocol/error.h"
#include <cstring>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <limits>
#include <algorithm>

namespace danws {

// --- Endian helpers (Big Endian / network byte order) ---

static void writeBE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void writeBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void writeBE64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

static uint16_t readBE16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

static uint32_t readBE32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

static uint64_t readBE64(const uint8_t* data) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result = (result << 8) | data[i];
    }
    return result;
}

static void writeFloat32BE(std::vector<uint8_t>& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    // Convert to big endian
    writeBE32(out, bits);
}

static void writeFloat64BE(std::vector<uint8_t>& out, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    writeBE64(out, bits);
}

static float readFloat32BE(const uint8_t* data) {
    uint32_t bits = readBE32(data);
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}

static double readFloat64BE(const uint8_t* data) {
    uint64_t bits = readBE64(data);
    double result;
    std::memcpy(&result, &bits, 8);
    return result;
}

// --- VarInt ---

std::vector<uint8_t> encodeVarInt(uint64_t value) {
    std::vector<uint8_t> bytes;
    if (value == 0) {
        bytes.push_back(0);
        return bytes;
    }
    while (value > 0) {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value > 0) byte |= 0x80;
        bytes.push_back(byte);
    }
    return bytes;
}

uint64_t decodeVarInt(const uint8_t* data, size_t length, size_t& offset) {
    uint64_t value = 0;
    uint64_t multiplier = 1;
    while (offset < length) {
        uint8_t byte = data[offset];
        value += static_cast<uint64_t>(byte & 0x7F) * multiplier;
        multiplier *= 128;
        offset++;
        if ((byte & 0x80) == 0) break;
    }
    return value;
}

// --- VarInteger (zigzag + VarInt) ---

static std::vector<uint8_t> serializeVarInteger(int64_t value) {
    // Zigzag encode: 0->0, -1->1, 1->2, -2->3, 2->4, ...
    uint64_t zigzag;
    if (value >= 0) {
        zigzag = static_cast<uint64_t>(value) * 2;
    } else {
        zigzag = static_cast<uint64_t>(-value) * 2 - 1;
    }
    return encodeVarInt(zigzag);
}

static int64_t deserializeVarInteger(const uint8_t* data, size_t length) {
    if (length == 0) {
        throw DanWSError("PAYLOAD_SIZE_MISMATCH", "VarInteger requires at least 1 byte");
    }
    size_t offset = 0;
    uint64_t zigzag = decodeVarInt(data, length, offset);
    // Zigzag decode
    if (zigzag & 1) {
        return -static_cast<int64_t>(zigzag / 2) - 1;
    } else {
        return static_cast<int64_t>(zigzag / 2);
    }
}

// --- VarDouble ---

static std::vector<uint8_t> fallbackFloat64(double value) {
    std::vector<uint8_t> result;
    result.push_back(0x80);
    writeFloat64BE(result, value);
    return result;
}

static std::vector<uint8_t> serializeVarDouble(double value) {
    if (!std::isfinite(value) || (value == 0.0 && std::signbit(value))) {
        return fallbackFloat64(value);
    }

    double absVal = std::abs(value);
    int scale = 0;
    uint64_t mantissa = static_cast<uint64_t>(absVal);

    // Determine scale via string representation to avoid floating-point drift
    std::ostringstream oss;
    oss << std::setprecision(17) << absVal;
    std::string str = oss.str();

    auto dotIdx = str.find('.');
    if (dotIdx != std::string::npos) {
        // Check for scientific notation
        if (str.find('e') != std::string::npos || str.find('E') != std::string::npos) {
            return fallbackFloat64(value);
        }
        // Remove trailing zeros for clean scale calculation
        auto lastNonZero = str.find_last_not_of('0');
        if (lastNonZero != std::string::npos && lastNonZero > dotIdx) {
            str = str.substr(0, lastNonZero + 1);
        } else if (lastNonZero == dotIdx) {
            str = str.substr(0, dotIdx);
        }
        // Recalculate after trimming
        dotIdx = str.find('.');
        if (dotIdx != std::string::npos) {
            scale = static_cast<int>(str.length() - dotIdx - 1);
            if (scale > 63) return fallbackFloat64(value);
            // Build mantissa by removing the dot
            std::string mantissaStr = str.substr(0, dotIdx) + str.substr(dotIdx + 1);
            // Parse mantissa
            try {
                mantissa = std::stoull(mantissaStr);
            } catch (...) {
                return fallbackFloat64(value);
            }
        } else {
            mantissa = static_cast<uint64_t>(absVal);
        }
    }

    // Check mantissa fits in safe range
    constexpr uint64_t MAX_SAFE_INT = 9007199254740991ULL; // 2^53 - 1
    if (mantissa > MAX_SAFE_INT) {
        return fallbackFloat64(value);
    }

    bool negative = value < 0;
    uint8_t firstByte = negative ? static_cast<uint8_t>(scale + 64) : static_cast<uint8_t>(scale);
    auto varint = encodeVarInt(mantissa);

    std::vector<uint8_t> result;
    result.push_back(firstByte);
    result.insert(result.end(), varint.begin(), varint.end());
    return result;
}

static double deserializeVarDouble(const uint8_t* data, size_t length) {
    if (length == 0) {
        throw DanWSError("PAYLOAD_SIZE_MISMATCH", "VarDouble requires at least 1 byte");
    }

    uint8_t firstByte = data[0];

    if (firstByte == 0x80) {
        if (length < 9) {
            throw DanWSError("PAYLOAD_SIZE_MISMATCH", "VarDouble fallback requires 9 bytes");
        }
        return readFloat64BE(data + 1);
    }

    bool negative = firstByte >= 64;
    int scale = negative ? (firstByte - 64) : firstByte;

    size_t offset = 1;
    uint64_t mantissa = decodeVarInt(data, length, offset);

    double result = static_cast<double>(mantissa) / std::pow(10.0, scale);
    if (negative) result = -result;
    return result;
}

// --- VarFloat ---

static std::vector<uint8_t> fallbackFloat32(float value) {
    std::vector<uint8_t> result;
    result.push_back(0x80);
    writeFloat32BE(result, value);
    return result;
}

static std::vector<uint8_t> serializeVarFloat(double value) {
    float fval = static_cast<float>(value);
    if (!std::isfinite(fval) || (fval == 0.0f && std::signbit(fval))) {
        return fallbackFloat32(fval);
    }

    double absVal = std::abs(value);
    int scale = 0;
    uint64_t mantissa = static_cast<uint64_t>(absVal);

    std::ostringstream oss;
    oss << std::setprecision(17) << absVal;
    std::string str = oss.str();

    auto dotIdx = str.find('.');
    if (dotIdx != std::string::npos) {
        if (str.find('e') != std::string::npos || str.find('E') != std::string::npos) {
            return fallbackFloat32(fval);
        }
        auto lastNonZero = str.find_last_not_of('0');
        if (lastNonZero != std::string::npos && lastNonZero > dotIdx) {
            str = str.substr(0, lastNonZero + 1);
        } else if (lastNonZero == dotIdx) {
            str = str.substr(0, dotIdx);
        }
        dotIdx = str.find('.');
        if (dotIdx != std::string::npos) {
            scale = static_cast<int>(str.length() - dotIdx - 1);
            if (scale > 63) return fallbackFloat32(fval);
            std::string mantissaStr = str.substr(0, dotIdx) + str.substr(dotIdx + 1);
            try {
                mantissa = std::stoull(mantissaStr);
            } catch (...) {
                return fallbackFloat32(fval);
            }
        } else {
            mantissa = static_cast<uint64_t>(absVal);
        }
    }

    constexpr uint64_t MAX_SAFE_INT = 9007199254740991ULL;
    if (mantissa > MAX_SAFE_INT) {
        return fallbackFloat32(fval);
    }

    bool negative = value < 0;
    uint8_t firstByte = negative ? static_cast<uint8_t>(scale + 64) : static_cast<uint8_t>(scale);
    auto varint = encodeVarInt(mantissa);

    std::vector<uint8_t> result;
    result.push_back(firstByte);
    result.insert(result.end(), varint.begin(), varint.end());
    return result;
}

static double deserializeVarFloat(const uint8_t* data, size_t length) {
    if (length == 0) {
        throw DanWSError("PAYLOAD_SIZE_MISMATCH", "VarFloat requires at least 1 byte");
    }

    uint8_t firstByte = data[0];

    if (firstByte == 0x80) {
        if (length < 5) {
            throw DanWSError("PAYLOAD_SIZE_MISMATCH", "VarFloat fallback requires 5 bytes");
        }
        return static_cast<double>(readFloat32BE(data + 1));
    }

    bool negative = firstByte >= 64;
    int scale = negative ? (firstByte - 64) : firstByte;

    size_t offset = 1;
    uint64_t mantissa = decodeVarInt(data, length, offset);

    double result = static_cast<double>(mantissa) / std::pow(10.0, scale);
    if (negative) result = -result;
    return result;
}

// --- Main serialize/deserialize ---

std::vector<uint8_t> serialize(DataType dataType, const Payload& value) {
    switch (dataType) {
        case DataType::Null:
            return {};

        case DataType::Bool: {
            bool v = std::get<bool>(value);
            return { static_cast<uint8_t>(v ? 0x01 : 0x00) };
        }

        case DataType::Uint8: {
            uint8_t v = std::get<uint8_t>(value);
            return { v };
        }

        case DataType::Uint16: {
            uint16_t v = std::get<uint16_t>(value);
            std::vector<uint8_t> out;
            writeBE16(out, v);
            return out;
        }

        case DataType::Uint32: {
            uint32_t v = std::get<uint32_t>(value);
            std::vector<uint8_t> out;
            writeBE32(out, v);
            return out;
        }

        case DataType::Uint64: {
            uint64_t v = std::get<uint64_t>(value);
            std::vector<uint8_t> out;
            writeBE64(out, v);
            return out;
        }

        case DataType::Int32: {
            int32_t v = std::get<int32_t>(value);
            std::vector<uint8_t> out;
            writeBE32(out, static_cast<uint32_t>(v));
            return out;
        }

        case DataType::Int64: {
            int64_t v = std::get<int64_t>(value);
            std::vector<uint8_t> out;
            writeBE64(out, static_cast<uint64_t>(v));
            return out;
        }

        case DataType::Float32: {
            float v = std::get<float>(value);
            std::vector<uint8_t> out;
            writeFloat32BE(out, v);
            return out;
        }

        case DataType::Float64: {
            double v = std::get<double>(value);
            std::vector<uint8_t> out;
            writeFloat64BE(out, v);
            return out;
        }

        case DataType::String: {
            const std::string& v = std::get<std::string>(value);
            return std::vector<uint8_t>(v.begin(), v.end());
        }

        case DataType::Binary: {
            return std::get<std::vector<uint8_t>>(value);
        }

        case DataType::Timestamp: {
            int64_t v = std::get<int64_t>(value);
            std::vector<uint8_t> out;
            writeBE64(out, static_cast<uint64_t>(v));
            return out;
        }

        case DataType::VarInteger: {
            // Accept int32_t or int64_t
            if (auto* v = std::get_if<int32_t>(&value)) {
                return serializeVarInteger(static_cast<int64_t>(*v));
            }
            if (auto* v = std::get_if<int64_t>(&value)) {
                return serializeVarInteger(*v);
            }
            if (auto* v = std::get_if<double>(&value)) {
                return serializeVarInteger(static_cast<int64_t>(*v));
            }
            throw DanWSError("INVALID_VALUE_TYPE", "VarInteger requires integer value");
        }

        case DataType::VarDouble: {
            double v = std::get<double>(value);
            return serializeVarDouble(v);
        }

        case DataType::VarFloat: {
            double v;
            if (auto* fv = std::get_if<float>(&value)) {
                v = static_cast<double>(*fv);
            } else {
                v = std::get<double>(value);
            }
            return serializeVarFloat(v);
        }

        default:
            throw DanWSError("UNKNOWN_DATA_TYPE", "Unknown data type");
    }
}

Payload deserialize(DataType dataType, const uint8_t* data, size_t length) {
    int expected = dataTypeSize(dataType);
    if (expected >= 0 && length != static_cast<size_t>(expected)) {
        throw DanWSError("PAYLOAD_SIZE_MISMATCH",
            std::string(1, '0') + " expects " + std::to_string(expected) +
            " bytes, got " + std::to_string(length));
    }

    switch (dataType) {
        case DataType::Null:
            return std::monostate{};

        case DataType::Bool:
            if (data[0] == 0x01) return true;
            if (data[0] == 0x00) return false;
            throw DanWSError("INVALID_VALUE_TYPE", "Bool payload must be 0x00 or 0x01");

        case DataType::Uint8:
            return data[0];

        case DataType::Uint16:
            return readBE16(data);

        case DataType::Uint32:
            return readBE32(data);

        case DataType::Uint64:
            return readBE64(data);

        case DataType::Int32:
            return static_cast<int32_t>(readBE32(data));

        case DataType::Int64:
            return static_cast<int64_t>(readBE64(data));

        case DataType::Float32:
            return readFloat32BE(data);

        case DataType::Float64:
            return readFloat64BE(data);

        case DataType::String:
            return std::string(reinterpret_cast<const char*>(data), length);

        case DataType::Binary:
            return std::vector<uint8_t>(data, data + length);

        case DataType::Timestamp:
            return static_cast<int64_t>(readBE64(data));

        case DataType::VarInteger: {
            int64_t val = deserializeVarInteger(data, length);
            // Return as int32_t if it fits, otherwise int64_t
            if (val >= std::numeric_limits<int32_t>::min() &&
                val <= std::numeric_limits<int32_t>::max()) {
                return static_cast<int32_t>(val);
            }
            return val;
        }

        case DataType::VarDouble:
            return deserializeVarDouble(data, length);

        case DataType::VarFloat:
            return deserializeVarFloat(data, length);

        default:
            throw DanWSError("UNKNOWN_DATA_TYPE", "Unknown data type");
    }
}

} // namespace danws
