#include "danws/api/topic_client_handle.h"
#include <algorithm>

namespace danws {

TopicClientHandle::TopicClientHandle(const std::string& name, int index,
                                     KeyRegistry& registry,
                                     std::function<Payload(uint32_t)> storeGet)
    : name_(name), index_(index), registry_(registry), storeGet_(std::move(storeGet)) {}

Payload TopicClientHandle::get(const std::string& key) const {
    std::string wirePath = "t." + std::to_string(index_) + "." + key;
    const auto* entry = registry_.getByPath(wirePath);
    if (!entry) return std::monostate{};
    return storeGet_(entry->keyId);
}

std::vector<std::string> TopicClientHandle::keys() const {
    std::string prefix = "t." + std::to_string(index_) + ".";
    std::vector<std::string> result;
    for (const auto& path : registry_.paths()) {
        if (path.compare(0, prefix.size(), prefix) == 0) {
            result.push_back(path.substr(prefix.size()));
        }
    }
    return result;
}

std::function<void()> TopicClientHandle::onReceive(
    std::function<void(const std::string&, const Payload&)> cb) {
    onReceive_.push_back(std::move(cb));
    auto idx = onReceive_.size() - 1;
    auto* vec = &onReceive_;
    return [vec, idx]() {
        if (idx < vec->size()) {
            vec->erase(vec->begin() + static_cast<ptrdiff_t>(idx));
        }
    };
}

std::function<void()> TopicClientHandle::onUpdate(std::function<void()> cb) {
    onUpdate_.push_back(std::move(cb));
    auto idx = onUpdate_.size() - 1;
    auto* vec = &onUpdate_;
    return [vec, idx]() {
        if (idx < vec->size()) {
            vec->erase(vec->begin() + static_cast<ptrdiff_t>(idx));
        }
    };
}

void TopicClientHandle::_notify(const std::string& userKey, const Payload& value) {
    for (const auto& cb : onReceive_) {
        try { cb(userKey, value); } catch (...) {}
    }
    dirty_ = true;
}

void TopicClientHandle::_flushUpdate() {
    if (!dirty_ || onUpdate_.empty()) return;
    dirty_ = false;
    for (const auto& cb : onUpdate_) {
        try { cb(); } catch (...) {}
    }
}

void TopicClientHandle::_setIndex(int index) {
    index_ = index;
}

} // namespace danws
