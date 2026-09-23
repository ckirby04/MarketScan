#ifndef MARKETSCAN_LIVE_LIVE_FEED_HPP
#define MARKETSCAN_LIVE_LIVE_FEED_HPP

/*
 * A live market data feed, behind the same IMarketDataFeed every replay uses.
 *
 * THE SEAM
 *
 * This is where "only the clock and the transport change" is either true or a
 * comment. The engine does not know this class exists. It holds an
 * IEventSource, calls next(), and gets an event or a false. What is different
 * from a CsvFeed is that false here means "nothing right now" rather than "end
 * of stream", and that is already in the interface's contract -- IEventSource
 * says so in engine.hpp, and said so before there was a live feed to need it.
 *
 * THREADS
 *
 * One thread owns the socket. It reads, parses, and pushes events into an
 * SpscRingBuffer. The engine thread pops. The SymbolTable is interned once, on
 * the constructing thread, before the socket thread starts; after that the
 * parser only reads it. Nothing else is shared.
 *
 * RECONNECTION
 *
 * Capped exponential backoff, and the cap matters more than the curve: a feed
 * that reconnects as fast as it can when the other end is unhappy is a feed
 * that gets its credentials suspended. docs/DATA_POLICY.md section 1 applies
 * to a socket exactly as it applies to a fetch. Every attempt is logged, and a
 * recorder is told about the gap so a replay can see it rather than reading
 * straight across it.
 *
 * CLOCK DISCIPLINE
 *
 * Every event carries an exchange timestamp. This thread also knows when the
 * message arrived here. The difference is skew -- transport plus clock offset,
 * not separable without a time source this machine does not have -- and it is
 * measured and reported rather than assumed away. Events keep their exchange
 * timestamps; the skew is a statistic about the run, not a correction applied
 * to the data.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "marketscan/core/ring_buffer.hpp"
#include "marketscan/data/feed.hpp"
#include "marketscan/live/credentials.hpp"
#include "marketscan/live/recorder.hpp"
#include "marketscan/live/stream_parser.hpp"
#include "marketscan/live/websocket.hpp"

namespace marketscan::live {

enum class FeedState : std::uint8_t {
    /// Constructed, thread not started.
    Idle = 0,
    Connecting = 1,
    /// Socket open, handshake done, subscription confirmed.
    Streaming = 2,
    /// Dropped; waiting out the backoff before trying again.
    Reconnecting = 3,
    /// Stopped by the caller, or failed in a way retrying will not fix --
    /// rejected credentials, a refused subscription.
    Stopped = 4,
};

[[nodiscard]] std::string_view to_string(FeedState state) noexcept;

struct LiveFeedConfig {
    /// Tickers to subscribe to. Interned before the socket thread starts.
    std::vector<std::string> symbols;

    bool subscribe_trades = true;
    bool subscribe_quotes = true;
    bool subscribe_bars = true;

    /// Events buffered between the socket thread and the engine thread.
    /// Rounded up to a power of two. A full buffer drops the newest event and
    /// increments dropped_events -- loudly, never silently.
    std::size_t queue_capacity = 1u << 16;

    /// First backoff, then doubling to the cap. Both in milliseconds.
    int reconnect_initial_ms = 1000;
    int reconnect_max_ms = 60000;

    /// Give up after this many consecutive failures. Zero means never.
    std::uint32_t max_reconnect_attempts = 0;

    /// Where to write the recording. Empty means do not record.
    ///
    /// Only consulted when `recorder` is null: a caller that wants the venue
    /// side in the same file passes a recorder both the feed and the broker
    /// hold, and this is then irrelevant.
    std::filesystem::path recording_path;

    /// A recorder shared with the broker, so market data, orders out and
    /// answers back land in one file in one arrival order.
    ///
    /// When this is set the feed does not own it and will not close it -- the
    /// broker may still be writing after the feed has stopped, which is exactly
    /// what happens when a session ends by cancelling everything.
    std::shared_ptr<StreamRecorder> recorder;

    std::string user_agent = "MarketScan/0.1 (research tool)";
};

/// A snapshot of the feed, safe to read from another thread.
///
/// The GUI reads this every frame to draw the connection state. Plain atomics
/// copied into a struct rather than a lock, because a status panel that can
/// block the socket thread is worse than a status panel that is one frame out
/// of date.
struct LiveFeedStats {
    FeedState state = FeedState::Idle;

    std::uint64_t messages = 0;
    std::uint64_t events_parsed = 0;
    std::uint64_t events_delivered = 0;

    /// Events the ring buffer had no room for. Any value above zero means the
    /// engine thread could not keep up and the run saw less than the market
    /// did. Reported, never absorbed.
    std::uint64_t dropped_events = 0;

    std::uint64_t unknown_symbol = 0;
    std::uint64_t malformed = 0;
    std::uint64_t unhandled_kind = 0;

    /// Trades and quotes whose exchange timestamp arrived behind their
    /// predecessor's. Transport jitter.
    std::uint64_t reordered = 0;
    std::int64_t max_backwards_ns = 0;

    /// Bars, which are stamped at the start of their interval and cannot be
    /// sent until it ends. Counted apart from the line above because mixing
    /// them produces a number dominated by the bar interval that describes
    /// neither -- see MonotonicGate.
    std::uint64_t bars_interval_stamped = 0;
    std::int64_t max_bar_lag_ns = 0;

    std::uint32_t reconnects = 0;
    std::uint32_t consecutive_failures = 0;

    /// Local clock minus exchange timestamp, in nanoseconds, over the trades
    /// and quotes seen so far. Transport plus clock offset together; this
    /// machine has no reference good enough to separate them, and pretending
    /// otherwise would be a made-up number.
    ///
    /// Bars are excluded. A bar's timestamp is the start of its interval, so
    /// including one adds a whole bar interval to a statistic that is supposed
    /// to describe the network -- and a single minute bar is enough to swamp
    /// thousands of quotes.
    std::int64_t skew_last_ns = 0;
    std::int64_t skew_min_ns = 0;
    std::int64_t skew_max_ns = 0;
    std::int64_t skew_mean_ns = 0;
    std::uint64_t skew_samples = 0;

    /// Non-empty when the feed stopped for a reason retrying will not fix.
    std::string error;
};

/// The live feed.
class LiveMarketDataFeed final : public IMarketDataFeed {
public:
    /// Interns `config.symbols` into `symbols` on the calling thread. Throws
    /// std::invalid_argument if no symbols were given or the credentials are
    /// missing; the host allowlist is enforced by the WebSocket it opens.
    LiveMarketDataFeed(LiveFeedConfig config, Credentials credentials, SymbolTable& symbols);
    ~LiveMarketDataFeed() override;

    /// Starts the socket thread. Returns immediately; the feed reports
    /// Connecting until the handshake completes.
    void start();

    /// Signals the socket thread to stop and joins it.
    void stop();

    /// Blocks until the feed reaches Streaming or stops. Returns true if it
    /// reached Streaming. For a runner that wants to know the subscription
    /// took before it starts the engine.
    [[nodiscard]] bool wait_until_streaming(int timeout_ms);

    /// Asks the pump to drop its connection at the next opportunity, so the
    /// reconnect path can be exercised without unplugging anything.
    ///
    /// A flag rather than closing the handle from here: the socket thread is
    /// usually inside a blocking receive, and tearing a WinHTTP handle out
    /// from under it would be testing the operating system's tolerance rather
    /// than this feed's reconnect logic. The pump notices within one receive
    /// timeout and then takes exactly the path a real drop takes.
    void request_drop_for_test();

    // --- IEventSource -----------------------------------------------------

    /// Pops one buffered event. False means nothing right now, which for a
    /// live source is ordinary and is not end of stream.
    bool next(MarketEvent& out) override;

    /// False. There is no beginning to rewind to, and the transport panel uses
    /// this to refuse to offer a scrub bar for something that has not happened
    /// yet.
    [[nodiscard]] bool can_reset() const noexcept override { return false; }

    [[nodiscard]] std::string_view name() const noexcept override { return "live"; }

    // --- IMarketDataFeed --------------------------------------------------

    [[nodiscard]] const std::vector<SymbolId>& symbols() const noexcept override {
        return symbol_ids_;
    }

    /// `known` stays false: a live feed has no last timestamp.
    [[nodiscard]] FeedSpan span() const noexcept override;

    // --- Status -----------------------------------------------------------

    [[nodiscard]] LiveFeedStats stats() const;

    [[nodiscard]] FeedState state() const noexcept;

    /// Messages recorded, or zero when not recording.
    [[nodiscard]] std::uint64_t recorded_messages() const noexcept;

private:
    struct Pump;

    void run_socket();

    void publish(const ParseResult& parsed, Timestamp received);

    void set_error(std::string message);

    LiveFeedConfig config_;
    Credentials credentials_;
    SymbolTable& symbol_table_;
    std::vector<SymbolId> symbol_ids_;

    SpscRingBuffer<MarketEvent> queue_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> drop_requested_{false};
    std::atomic<FeedState> state_{FeedState::Idle};

    /// Written by the socket thread, read by anyone. Individually atomic so a
    /// reader never blocks the socket thread; a stats() snapshot can therefore
    /// be a nanosecond inconsistent with itself, which a status panel does not
    /// care about and nothing else reads.
    std::atomic<std::uint64_t> messages_{0};
    std::atomic<std::uint64_t> events_parsed_{0};
    std::atomic<std::uint64_t> events_delivered_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> unknown_symbol_{0};
    std::atomic<std::uint64_t> malformed_{0};
    std::atomic<std::uint64_t> unhandled_{0};
    std::atomic<std::uint64_t> reordered_{0};
    std::atomic<std::int64_t> max_backwards_{0};
    std::atomic<std::uint64_t> bars_interval_stamped_{0};
    std::atomic<std::int64_t> max_bar_lag_{0};
    std::atomic<std::uint32_t> reconnects_{0};
    std::atomic<std::uint32_t> consecutive_failures_{0};
    std::atomic<std::int64_t> skew_last_{0};
    std::atomic<std::int64_t> skew_min_{0};
    std::atomic<std::int64_t> skew_max_{0};
    std::atomic<std::int64_t> skew_total_{0};
    std::atomic<std::uint64_t> skew_samples_{0};

    mutable std::mutex error_mutex_;
    std::string error_;

    /// Socket thread only, after construction.
    MonotonicGate gate_;
    std::shared_ptr<StreamRecorder> recorder_;

    /// True when this feed created the recorder and must therefore close it.
    bool owns_recorder_ = false;

    /// The socket the pump is currently using, so stop() can end a receive
    /// that would otherwise never return. Owned by the pump thread's stack;
    /// this pointer and the call through it are both under the mutex, and the
    /// pump clears it before the socket goes out of scope.
    ///
    /// Needed because WinHTTP's synchronous WebSocket receive does not honour
    /// a timeout -- see WebSocket::request_close().
    mutable std::mutex socket_mutex_;
    WebSocket* active_socket_ = nullptr;
};

/// Builds the subscribe message for a config. Exposed so a test can assert its
/// shape without opening a socket.
[[nodiscard]] std::string subscribe_message(const LiveFeedConfig& config);

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_LIVE_FEED_HPP
