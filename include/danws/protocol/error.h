#pragma once

#include <stdexcept>
#include <string>

namespace danws {

/// Exception type for all DanProtocol errors.
class DanWSError : public std::runtime_error {
public:
    DanWSError(const std::string& code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

} // namespace danws
