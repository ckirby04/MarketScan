#include "marketscan/live/http.hpp"

#include <stdexcept>
#include <utility>

#include "marketscan/live/endpoints.hpp"
#include "marketscan/util/logging.hpp"

#if defined(_WIN32)
#    include <windows.h>
//
#    include <winhttp.h>
#endif

namespace marketscan::live {
namespace {

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

[[nodiscard]] std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8,
                          0,
                          text.data(),
                          static_cast<int>(text.size()),
                          out.data(),
                          needed,
                          nullptr,
                          nullptr);
    return out;
}

[[nodiscard]] std::string last_error(const char* what) {
    return std::string(what) + " failed (code " + std::to_string(::GetLastError()) + ")";
}

#endif  // _WIN32

}  // namespace

struct HttpClient::Impl {
    std::string host;
    std::string user_agent;
    std::string error;
    Credentials credentials;
    std::uint64_t requests = 0;
    std::uint64_t rate_limited = 0;

#if defined(_WIN32)
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
#endif

    ~Impl() {
#if defined(_WIN32)
        if (connection != nullptr) {
            ::WinHttpCloseHandle(connection);
        }
        if (session != nullptr) {
            ::WinHttpCloseHandle(session);
        }
#endif
    }
};

HttpClient::HttpClient(std::string host, Credentials credentials, std::string user_agent)
    : impl_(std::make_unique<Impl>()) {
    // The runtime half of the paper-only rule. A host assembled at runtime would
    // pass both the source scan and the binary scan and fail right here.
    if (!is_allowed_host(host)) {
        throw std::invalid_argument("refusing to connect to '" + host +
                                    "': not on the paper allowlist. This project is paper-only "
                                    "by construction; see docs/DATA_POLICY.md.");
    }

    impl_->host = std::move(host);
    impl_->user_agent = std::move(user_agent);
    impl_->credentials = std::move(credentials);

#if defined(_WIN32)
    impl_->session = ::WinHttpOpen(widen(impl_->user_agent).c_str(),
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS,
                                   0);
    if (impl_->session == nullptr) {
        impl_->error = last_error("WinHttpOpen");
        return;
    }

    // Generous but finite. A live feed that hangs forever is worse than one that
    // reports a timeout and reconnects.
    ::WinHttpSetTimeouts(impl_->session, 10'000, 10'000, 30'000, 30'000);

    impl_->connection = ::WinHttpConnect(
        impl_->session, widen(impl_->host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (impl_->connection == nullptr) {
        impl_->error = last_error("WinHttpConnect");
    }
#else
    impl_->error = "HTTPS is implemented for Windows only in this build";
#endif
}

HttpClient::~HttpClient() = default;
HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

const std::string& HttpClient::host() const noexcept {
    return impl_->host;
}

bool HttpClient::connected() const noexcept {
#if defined(_WIN32)
    return impl_->connection != nullptr;
#else
    return false;
#endif
}

const std::string& HttpClient::error() const noexcept {
    return impl_->error;
}

std::uint64_t HttpClient::request_count() const noexcept {
    return impl_->requests;
}

std::uint64_t HttpClient::rate_limited_count() const noexcept {
    return impl_->rate_limited;
}

HttpResponse HttpClient::get(std::string_view path) {
    return send("GET", path, {});
}

HttpResponse HttpClient::post(std::string_view path, std::string_view body) {
    return send("POST", path, body);
}

HttpResponse HttpClient::del(std::string_view path) {
    return send("DELETE", path, {});
}

HttpResponse HttpClient::send(std::string_view verb, std::string_view path, std::string_view body) {
    HttpResponse out;

#if defined(_WIN32)
    if (!connected()) {
        out.error = impl_->error.empty() ? "not connected" : impl_->error;
        return out;
    }
    ++impl_->requests;

    HINTERNET request = ::WinHttpOpenRequest(impl_->connection,
                                             widen(verb).c_str(),
                                             widen(path).c_str(),
                                             nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        out.error = last_error("WinHttpOpenRequest");
        return out;
    }

    // Credentials go on the wire and nowhere else: not into a log line, not into
    // an error message, not into the response this returns.
    std::wstring headers = L"APCA-API-KEY-ID: " + widen(impl_->credentials.key_id) + L"\r\n" +
                           L"APCA-API-SECRET-KEY: " + widen(impl_->credentials.secret) + L"\r\n";
    if (!body.empty()) {
        headers += L"Content-Type: application/json\r\n";
    }

    const BOOL sent = ::WinHttpSendRequest(request,
                                           headers.c_str(),
                                           static_cast<DWORD>(-1),
                                           body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                        : const_cast<char*>(body.data()),
                                           static_cast<DWORD>(body.size()),
                                           static_cast<DWORD>(body.size()),
                                           0);
    if (sent == FALSE) {
        out.error = last_error("WinHttpSendRequest");
        ::WinHttpCloseHandle(request);
        return out;
    }

    if (::WinHttpReceiveResponse(request, nullptr) == FALSE) {
        out.error = last_error("WinHttpReceiveResponse");
        ::WinHttpCloseHandle(request);
        return out;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX,
                          &status,
                          &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);

    if (out.status == 429) {
        ++impl_->rate_limited;
        DWORD retry = 0;
        DWORD retry_size = sizeof(retry);
        if (::WinHttpQueryHeaders(request,
                                  WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX,
                                  &retry,
                                  &retry_size,
                                  WINHTTP_NO_HEADER_INDEX) == TRUE) {
            out.retry_after_seconds = static_cast<int>(retry);
        }
        log::write(MS_LOG_WARN,
                   "http",
                   "rate limited by " + impl_->host + "; waiting " +
                       std::to_string(out.retry_after_seconds) +
                       "s. Not retrying faster -- see docs/DATA_POLICY.md.");
    }

    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(request, &available) == FALSE) {
            out.error = last_error("WinHttpQueryDataAvailable");
            break;
        }
        if (available == 0) {
            break;
        }
        const std::size_t offset = out.body.size();
        out.body.resize(offset + available);
        DWORD read = 0;
        if (::WinHttpReadData(request, out.body.data() + offset, available, &read) == FALSE) {
            out.error = last_error("WinHttpReadData");
            break;
        }
        out.body.resize(offset + read);
        if (read == 0) {
            break;
        }
    }

    ::WinHttpCloseHandle(request);
#else
    (void)verb;
    (void)path;
    (void)body;
    out.error = "HTTPS is implemented for Windows only in this build";
#endif
    return out;
}

}  // namespace marketscan::live
