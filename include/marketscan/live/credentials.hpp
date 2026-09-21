#ifndef MARKETSCAN_LIVE_CREDENTIALS_HPP
#define MARKETSCAN_LIVE_CREDENTIALS_HPP

/*
 * Broker credentials, from the environment and from nowhere else.
 *
 * No file, no config key, no command-line argument, no default. A credential
 * that can come from a file is a credential that ends up in a repository, and
 * this project's gitignore entries were written before the first key existed
 * precisely so that the answer was already settled.
 *
 * ABSENT CREDENTIALS ARE AN ERROR, NOT A FALLBACK
 *
 * A missing key produces a diagnostic naming the variable that is missing. It
 * does not quietly fall back to a simulator, to a read-only mode, or to a
 * different account. The failure mode being avoided is a run that looks live,
 * reports numbers, and was never connected to anything -- which is the same
 * class of bug as a test that passes without measuring.
 */

#include <string>

namespace marketscan::live {

/// The environment variables read. Alpaca's own names, so a key already exported
/// for their tooling works here without being copied.
inline constexpr const char* kKeyIdVariable = "APCA_API_KEY_ID";
inline constexpr const char* kSecretVariable = "APCA_API_SECRET_KEY";

/// A key pair, or the reason there isn't one.
struct Credentials {
    std::string key_id;
    std::string secret;

    /// Empty when the credentials loaded. Otherwise names what is missing.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Reads the credentials from the environment.
///
/// Never logs, prints or returns the secret in `error`. The diagnostic names the
/// variable, never its value: a secret that reaches a log has leaked, and a log
/// is the easiest place in a system to forget about.
[[nodiscard]] Credentials load_credentials();

/// A form safe to log: the key id's first four characters and nothing else.
///
/// Enough to tell two accounts apart in a log, not enough to be one.
[[nodiscard]] std::string redact(const std::string& value);

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_CREDENTIALS_HPP
