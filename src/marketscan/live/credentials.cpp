#include "marketscan/live/credentials.hpp"

#include <cstdlib>
#include <string>

namespace marketscan::live {
namespace {

/// Reads an environment variable, or an empty string.
///
/// `getenv` rather than `getenv_s`: the value is copied immediately and the
/// pointer is not retained, which is the constraint the safe variant exists to
/// enforce.
[[nodiscard]] std::string from_environment(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

}  // namespace

Credentials load_credentials() {
    Credentials out;
    out.key_id = from_environment(kKeyIdVariable);
    out.secret = from_environment(kSecretVariable);

    // Both named when both are missing, so a first-time reader is told the whole
    // requirement rather than discovering it one variable at a time.
    if (out.key_id.empty() && out.secret.empty()) {
        out.error = std::string("no broker credentials: set ") + kKeyIdVariable + " and " +
                    kSecretVariable +
                    ". They are read from the environment only -- never from a file in this "
                    "repository. See docs/DATA_POLICY.md.";
        return out;
    }
    if (out.key_id.empty()) {
        out.error = std::string(kKeyIdVariable) + " is not set (" + kSecretVariable + " is)";
        return out;
    }
    if (out.secret.empty()) {
        out.error = std::string(kSecretVariable) + " is not set (" + kKeyIdVariable + " is)";
        return out;
    }
    return out;
}

std::string redact(const std::string& value) {
    if (value.empty()) {
        return "(unset)";
    }
    if (value.size() <= 4) {
        // Too short to show any of: whatever this is, it is not a key, and
        // showing half of a four-character string shows most of it.
        return "****";
    }
    return value.substr(0, 4) + std::string(value.size() - 4, '*');
}

}  // namespace marketscan::live
