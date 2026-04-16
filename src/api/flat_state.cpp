#include "danws/api/flat_state.h"
#include "danws/protocol/serializer.h"

namespace danws {

FlatState::FlatState(Callbacks cb)
    : cb_(std::move(cb)) {}

DataType FlatState::detectDataType(const Payload& value) {
    return std::visit([](const auto& v) -> DataType {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) return DataType::Null;
        else if constexpr (std::is_same_v<T, bool>) return DataType::Bool;
        else if constexpr (std::is_same_v<T, uint8_t>) return DataType::Uint8;
        else if constexpr (std::is_same_v<T, uint16_t>) return DataType::Uint16;
        else if constexpr (std::is_same_v<T, uint32_t>) return DataType::Uint32;
        else if constexpr (std::is_same_v<T, uint64_t>) return DataType::Uint64;
        else if constexpr (std::is_same_v<T, int32_t>) return DataType::VarInteger;
        else if constexpr (std::is_same_v<T, int64_t>) return DataType::Int64;
        else if constexpr (std::is_same_v<T, float>) return DataType::VarFloat;
        else if constexpr (std::is_same_v<T, double>) return DataType::VarDouble;
        else if constexpr (std::is_same_v<T, std::string>) return DataType::String;
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) return DataType::Binary;
        else return DataType::Null;
    }, value);
}

bool FlatState::setLeaf(const std::string& key, const Payload& value) {
    DataType newType = detectDataType(value);

    auto it = entries_.find(key);
    if (it != entries_.end()) {
        // Existing key
        if (it->second.type != newType) {
            // Type changed: delete old, register new
            Frame delFrame{ FrameType::ServerKeyDelete, it->second.keyId,
                           DataType::Null, std::monostate{} };
            cb_.enqueue(delFrame);
            byKeyId_.erase(it->second.keyId);
            entries_.erase(it);

            uint32_t newKeyId = cb_.allocateKeyId();
            Entry newEntry{ newKeyId, newType, value };
            entries_[key] = newEntry;
            byKeyId_[newKeyId] = key;

            std::string wirePath = cb_.wirePrefix.empty() ? key : (cb_.wirePrefix + key);
            cb_.enqueue({ FrameType::ServerKeyRegistration, newKeyId, newType, Payload(wirePath) });
            cb_.enqueue({ FrameType::ServerSync, 0, DataType::Null, std::monostate{} });
            cb_.enqueue({ FrameType::ServerValue, newKeyId, newType, value });
            return false;
        }

        it->second.value = value;
        cb_.enqueue({ FrameType::ServerValue, it->second.keyId, it->second.type, value });
        return false;
    }

    // New key
    uint32_t keyId = cb_.allocateKeyId();
    Entry newEntry{ keyId, newType, value };
    entries_[key] = newEntry;
    byKeyId_[keyId] = key;

    std::string wirePath = cb_.wirePrefix.empty() ? key : (cb_.wirePrefix + key);
    Frame keyFrame{ FrameType::ServerKeyRegistration, keyId, newType, Payload(wirePath) };
    Frame syncFrame{ FrameType::ServerSync, 0, DataType::Null, std::monostate{} };
    Frame valueFrame{ FrameType::ServerValue, keyId, newType, value };

    cb_.enqueue(keyFrame);
    cb_.enqueue(syncFrame);
    cb_.enqueue(valueFrame);
    return true;
}

void FlatState::set(const std::string& key, const Payload& value) {
    setLeaf(key, value);
}

Payload FlatState::get(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::monostate{};
    return it->second.value;
}

std::vector<std::string> FlatState::keys() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& pair : entries_) {
        result.push_back(pair.first);
    }
    return result;
}

void FlatState::clear() {
    if (entries_.empty()) return;
    entries_.clear();
    byKeyId_.clear();
    cb_.onResync();
}

void FlatState::clear(const std::string& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    cb_.enqueue({ FrameType::ServerKeyDelete, it->second.keyId,
                 DataType::Null, std::monostate{} });
    byKeyId_.erase(it->second.keyId);
    entries_.erase(it);
}

std::vector<Frame> FlatState::buildKeyFrames() const {
    std::vector<Frame> frames;
    frames.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        std::string wirePath = cb_.wirePrefix.empty() ? key : (cb_.wirePrefix + key);
        frames.push_back({ FrameType::ServerKeyRegistration, entry.keyId,
                          entry.type, Payload(wirePath) });
    }
    return frames;
}

std::vector<Frame> FlatState::buildValueFrames() const {
    std::vector<Frame> frames;
    frames.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        frames.push_back({ FrameType::ServerValue, entry.keyId, entry.type, entry.value });
    }
    return frames;
}

const FlatState::Entry* FlatState::getByKeyId(uint32_t keyId) const {
    auto it = byKeyId_.find(keyId);
    if (it == byKeyId_.end()) return nullptr;
    auto entryIt = entries_.find(it->second);
    if (entryIt == entries_.end()) return nullptr;
    return &entryIt->second;
}

std::string FlatState::getPathByKeyId(uint32_t keyId) const {
    auto it = byKeyId_.find(keyId);
    if (it == byKeyId_.end()) return "";
    return it->second;
}

} // namespace danws
