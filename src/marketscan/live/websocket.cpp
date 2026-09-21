#include "marketscan/live/websocket.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>

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

/// How long one underlying receive blocks before reporting silence.
///
/// This is set on the *request* handle, before the upgrade, because that is
/// the only handle WinHTTP will take it on. WINHTTP_OPTION_RECEIVE_TIMEOUT on
/// a WebSocket handle is accepted and then ignored, which is worse than being
/// rejected: a receive on a quiet socket blocks until the connection dies.
///
/// That cost a real debugging session. The market data socket is never quiet
/// during a session, so it never showed the bug; the trade-updates socket is
/// silent whenever no order is working, so *its* pump thread sat in a receive
/// that would not return and every shutdown that had to join it hung.
///
/// A caller's longer timeout is built by looping over these, which is why this
/// is small.
constexpr DWORD kPollTimeoutMs = 250;

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

#endif  // _WIN32

}  // namespace

struct WebSocket::Impl {
    SocketState state = SocketState::Closed;
    std::string error;
    std::uint64_t frames = 0;

    /// Guards the socket handle against a request_close() racing the owning
    /// thread's shutdown(). It does *not* guard receive(): a receive must not
    /// hold a lock that request_close() needs, or the two would deadlock in
    /// exactly the situation this exists to resolve.
    ///
    /// That is safe because the handle is only destroyed in shutdown(), which
    /// the owning thread calls after its receive has returned.
    std::mutex handle_mutex;

#if defined(_WIN32)
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET socket = nullptr;
#endif

    void shutdown() {
        const std::lock_guard<std::mutex> lock(handle_mutex);
#if defined(_WIN32)
        if (socket != nullptr) {
            ::WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            ::WinHttpCloseHandle(socket);
            socket = nullptr;
        }
        if (connection != nullptr) {
            ::WinHttpCloseHandle(connection);
            connection = nullptr;
        }
        if (session != nullptr) {
            ::WinHttpCloseHandle(session);
            session = nullptr;
        }
#endif
        state = SocketState::Closed;
    }

    ~Impl() { shutdown(); }
};

WebSocket::WebSocket() : impl_(std::make_unique<Impl>()) {}

WebSocket::~WebSocket() = default;

SocketState WebSocket::state() const noexcept {
    return impl_->state;
}

const std::string& WebSocket::error() const noexcept {
    return impl_->error;
}

std::uint64_t WebSocket::frames_received() const noexcept {
    return impl_->frames;
}

bool WebSocket::connect(const std::string& host,
                        const std::string& path,
                        const std::string& user_agent) {
    // Same runtime guard as HttpClient. A hostname assembled at runtime passes
    // both the source and the binary scan and fails here.
    if (!is_allowed_host(host)) {
        throw std::invalid_argument("refusing to open a socket to '" + host +
                                    "': not on the paper allowlist. See docs/DATA_POLICY.md.");
    }

    impl_->shutdown();
    impl_->error.clear();

#if defined(_WIN32)
    impl_->session = ::WinHttpOpen(widen(user_agent).c_str(),
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS,
                                   0);
    if (impl_->session == nullptr) {
        impl_->error = last_error("WinHttpOpen");
        impl_->state = SocketState::Failed;
        return false;
    }
    ::WinHttpSetTimeouts(impl_->session, 10'000, 10'000, 30'000, 30'000);

    impl_->connection =
        ::WinHttpConnect(impl_->session, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (impl_->connection == nullptr) {
        impl_->error = last_error("WinHttpConnect");
        impl_->state = SocketState::Failed;
        return false;
    }

    HINTERNET request = ::WinHttpOpenRequest(impl_->connection,
                                             L"GET",
                                             widen(path).c_str(),
                                             nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        impl_->error = last_error("WinHttpOpenRequest");
        impl_->state = SocketState::Failed;
        return false;
    }

    // The receive timeout goes on the request, before the upgrade. It is
    // inherited by the WebSocket handle, and it is the only place WinHTTP will
    // honour it -- see kPollTimeoutMs.
    ::WinHttpSetTimeouts(request, 10'000, 10'000, 30'000, static_cast<int>(kPollTimeoutMs));

    // Asking for the upgrade before sending is what turns this request into a
    // socket rather than a response body.
    if (::WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) == FALSE) {
        impl_->error = last_error("WinHttpSetOption(UPGRADE_TO_WEB_SOCKET)");
        ::WinHttpCloseHandle(request);
        impl_->state = SocketState::Failed;
        return false;
    }

    if (::WinHttpSendRequest(
            request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
        impl_->error = last_error("WinHttpSendRequest");
        ::WinHttpCloseHandle(request);
        impl_->state = SocketState::Failed;
        return false;
    }

    if (::WinHttpReceiveResponse(request, nullptr) == FALSE) {
        impl_->error = last_error("WinHttpReceiveResponse");
        ::WinHttpCloseHandle(request);
        impl_->state = SocketState::Failed;
        return false;
    }

    impl_->socket = ::WinHttpWebSocketCompleteUpgrade(request, 0);
    // The request handle is finished with either way: on success the socket
    // handle replaces it, and on failure there is nothing to keep.
    ::WinHttpCloseHandle(request);

    if (impl_->socket == nullptr) {
        impl_->error = last_error("WinHttpWebSocketCompleteUpgrade");
        impl_->state = SocketState::Failed;
        return false;
    }

    impl_->state = SocketState::Open;
    impl_->frames = 0;
    return true;
#else
    (void)path;
    (void)user_agent;
    impl_->error = "WebSocket is implemented for Windows only in this build";
    impl_->state = SocketState::Failed;
    return false;
#endif
}

bool WebSocket::send(std::string_view text) {
#if defined(_WIN32)
    if (impl_->state != SocketState::Open || impl_->socket == nullptr) {
        return false;
    }
    const DWORD result = ::WinHttpWebSocketSend(impl_->socket,
                                                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                                const_cast<char*>(text.data()),
                                                static_cast<DWORD>(text.size()));
    if (result != NO_ERROR) {
        impl_->error = "WinHttpWebSocketSend failed (code " + std::to_string(result) + ")";
        impl_->state = SocketState::Failed;
        return false;
    }
    return true;
#else
    (void)text;
    return false;
#endif
}

SocketMessage WebSocket::receive(int timeout_ms) {
    SocketMessage out;

#if defined(_WIN32)
    if (impl_->state != SocketState::Open || impl_->socket == nullptr) {
        out.closed = true;
        return out;
    }

    // The timeout is built by polling, because the underlying one is fixed at
    // connect time: see kPollTimeoutMs for why it cannot be changed per call.
    //
    // Elapsed time is measured rather than counted in polls, so a caller that
    // asks for 250ms waits 250ms whatever the poll interval happens to be.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);

    // A message can arrive in several fragments. They are concatenated here, so
    // what a caller sees is always one whole message.
    char chunk[8192];
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result =
            ::WinHttpWebSocketReceive(impl_->socket, chunk, sizeof(chunk), &read, &type);

        if (result == ERROR_WINHTTP_TIMEOUT) {
            // Silence, not failure. A partial message keeps waiting -- handing
            // back half of one would corrupt the stream -- but an empty result
            // past the deadline is just a quiet interval, which is ordinary.
            if (out.text.empty() && std::chrono::steady_clock::now() >= deadline) {
                return out;
            }
            if (std::chrono::steady_clock::now() >= deadline && !out.text.empty()) {
                // A fragment that never completed. Report the connection as
                // unusable rather than returning a truncated message: the
                // caller reconnects, which is recoverable, where a silently
                // half-parsed message is not.
                impl_->error = "a fragmented message did not complete before the timeout";
                impl_->state = SocketState::Failed;
                out.closed = true;
                return out;
            }
            continue;
        }
        if (result != NO_ERROR) {
            impl_->error = "WinHttpWebSocketReceive failed (code " + std::to_string(result) + ")";
            impl_->state = SocketState::Failed;
            out.closed = true;
            return out;
        }

        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            impl_->state = SocketState::Closed;
            out.closed = true;
            return out;
        }

        out.text.append(chunk, read);

        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            ++impl_->frames;
            out.ok = true;
            return out;
        }
        // Otherwise a FRAGMENT type: go round again for the rest of it.
    }
#else
    (void)timeout_ms;
    out.closed = true;
    return out;
#endif
}

void WebSocket::request_close() {
#if defined(_WIN32)
    const std::lock_guard<std::mutex> lock(impl_->handle_mutex);
    if (impl_->socket == nullptr) {
        return;
    }
    // Shutdown rather than close: the handle stays valid, so the thread
    // sitting in a receive is not left holding a destroyed one. The peer
    // answers with a close frame and that receive returns.
    ::WinHttpWebSocketShutdown(impl_->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
#endif
}

void WebSocket::close() {
    impl_->shutdown();
}

}  // namespace marketscan::live
