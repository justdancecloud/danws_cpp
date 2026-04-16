#pragma once

#include "../protocol/types.h"
#include "flat_state.h"
#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace danws {

/// Shared TX state for one principal. All sessions of the same principal
/// share this state. Mirrors the TS PrincipalTX.
class PrincipalTX {
public:
    explicit PrincipalTX(const std::string& name);

    const std::string& name() const { return name_; }

    void set(const std::string& key, const Payload& value);
    Payload get(const std::string& key) const;
    std::vector<std::string> keys() const;

    void clear();
    void clear(const std::string& key);

    // Internal: event binding (called by server)
    void _onValue(std::function<void(const Frame&)> fn);
    void _onResync(std::function<void()> fn);
    void _onIncremental(std::function<void(const Frame&, const Frame&, const Frame&)> fn);

    // Internal: build frames for syncing new sessions
    std::vector<Frame> _buildKeyFrames();
    std::vector<Frame> _buildValueFrames();

private:
    std::string name_;
    uint32_t nextKeyId_ = 1;
    std::function<void(const Frame&)> onValueSet_;
    std::function<void()> onKeysChanged_;
    std::function<void(const Frame&, const Frame&, const Frame&)> onIncrementalKey_;
    std::unique_ptr<FlatState> flatState_;
    std::vector<Frame> cachedKeyFrames_;
    bool keyFramesCached_ = false;

    void triggerResync();
};

/// Manages all principals.
class PrincipalManager {
public:
    PrincipalManager() = default;

    PrincipalTX& principal(const std::string& name);
    bool has(const std::string& name) const;
    void remove(const std::string& name);
    size_t size() const { return principals_.size(); }
    std::vector<std::string> principalNames() const;

    void _setOnNewPrincipal(std::function<void(PrincipalTX&)> fn);
    void _addSession(const std::string& principal);
    bool _removeSession(const std::string& principal);
    bool _hasActiveSessions(const std::string& principal) const;

private:
    std::unordered_map<std::string, std::unique_ptr<PrincipalTX>> principals_;
    std::unordered_map<std::string, int> sessionCounts_;
    std::function<void(PrincipalTX&)> onNewPrincipal_;
};

} // namespace danws
