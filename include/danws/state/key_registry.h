#pragma once

#include "../protocol/types.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdint>

namespace danws {

struct KeyEntry {
    std::string path;
    DataType    type;
    uint32_t    keyId;
};

/// Validates a key path against protocol rules.
/// Throws DanWSError on invalid paths.
void validateKeyPath(const std::string& path);

/// Bidirectional key ID <-> path registry.
class KeyRegistry {
public:
    explicit KeyRegistry(size_t maxKeys = 10000);

    /// Register a single key with a specific keyId (for receiving remote registrations).
    void registerOne(uint32_t keyId, const std::string& path, DataType type);

    /// Look up by keyId.
    const KeyEntry* getByKeyId(uint32_t keyId) const;

    /// Look up by path.
    const KeyEntry* getByPath(const std::string& path) const;

    bool hasKeyId(uint32_t keyId) const;
    bool hasPath(const std::string& path) const;

    /// Remove a key by ID. Returns true if the key existed.
    bool removeByKeyId(uint32_t keyId);

    size_t size() const { return byId_.size(); }

    /// Get all registered paths.
    std::vector<std::string> paths() const;

    /// Clear all registrations.
    void clear();

private:
    std::unordered_map<uint32_t, KeyEntry> byId_;
    std::unordered_map<std::string, uint32_t> pathToId_;
    uint32_t nextId_ = 1;
    size_t maxKeys_;
};

} // namespace danws
