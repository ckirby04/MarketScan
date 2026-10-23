#ifndef MARKETSCAN_RESEARCH_SERVICE_CLIENT_HPP
#define MARKETSCAN_RESEARCH_SERVICE_CLIENT_HPP

/*
 * The research path's one way onto the network: the job service.
 *
 * docs/specs/SPEC_DESKTOP_CHAT.md section 3. The desktop Research view's
 * conversation goes through the service the web app uses, and this is the
 * client it uses to get there.
 *
 * NOT live/http.hpp
 *
 * SPEC_RESEARCH_VIEW.md section 2 keeps the research path from linking live/,
 * so a broker cannot be reached from here however the code is driven. This is
 * its own library (marketscan_service), built on WinHTTP like the broker
 * client, with its own allowlist (research/endpoints.hpp) and its own refusal.
 * ms_research links neither; tests/test_research_job.cpp reads its binary to be
 * sure.
 *
 * THE RULES IT KEEPS
 *
 * - A host not on the research allowlist is refused at construction, and so is
 *   plain HTTP to anything but this machine.
 * - The key goes on the wire as a bearer token and nowhere else: not into a log
 *   line, an error, or anything this returns.
 * - A 429 comes back with its Retry-After. Nothing here retries around it
 *   (docs/DATA_POLICY.md).
 * - A conversation turn streams NDJSON for up to five minutes, so a stream
 *   delivers each line as it arrives and can be cancelled from the callback.
 *
 * Every request goes through a ServiceTransport. The real one is WinHTTP;
 * tests hand in a fake, so no test touches the network (SPEC_RESEARCH_VIEW.md
 * section 8).
 */

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace marketscan::research {

/// Where the service is: scheme, host and port, and nothing else.
struct ServiceUrl {
    bool tls = true;
    std::string host;
    int port = 443;
};

/// Parses "https://host", "https://host:port" or "http://host:port", with an
/// optional trailing slash and no path. Throws std::invalid_argument otherwise.
[[nodiscard]] ServiceUrl parse_service_url(std::string_view url);

struct ServiceResponse {
    /// HTTP status, or zero when the request never got that far.
    int status = 0;
    /// The body, for a request that was not streamed.
    std::string body;
    /// Transport-level failure. Empty when a response arrived at all, whatever
    /// its status.
    std::string error;
    /// Seconds the service asked us to wait, from Retry-After. Zero if absent.
    int retry_after_seconds = 0;
    /// The stream's callback asked to stop.
    bool cancelled = false;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && status >= 200 && status < 300;
    }

    [[nodiscard]] bool rate_limited() const noexcept { return status == 429; }

    [[nodiscard]] bool unauthorised() const noexcept { return status == 401 || status == 403; }
};

/// Called with each line of a streamed body, without its newline. Returning
/// false stops reading and closes the request.
using LineHandler = std::function<bool(std::string_view line)>;

namespace detail {
/// Hands each complete line in `pending` to `on_line`, without its newline or
/// a trailing carriage return, and keeps the incomplete tail for the next
/// chunk: a chunk off the network can end anywhere. False when the handler
/// asked to stop.
[[nodiscard]] bool deliver_lines(std::string& pending, const LineHandler& on_line);
}  // namespace detail

/// One request. The seam the tests replace.
struct ServiceRequest {
    std::string_view method;
    std::string_view path;
    std::string_view body;  // JSON, or empty
    /// The full header value, "Bearer <key>". Passed down so a transport never
    /// stores it.
    std::string_view authorization;
    std::chrono::seconds timeout{30};
    /// When set, the body is delivered here line by line rather than returned.
    const LineHandler* on_line = nullptr;
};

class ServiceTransport {
public:
    virtual ~ServiceTransport() = default;
    [[nodiscard]] virtual ServiceResponse send(const ServiceRequest& request) = 0;
};

/// WinHTTP to `url`. The caller has already checked the host.
[[nodiscard]] std::unique_ptr<ServiceTransport> make_winhttp_transport(const ServiceUrl& url,
                                                                       std::string user_agent);

class ServiceClient {
public:
    /// How long a conversation turn may stream: the web route's maxDuration.
    static constexpr std::chrono::seconds kTurnTimeout{300};

    /// Throws std::invalid_argument when `url`'s host is not on the research
    /// allowlist, when it is plain HTTP to anything but this machine, or when the
    /// key is empty.
    ServiceClient(const ServiceUrl& url,
                  std::string key,
                  std::unique_ptr<ServiceTransport> transport);

    ServiceClient(const ServiceClient&) = delete;
    ServiceClient& operator=(const ServiceClient&) = delete;

    [[nodiscard]] const std::string& host() const noexcept { return url_.host; }

    [[nodiscard]] ServiceResponse get(std::string_view path);
    [[nodiscard]] ServiceResponse post(std::string_view path, const nlohmann::json& body);

    /// POSTs `body` and hands each NDJSON line to `on_event` as a parsed object.
    /// A line that is not JSON ends the stream with an error naming its position,
    /// never its content. `on_event` returning false cancels.
    [[nodiscard]] ServiceResponse stream(std::string_view path,
                                         const nlohmann::json& body,
                                         const std::function<bool(const nlohmann::json&)>& on_event,
                                         std::chrono::seconds timeout = kTurnTimeout);

private:
    ServiceUrl url_;
    std::string authorization_;  // "Bearer <key>"; never logged, never returned
    std::unique_ptr<ServiceTransport> transport_;
};

/// A client from the environment, or why there is none.
struct ServiceSetup {
    std::unique_ptr<ServiceClient> client;
    /// Empty when `client` is set. Otherwise one sentence for the view, naming
    /// the variable to set and never its value.
    std::string reason;
};

/// The service URL from MS_SERVICE_URL (default https://kServiceHost) and the key
/// from MS_SERVICE_KEY. `transport_for` makes the transport; the default is
/// WinHTTP, and tests pass a fake.
[[nodiscard]] ServiceSetup connect_from_environment(
    std::string user_agent,
    const std::function<std::unique_ptr<ServiceTransport>(const ServiceUrl&)>& transport_for = {});

}  // namespace marketscan::research

#endif  // MARKETSCAN_RESEARCH_SERVICE_CLIENT_HPP
