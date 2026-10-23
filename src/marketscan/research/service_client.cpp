#include "marketscan/research/service_client.hpp"

#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "marketscan/research/endpoints.hpp"
#include "marketscan/util/logging.hpp"

#if defined(_WIN32)
#    include <windows.h>
//
#    include <winhttp.h>
#endif

namespace marketscan::research {
namespace {

using nlohmann::json;

/// An environment variable, or an empty string. Copied at once and the pointer
/// not kept, as live/credentials.cpp does.
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

[[nodiscard]] std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

#if defined(_WIN32)

/// UTF-8 to UTF-16, because every WinHTTP entry point takes wide strings.
[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

[[nodiscard]] std::string last_error(const char* what) {
    return std::string(what) + " failed (code " + std::to_string(::GetLastError()) + ")";
}

class WinHttpTransport final : public ServiceTransport {
public:
    WinHttpTransport(ServiceUrl url, std::string user_agent) : url_(std::move(url)) {
        session_ = ::WinHttpOpen(widen(user_agent).c_str(),
                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS,
                                 0);
        if (session_ == nullptr) {
            error_ = last_error("WinHttpOpen");
            return;
        }
        connection_ = ::WinHttpConnect(
            session_, widen(url_.host).c_str(), static_cast<INTERNET_PORT>(url_.port), 0);
        if (connection_ == nullptr) {
            error_ = last_error("WinHttpConnect");
        }
    }

    ~WinHttpTransport() override {
        if (connection_ != nullptr) {
            ::WinHttpCloseHandle(connection_);
        }
        if (session_ != nullptr) {
            ::WinHttpCloseHandle(session_);
        }
    }

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    ServiceResponse send(const ServiceRequest& request) override {
        ServiceResponse out;
        if (connection_ == nullptr) {
            out.error = error_.empty() ? "not connected" : error_;
            return out;
        }
        HINTERNET handle = ::WinHttpOpenRequest(connection_,
                                                widen(request.method).c_str(),
                                                widen(request.path).c_str(),
                                                nullptr,
                                                WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                url_.tls ? WINHTTP_FLAG_SECURE : 0);
        if (handle == nullptr) {
            out.error = last_error("WinHttpOpenRequest");
            return out;
        }
        const int timeout_ms = static_cast<int>(request.timeout.count() * 1000);
        ::WinHttpSetTimeouts(handle, 10'000, 10'000, 30'000, timeout_ms);

        // The key goes on the wire and nowhere else.
        std::wstring headers = L"Authorization: " + widen(request.authorization) + L"\r\n";
        if (!request.body.empty()) {
            headers += L"Content-Type: application/json\r\n";
        }
        const BOOL sent = ::WinHttpSendRequest(
            handle,
            headers.c_str(),
            static_cast<DWORD>(-1),
            request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
            static_cast<DWORD>(request.body.size()),
            static_cast<DWORD>(request.body.size()),
            0);
        if (sent == FALSE || ::WinHttpReceiveResponse(handle, nullptr) == FALSE) {
            out.error = last_error(sent == FALSE ? "WinHttpSendRequest" : "WinHttpReceiveResponse");
            ::WinHttpCloseHandle(handle);
            return out;
        }

        DWORD status = 0;
        DWORD size = sizeof(status);
        ::WinHttpQueryHeaders(handle,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX,
                              &status,
                              &size,
                              WINHTTP_NO_HEADER_INDEX);
        out.status = static_cast<int>(status);
        if (out.status == 429) {
            DWORD retry = 0;
            DWORD retry_size = sizeof(retry);
            if (::WinHttpQueryHeaders(handle,
                                      WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
                                      WINHTTP_HEADER_NAME_BY_INDEX,
                                      &retry,
                                      &retry_size,
                                      WINHTTP_NO_HEADER_INDEX) == TRUE) {
                out.retry_after_seconds = static_cast<int>(retry);
            }
            log::write(MS_LOG_WARN,
                       "service",
                       "rate limited by " + url_.host + "; asked to wait " +
                           std::to_string(out.retry_after_seconds) +
                           "s. Not retrying faster -- see docs/DATA_POLICY.md.");
        }

        // Only a successful response streams; an error's body is read whole so the
        // caller can show what the service said.
        const bool streaming = request.on_line != nullptr && out.status >= 200 && out.status < 300;
        std::string pending;
        for (;;) {
            DWORD available = 0;
            if (::WinHttpQueryDataAvailable(handle, &available) == FALSE) {
                out.error = last_error("WinHttpQueryDataAvailable");
                break;
            }
            if (available == 0) {
                break;
            }
            std::string& into = streaming ? pending : out.body;
            const std::size_t offset = into.size();
            into.resize(offset + available);
            DWORD read = 0;
            if (::WinHttpReadData(handle, into.data() + offset, available, &read) == FALSE) {
                out.error = last_error("WinHttpReadData");
                break;
            }
            into.resize(offset + read);
            if (read == 0) {
                break;
            }
            if (streaming && !detail::deliver_lines(pending, *request.on_line)) {
                out.cancelled = true;
                break;
            }
        }
        if (streaming && !out.cancelled && out.error.empty() && !pending.empty()) {
            // The last line, if the service did not end it with a newline.
            out.cancelled = !(*request.on_line)(pending);
        }
        ::WinHttpCloseHandle(handle);
        return out;
    }

private:
    ServiceUrl url_;
    std::string error_;
    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
};

#endif  // _WIN32

}  // namespace

bool detail::deliver_lines(std::string& pending, const LineHandler& on_line) {
    std::size_t start = 0;
    for (std::size_t newline = pending.find('\n', start); newline != std::string::npos;
         newline = pending.find('\n', start)) {
        std::string_view line(pending.data() + start, newline - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        start = newline + 1;
        if (!on_line(line)) {
            pending.erase(0, start);
            return false;
        }
    }
    pending.erase(0, start);
    return true;
}

ServiceUrl parse_service_url(std::string_view url) {
    ServiceUrl out;
    std::string_view rest = url;
    if (rest.substr(0, 8) == "https://") {
        out.tls = true;
        out.port = 443;
        rest.remove_prefix(8);
    } else if (rest.substr(0, 7) == "http://") {
        out.tls = false;
        out.port = 80;
        rest.remove_prefix(7);
    } else {
        throw std::invalid_argument("a service URL starts with https:// or http://");
    }
    if (!rest.empty() && rest.back() == '/') {
        rest.remove_suffix(1);
    }
    if (rest.find('/') != std::string_view::npos || rest.find('@') != std::string_view::npos ||
        rest.find('?') != std::string_view::npos) {
        throw std::invalid_argument("a service URL is a scheme, a host and a port, with no path");
    }
    const auto colon = rest.find(':');
    out.host = std::string(rest.substr(0, colon));
    if (colon != std::string_view::npos) {
        const std::string port(rest.substr(colon + 1));
        if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos ||
            port.size() > 5) {
            throw std::invalid_argument("a service URL's port is a number");
        }
        out.port = std::stoi(port);
        if (out.port < 1 || out.port > 65535) {
            throw std::invalid_argument("a service URL's port is between 1 and 65535");
        }
    }
    if (out.host.empty()) {
        throw std::invalid_argument("a service URL names a host");
    }
    return out;
}

std::unique_ptr<ServiceTransport> make_winhttp_transport(const ServiceUrl& url,
                                                         std::string user_agent) {
#if defined(_WIN32)
    return std::make_unique<WinHttpTransport>(url, std::move(user_agent));
#else
    (void)url;
    (void)user_agent;
    throw std::runtime_error("the service client is implemented for Windows only in this build");
#endif
}

ServiceClient::ServiceClient(const ServiceUrl& url,
                             std::string key,
                             std::unique_ptr<ServiceTransport> transport)
    : url_(url), transport_(std::move(transport)) {
    // The runtime half of SPEC_RESEARCH_VIEW.md 5.3: a host assembled at runtime
    // would pass every scan and stop here.
    if (!is_service_host(url_.host)) {
        throw std::invalid_argument("refusing to connect to '" + url_.host +
                                    "': not on the research allowlist "
                                    "(include/marketscan/research/endpoints.hpp)");
    }
    if (!url_.tls && !is_loopback_host(url_.host)) {
        throw std::invalid_argument("refusing plain HTTP to '" + url_.host +
                                    "': only this machine may be reached without TLS");
    }
    key = trimmed(std::move(key));
    if (key.empty()) {
        throw std::invalid_argument("the service key is empty");
    }
    if (!transport_) {
        throw std::invalid_argument("no transport");
    }
    authorization_ = "Bearer " + key;
}

ServiceResponse ServiceClient::get(std::string_view path) {
    ServiceRequest request;
    request.method = "GET";
    request.path = path;
    request.authorization = authorization_;
    return transport_->send(request);
}

ServiceResponse ServiceClient::post(std::string_view path, const json& body) {
    const std::string text = body.dump();
    ServiceRequest request;
    request.method = "POST";
    request.path = path;
    request.body = text;
    request.authorization = authorization_;
    return transport_->send(request);
}

ServiceResponse ServiceClient::stream(std::string_view path,
                                      const json& body,
                                      const std::function<bool(const json&)>& on_event,
                                      std::chrono::seconds timeout) {
    const std::string text = body.dump();
    std::size_t number = 0;
    std::string malformed;
    const LineHandler on_line = [&](std::string_view line) {
        ++number;
        if (line.find_first_not_of(" \t") == std::string_view::npos) {
            return true;  // a blank line carries no event
        }
        json event = json::parse(line, nullptr, false);
        if (event.is_discarded() || !event.is_object()) {
            // Its position, never its content: a line could carry anything.
            malformed = "line " + std::to_string(number) + " of the stream is not a JSON object";
            return false;
        }
        return on_event(event);
    };
    ServiceRequest request;
    request.method = "POST";
    request.path = path;
    request.body = text;
    request.authorization = authorization_;
    request.timeout = timeout;
    request.on_line = &on_line;
    ServiceResponse out = transport_->send(request);
    if (!malformed.empty()) {
        out.error = malformed;
        out.cancelled = false;
    }
    return out;
}

ServiceSetup connect_from_environment(
    std::string user_agent,
    const std::function<std::unique_ptr<ServiceTransport>(const ServiceUrl&)>& transport_for) {
    ServiceSetup out;
    const std::string key = trimmed(from_environment("MS_SERVICE_KEY"));
    if (key.empty()) {
        out.reason = "The assistant needs the job service's key: set MS_SERVICE_KEY. Until then, "
                     "fill in a form by hand; everything after the form works without it.";
        return out;
    }
    std::string url_text = trimmed(from_environment("MS_SERVICE_URL"));
    if (url_text.empty()) {
        url_text = "https://" + std::string(kServiceHost);
    }
    try {
        const ServiceUrl url = parse_service_url(url_text);
        std::unique_ptr<ServiceTransport> transport =
            transport_for ? transport_for(url) : make_winhttp_transport(url, std::move(user_agent));
        out.client = std::make_unique<ServiceClient>(url, key, std::move(transport));
    } catch (const std::exception& error) {
        // The URL may be shown -- it is not a secret -- but never the key.
        out.reason = std::string("MS_SERVICE_URL cannot be used: ") + error.what() + ".";
    }
    return out;
}

}  // namespace marketscan::research
