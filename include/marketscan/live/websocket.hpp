#ifndef MARKETSCAN_LIVE_WEBSOCKET_HPP
#define MARKETSCAN_LIVE_WEBSOCKET_HPP

/*
 * A WebSocket client, over the same WinHTTP the REST client uses.
 *
 * Blocking, single-threaded, and deliberately so: the live feed owns a thread of
 * its own and pumps this from it, which is simpler to reason about than a
 * callback API and much simpler to make deterministic at the seam where events
 * enter the engine.
 *
 * RECONNECTION IS THE CALLER'S BUSINESS
 *
 * This reports that the connection is gone. It does not decide what to do about
 * it, because the policy -- backoff, cap, whether to resubscribe, whether to
 * treat a gap as a gap -- belongs to the thing that knows what the stream is
 * for. See LiveMarketDataFeed.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/live/credentials.hpp"

namespace marketscan::live {

enum class SocketState : std::uint8_t {
    Closed = 0,
    Open = 1,
    Failed = 2,
};

struct SocketMessage {
    /// The payload, as text. Empty with `ok == false` means nothing arrived
    /// before the timeout, which is ordinary rather than an error.
    std::string text;
    bool ok = false;

    /// Set when the connection is gone and will not recover on its own.
    bool closed = false;
};

/// One WebSocket connection.
class WebSocket {
public:
    WebSocket();
    ~WebSocket();

    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    /// Opens a connection. `host` must be on the paper allowlist; anything else
    /// throws, exactly as HttpClient does, so a runtime-assembled hostname
    /// cannot slip past the source and binary scans.
    ///
    /// Returns false on failure, with `error()` saying why.
    [[nodiscard]] bool
    connect(const std::string& host, const std::string& path, const std::string& user_agent);

    /// Sends one text frame.
    [[nodiscard]] bool send(std::string_view text);

    /// Waits up to `timeout_ms` for one message.
    ///
    /// A timeout is not a failure: a quiet market is quiet, and treating silence
    /// as a dropped connection is how a feed reconnects itself to death.
    [[nodiscard]] SocketMessage receive(int timeout_ms);

    void close();

    /// Unblocks a receive that is happening on another thread.
    ///
    /// WHY THIS EXISTS, AND WHY A TIMEOUT DOES NOT
    ///
    /// WinHTTP's synchronous WebSocket receive does not honour a timeout. Not
    /// the session's, not the request's, and not WINHTTP_OPTION_RECEIVE_TIMEOUT
    /// on the socket -- that last one is accepted and then ignored, which is
    /// worse than being refused. A receive on a socket that has nothing to say
    /// blocks until the connection dies.
    ///
    /// This was measured, not assumed: a probe held two authenticated sockets
    /// open with nothing subscribed and sat in the first receive for seven
    /// minutes with a thirty-second timeout configured. The market data socket
    /// never showed it, because a subscribed socket is never quiet; the
    /// trade-updates socket is silent whenever no order is working, so its pump
    /// thread could not be joined and every shutdown that had to join it hung.
    ///
    /// So the receive is ended by ending the conversation: this sends a close
    /// frame, the peer answers, and the blocked receive returns with `closed`
    /// set. Safe to call from any thread and safe to call twice.
    void request_close();

    [[nodiscard]] SocketState state() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;

    /// Frames received since the connection opened.
    [[nodiscard]] std::uint64_t frames_received() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_WEBSOCKET_HPP
