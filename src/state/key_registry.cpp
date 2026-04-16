#include "danws/state/key_registry.h"
#include "danws/protocol/error.h"
#include <regex>
#include <unordered_set>

namespace danws {

static const std::regex KEY_PATH_REGEX("^[a-zA-Z0-9_]+(\\.[a-zA-Z0-9_]+)*$");
static constexpr size_t MAX_KEY_PATH_BYTES = 200;

// Simple validation cache
static std::unordered_set<std::string> validatedPaths;
static constexpr size_t MAX_VALIDATED_CACHE = 10000;

void validateKeyPath(const std::string& path) {
    if (validatedPaths.count(path)) return;

    if (path.empty()) {
        throw DanWSError("INVALID_KEY_PATH", "Key path must not be empty");
    }
    if (!std::regex_match(path, KEY_PATH_REGEX)) {
        throw DanWSError("INVALID_KEY_PATH", "Invalid key path: \"" + path + "\"");
    }
    if (path.size() > MAX_KEY_PATH_BYTES) {
        throw DanWSError("INVALID_KEY_PATH", "Key path exceeds 200 bytes: \"" + path + "\"");
    }

    if (validatedPaths.size() >= MAX_VALIDATED_CACHE) {
        validatedPaths.clear();
    }
    validatedPaths.insert(path);
}

KeyRegistry::KeyRegistry(size_t maxKeys)
    : maxKeys_(maxKeys) {}

void KeyRegistry::registerOne(uint32_t keyId, const std::string& path, DataType type) {
    validateKeyPath(path);

    // Check limit only if this is a genuinely new path
    if (pathToId_.find(path) == pathToId_.end()) {
        if (byId_.size() >= maxKeys_) {
            throw DanWSError("KEY_LIMIT_EXCEEDED",
                "Key registry limit reached (" + std::to_string(maxKeys_) + ")");
        }
    }

    KeyEntry entry{ path, type, keyId };
    byId_[keyId] = entry;
    pathToId_[path] = keyId;

    if (keyId >= nextId_) {
        nextId_ = keyId + 1;
    }
}

const KeyEntry* KeyRegistry::getByKeyId(uint32_t keyId) const {
    auto it = byId_.find(keyId);
    if (it == byId_.end()) return nullptr;
    return &it->second;
}

const KeyEntry* KeyRegistry::getByPath(const std::string& path) const {
    auto it = pathToId_.find(path);
    if (it == pathToId_.end()) return nullptr;
    auto entryIt = byId_.find(it->second);
    if (entryIt == byId_.end()) return nullptr;
    return &entryIt->second;
}

bool KeyRegistry::hasKeyId(uint32_t keyId) const {
    return byId_.count(keyId) > 0;
}

bool KeyRegistry::hasPath(const std::string& path) const {
    return pathToId_.count(path) > 0;
}

bool KeyRegistry::removeByKeyId(uint32_t keyId) {
    auto it = byId_.find(keyId);
    if (it == byId_.end()) return false;
    pathToId_.erase(it->second.path);
    byId_.erase(it);
    return true;
}

std::vector<std::string> KeyRegistry::paths() const {
    std::vector<std::string> result;
    result.reserve(pathToId_.size());
    for (const auto& pair : pathToId_) {
        result.push_back(pair.first);
    }
    return result;
}

void KeyRegistry::clear() {
    byId_.clear();
    pathToId_.clear();
    nextId_ = 1;
}

} // namespace danws
