#pragma once

#include "../protocol/types.h"
#include "../protocol/error.h"
#include "../state/key_registry.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

namespace danws {

/// Server-side flat state manager. Stores key-value pairs and produces
/// protocol frames for synchronization with clients.
/// Mirrors the TS FlatStateManager.
class FlatState {
public:
    struct Entry {
        uint32_t keyId;
        DataType type;
        Payload  value;
    };

    struct Callbacks {
        std::function<uint32_t()> allocateKeyId;
        std::function<void(const Frame&)> enqueue;
        std::function<void()> onResync;
        std::string wirePrefix;  // e.g. "t.0." for topics, "" for flat
    };

    explicit FlatState(Callbacks cb);

    void set(const std::string& key, const Payload& value);
    Payload get(const std::string& key) const;
    std::vector<std::string> keys() const;
    size_t size() const { return entries_.size(); }

    void clear();
    void clear(const std::string& key);

    std::vector<Frame> buildKeyFrames() const;
    std::vector<Frame> buildValueFrames() const;

    const Entry* getByKeyId(uint32_t keyId) const;
    std::string getPathByKeyId(uint32_t keyId) const;

private:
    Callbacks cb_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<uint32_t, std::string> byKeyId_;

    /// Returns detected DataType for the given Payload.
    static DataType detectDataType(const Payload& value);

    /// Set a single leaf key. Returns true if a new key was created.
    bool setLeaf(const std::string& key, const Payload& value);
};

} // namespace danws
