#ifndef MARKETSCAN_LIVE_HTTP_HPP
#define MARKETSCAN_LIVE_HTTP_HPP

/*
 * The smallest HTTPS client that does this job.
 *
 * WinHTTP, which is in the operating system, rather than a vendored client.
 * Adding a dependency to a frozen-ABI C++ project is a decision with a long
 * tail -- a vcpkg port to pin, a baseline to bump, a transitive OpenSSL, a CI
 * cache to grow -- and the engine is Windows-only already. WinHTTP also speaks
 * WebSocket, so one API covers both halves of live execution.
 *
 * PAPER ONLY, ENFORCED HERE TOO
 *
 * The constructor refuses a host that is not on the allowlist in endpoints.hpp.
 * That is the third place the rule is enforced and the only one that acts at
 * runtime: the source scan and the binary scan both check that a live host is
 * not *present*, and this checks that one is not *reached*. A string assembled
 * at runtime would pass both scans and fail here.
 *
 * RATE LIMITS ARE OBEYED, NOT EVADED
 *
 * A 429 is returned to the caller with its Retry-After intact. Nothing here
 * retries faster, opens a second connection, or rotates anything --
 * docs/DATA_POLICY.md §1 and §7 govern a broker exactly as they govern a data
 * vendor.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/live/credentials.hpp"

namespace marketscan::live {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpResponse {
    /// HTTP status, or zero when the request never got that far.
    int status = 0;
    std::string body;

    /// Transport-level failure. Empty when a response arrived at all, whatever
    /// its status -- a 500 is an answer, a dropped connection is not.
    std::string error;

    /// Seconds the server asked us to wait, from Retry-After. Zero if absent.
    int retry_after_seconds = 0;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && status >= 200 && status < 300;
    }

    /// The server asking for less traffic. Honoured, never worked around.
    [[nodiscard]] bool rate_limited() const noexcept { return status == 429; }

    /// The credentials were refused. Distinguished from every other failure
    /// because the remedy is different and the message should say so.
    [[nodiscard]] bool unauthorised() const noexcept { return status == 401 || status == 403; }
};

/// One connection to one host.
class HttpClient {
public:
    /// Throws std::invalid_argument if `host` is not on the paper allowlist.
    HttpClient(std::string host, Credentials credentials, std::string user_agent);
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;

    [[nodiscard]] const std::string& host() const noexcept;

    /// True once the session and connection handles exist. False means the
    /// constructor failed, and `error()` says why.
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] const std::string& error() const noexcept;

    [[nodiscard]] HttpResponse get(std::string_view path);
    [[nodiscard]] HttpResponse post(std::string_view path, std::string_view body);
    [[nodiscard]] HttpResponse del(std::string_view path);

    /// Requests made, for a report that wants to say how much traffic a run
    /// generated. A courtesy to whoever is being asked for data.
    [[nodiscard]] std::uint64_t request_count() const noexcept;

    /// Times the server answered 429.
    [[nodiscard]] std::uint64_t rate_limited_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[nodiscard]] HttpResponse
    send(std::string_view verb, std::string_view path, std::string_view body);
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_HTTP_HPP
