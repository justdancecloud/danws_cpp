#include "danws/api/principal_tx.h"

namespace danws {

// --- PrincipalTX ---

PrincipalTX::PrincipalTX(const std::string& name)
    : name_(name) {
    flatState_ = std::make_unique<FlatState>(FlatState::Callbacks{
        [this]() -> uint32_t { return nextKeyId_++; },
        [this](const Frame& f) {
            if (onValueSet_) onValueSet_(f);
        },
        [this]() { triggerResync(); },
        ""  // no wire prefix
    });
}

void PrincipalTX::set(const std::string& key, const Payload& value) {
    flatState_->set(key, value);
}

Payload PrincipalTX::get(const std::string& key) const {
    return flatState_->get(key);
}

std::vector<std::string> PrincipalTX::keys() const {
    return flatState_->keys();
}

void PrincipalTX::clear() {
    flatState_->clear();
    nextKeyId_ = 1;
}

void PrincipalTX::clear(const std::string& key) {
    flatState_->clear(key);
}

void PrincipalTX::_onValue(std::function<void(const Frame&)> fn) {
    onValueSet_ = std::move(fn);
}

void PrincipalTX::_onResync(std::function<void()> fn) {
    onKeysChanged_ = std::move(fn);
}

void PrincipalTX::_onIncremental(
    std::function<void(const Frame&, const Frame&, const Frame&)> fn) {
    onIncrementalKey_ = std::move(fn);
}

std::vector<Frame> PrincipalTX::_buildKeyFrames() {
    if (keyFramesCached_) return cachedKeyFrames_;

    cachedKeyFrames_ = flatState_->buildKeyFrames();
    // Always append ServerSync
    cachedKeyFrames_.push_back({
        FrameType::ServerSync, 0, DataType::Null, std::monostate{}
    });
    keyFramesCached_ = true;
    return cachedKeyFrames_;
}

std::vector<Frame> PrincipalTX::_buildValueFrames() {
    return flatState_->buildValueFrames();
}

void PrincipalTX::triggerResync() {
    keyFramesCached_ = false;
    cachedKeyFrames_.clear();
    if (onKeysChanged_) onKeysChanged_();
}

// --- PrincipalManager ---

PrincipalTX& PrincipalManager::principal(const std::string& name) {
    auto it = principals_.find(name);
    if (it != principals_.end()) return *it->second;

    auto ptx = std::make_unique<PrincipalTX>(name);
    auto& ref = *ptx;
    principals_[name] = std::move(ptx);
    if (onNewPrincipal_) onNewPrincipal_(ref);
    return ref;
}

bool PrincipalManager::has(const std::string& name) const {
    return principals_.count(name) > 0;
}

void PrincipalManager::remove(const std::string& name) {
    principals_.erase(name);
    sessionCounts_.erase(name);
}

std::vector<std::string> PrincipalManager::principalNames() const {
    std::vector<std::string> result;
    for (const auto& [name, count] : sessionCounts_) {
        if (count > 0) result.push_back(name);
    }
    return result;
}

void PrincipalManager::_setOnNewPrincipal(std::function<void(PrincipalTX&)> fn) {
    onNewPrincipal_ = std::move(fn);
}

void PrincipalManager::_addSession(const std::string& principal) {
    sessionCounts_[principal]++;
}

bool PrincipalManager::_removeSession(const std::string& principal) {
    auto it = sessionCounts_.find(principal);
    if (it == sessionCounts_.end()) return true;
    it->second--;
    if (it->second <= 0) {
        sessionCounts_.erase(it);
        return true;
    }
    return false;
}

bool PrincipalManager::_hasActiveSessions(const std::string& principal) const {
    auto it = sessionCounts_.find(principal);
    return it != sessionCounts_.end() && it->second > 0;
}

} // namespace danws
