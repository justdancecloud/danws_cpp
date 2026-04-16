#pragma once

#include "../state/key_registry.h"
#include "../protocol/types.h"
#include <string>
#include <functional>
#include <vector>
#include <unordered_map>

namespace danws {

/// Scoped view into a single topic's data within the client state.
class TopicClientHandle {
public:
    TopicClientHandle(const std::string& name, int index,
                      KeyRegistry& registry,
                      std::function<Payload(uint32_t)> storeGet);

    const std::string& name() const { return name_; }

    /// Get a value by user-facing key (without topic wire prefix).
    Payload get(const std::string& key) const;

    /// Get all user-facing keys for this topic.
    std::vector<std::string> keys() const;

    /// Register per-key receive callback. Returns unsubscribe function.
    std::function<void()> onReceive(std::function<void(const std::string&, const Payload&)> cb);

    /// Register batch update callback. Returns unsubscribe function.
    std::function<void()> onUpdate(std::function<void()> cb);

    // Internal methods used by DanWebSocketClient
    void _notify(const std::string& userKey, const Payload& value);
    void _flushUpdate();
    void _setIndex(int index);
    int  _idx() const { return index_; }

private:
    std::string name_;
    int index_;
    KeyRegistry& registry_;
    std::function<Payload(uint32_t)> storeGet_;
    std::vector<std::function<void(const std::string&, const Payload&)>> onReceive_;
    std::vector<std::function<void()>> onUpdate_;
    bool dirty_ = false;
};

} // namespace danws
