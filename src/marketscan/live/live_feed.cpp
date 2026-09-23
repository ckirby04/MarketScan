#include "marketscan/live/live_feed.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "marketscan/core/clock.hpp"
#include "marketscan/live/detail.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/websocket.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::live {
namespace {

/// How long one receive waits before returning empty-handed.
///
/// Short enough that stop() and request_drop_for_test() are noticed promptly,
/// long enough that a quiet symbol does not spin the thread.
constexpr int kReceiveTimeoutMs = 250;

/// Error codes the stream sends that another attempt will not fix. Retrying
/// past a refusal is exactly what docs/DATA_POLICY.md forbids: 402 is bad
/// credentials, 406 is too many connections for this account, 409 and 410 are
/// a subscription this key does not have.
[[nodiscard]] bool is_fatal_stream_error(int code) noexcept {
    switch (code) {
        case 401:
        case 402:
        case 403:
        case 404:
        case 405:
        case 406:
        case 409:
        case 410:
            return true;
        default:
            return false;
    }
}

using detail::interruptible_sleep;
using detail::local_now;

}  // namespace

std::string_view to_string(FeedState state) noexcept {
    switch (state) {
        case FeedState::Idle:
            return "idle";
        case FeedState::Connecting:
            return "connecting";
        case FeedState::Streaming:
            return "streaming";
        case FeedState::Reconnecting:
            return "reconnecting";
        case FeedState::Stopped:
            return "stopped";
    }
    return "?";
}

std::string subscribe_message(const LiveFeedConfig& config) {
    nlohmann::json message;
    message["action"] = "subscribe";
    if (config.subscribe_trades) {
        message["trades"] = config.symbols;
    }
    if (config.subscribe_quotes) {
        message["quotes"] = config.symbols;
    }
    if (config.subscribe_bars) {
        message["bars"] = config.symbols;
    }
    return message.dump();
}

LiveMarketDataFeed::LiveMarketDataFeed(LiveFeedConfig config,
                                       Credentials credentials,
                                       SymbolTable& symbols)
    : config_(std::move(config)),
      credentials_(std::move(credentials)),
      symbol_table_(symbols),
      queue_(config_.queue_capacity) {
    if (config_.symbols.empty()) {
        throw std::invalid_argument("LiveMarketDataFeed needs at least one symbol");
    }
    if (!credentials_.ok()) {
        throw std::invalid_argument("LiveMarketDataFeed has no credentials: " + credentials_.error);
    }
    if (!config_.subscribe_trades && !config_.subscribe_quotes && !config_.subscribe_bars) {
        throw std::invalid_argument("LiveMarketDataFeed subscribed to nothing");
    }

    // Interned here, on the constructing thread, before the socket thread
    // exists. After this the parser only reads the table, which is what makes
    // sharing it across two threads safe without a lock.
    symbol_ids_.reserve(config_.symbols.size());
    for (const std::string& ticker : config_.symbols) {
        symbol_ids_.push_back(symbol_table_.intern(ticker));
    }
    std::sort(symbol_ids_.begin(), symbol_ids_.end());
    symbol_ids_.erase(std::unique(symbol_ids_.begin(), symbol_ids_.end()), symbol_ids_.end());

    if (config_.recorder) {
        // Shared with the broker. Not ours to close.
        recorder_ = config_.recorder;
    } else if (!config_.recording_path.empty()) {
        RecordingHeader header;
        header.host = std::string(kMarketDataStreamHost);
        header.path = std::string(kMarketDataStreamPath);
        header.feed = "iex";
        header.symbols = config_.symbols;
        header.started = local_now();
        recorder_ = std::make_shared<StreamRecorder>(config_.recording_path, header);
        owns_recorder_ = true;
    }
}

LiveMarketDataFeed::~LiveMarketDataFeed() {
    stop();
}

void LiveMarketDataFeed::start() {
    if (thread_.joinable()) {
        return;
    }
    stopping_.store(false, std::memory_order_relaxed);
    state_.store(FeedState::Connecting, std::memory_order_release);
    thread_ = std::thread([this] { run_socket(); });
}

void LiveMarketDataFeed::stop() {
    stopping_.store(true, std::memory_order_relaxed);

    // Ends whatever receive the pump is sitting in. Without this a quiet
    // socket makes the join below wait for the connection to die of old age --
    // see WebSocket::request_close().
    {
        const std::lock_guard<std::mutex> lock(socket_mutex_);
        if (active_socket_ != nullptr) {
            active_socket_->request_close();
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }
    state_.store(FeedState::Stopped, std::memory_order_release);
    if (recorder_ && owns_recorder_) {
        recorder_->close();
    }
}

void LiveMarketDataFeed::request_drop_for_test() {
    drop_requested_.store(true, std::memory_order_relaxed);
}

bool LiveMarketDataFeed::wait_until_streaming(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const FeedState now = state_.load(std::memory_order_acquire);
        if (now == FeedState::Streaming) {
            return true;
        }
        if (now == FeedState::Stopped) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool LiveMarketDataFeed::next(MarketEvent& out) {
    if (!queue_.try_pop(out)) {
        return false;
    }
    events_delivered_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

FeedSpan LiveMarketDataFeed::span() const noexcept {
    // `known` stays false. A live feed has no last timestamp, and inventing one
    // is how a scrub bar comes to promise something it cannot do.
    FeedSpan out;
    out.event_count = events_delivered_.load(std::memory_order_relaxed);
    return out;
}

FeedState LiveMarketDataFeed::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

std::uint64_t LiveMarketDataFeed::recorded_messages() const noexcept {
    return recorder_ ? recorder_->messages() : 0;
}

void LiveMarketDataFeed::set_error(std::string message) {
    const std::lock_guard<std::mutex> lock(error_mutex_);
    error_ = std::move(message);
}

LiveFeedStats LiveMarketDataFeed::stats() const {
    LiveFeedStats out;
    out.state = state_.load(std::memory_order_acquire);
    out.messages = messages_.load(std::memory_order_relaxed);
    out.events_parsed = events_parsed_.load(std::memory_order_relaxed);
    out.events_delivered = events_delivered_.load(std::memory_order_relaxed);
    out.dropped_events = dropped_.load(std::memory_order_relaxed);
    out.queued = static_cast<std::uint64_t>(queue_.size_approx());
    out.unknown_symbol = unknown_symbol_.load(std::memory_order_relaxed);
    out.malformed = malformed_.load(std::memory_order_relaxed);
    out.unhandled_kind = unhandled_.load(std::memory_order_relaxed);
    out.reordered = reordered_.load(std::memory_order_relaxed);
    out.max_backwards_ns = max_backwards_.load(std::memory_order_relaxed);
    out.bars_interval_stamped = bars_interval_stamped_.load(std::memory_order_relaxed);
    out.max_bar_lag_ns = max_bar_lag_.load(std::memory_order_relaxed);
    out.reconnects = reconnects_.load(std::memory_order_relaxed);
    out.consecutive_failures = consecutive_failures_.load(std::memory_order_relaxed);

    out.skew_samples = skew_samples_.load(std::memory_order_relaxed);
    out.skew_last_ns = skew_last_.load(std::memory_order_relaxed);
    out.skew_min_ns = skew_min_.load(std::memory_order_relaxed);
    out.skew_max_ns = skew_max_.load(std::memory_order_relaxed);
    if (out.skew_samples > 0) {
        out.skew_mean_ns = skew_total_.load(std::memory_order_relaxed) /
                           static_cast<std::int64_t>(out.skew_samples);
    }

    {
        const std::lock_guard<std::mutex> lock(error_mutex_);
        out.error = error_;
    }
    return out;
}

void LiveMarketDataFeed::publish(const ParseResult& parsed, Timestamp received) {
    unknown_symbol_.fetch_add(parsed.stats.unknown_symbol, std::memory_order_relaxed);
    malformed_.fetch_add(parsed.stats.malformed, std::memory_order_relaxed);
    unhandled_.fetch_add(parsed.stats.unhandled_kind, std::memory_order_relaxed);

    for (MarketEvent event : parsed.events) {
        // Skew is measured against the exchange timestamp the event arrived
        // with, before the gate may raise it. Measuring after would fold this
        // feed's own reordering into a number that is supposed to describe the
        // network and the clock.
        //
        // Bars are excluded: a bar is stamped at the start of an interval it
        // cannot be sent before the end of, so one bar contributes its whole
        // interval and swamps thousands of quotes.
        if (event.type != EventType::Bar) {
            const std::int64_t skew = received - event.ts();
            const std::uint64_t sample = skew_samples_.fetch_add(1, std::memory_order_relaxed);
            skew_last_.store(skew, std::memory_order_relaxed);
            skew_total_.fetch_add(skew, std::memory_order_relaxed);
            if (sample == 0) {
                skew_min_.store(skew, std::memory_order_relaxed);
                skew_max_.store(skew, std::memory_order_relaxed);
            } else {
                if (skew < skew_min_.load(std::memory_order_relaxed)) {
                    skew_min_.store(skew, std::memory_order_relaxed);
                }
                if (skew > skew_max_.load(std::memory_order_relaxed)) {
                    skew_max_.store(skew, std::memory_order_relaxed);
                }
            }
        }

        gate_.apply(event);

        events_parsed_.fetch_add(1, std::memory_order_relaxed);
        if (!queue_.try_push(event)) {
            // The engine thread is behind. Say so; do not grow the buffer and
            // do not block the socket thread, either of which would hide the
            // fact that this run saw less than the market did.
            const std::uint64_t dropped = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (dropped == 1 || dropped % 1000 == 0) {
                log::write(MS_LOG_ERROR,
                           "live",
                           "event queue full; dropped " + std::to_string(dropped) +
                               " events. The run has not seen the whole tape.");
            }
        }
    }

    reordered_.store(gate_.reordered(), std::memory_order_relaxed);
    max_backwards_.store(gate_.max_backwards(), std::memory_order_relaxed);
    bars_interval_stamped_.store(gate_.interval_stamped(), std::memory_order_relaxed);
    max_bar_lag_.store(gate_.max_interval_lag(), std::memory_order_relaxed);
}

void LiveMarketDataFeed::run_socket() {
    int backoff_ms = config_.reconnect_initial_ms;

    while (!stopping_.load(std::memory_order_relaxed)) {
        state_.store(FeedState::Connecting, std::memory_order_release);
        drop_requested_.store(false, std::memory_order_relaxed);

        WebSocket socket;
        bool fatal = false;
        std::string reason;

        {
            const std::lock_guard<std::mutex> lock(socket_mutex_);
            active_socket_ = &socket;
        }

        if (!socket.connect(std::string(kMarketDataStreamHost),
                            std::string(kMarketDataStreamPath),
                            config_.user_agent)) {
            reason = socket.error();
        } else {
            // The greeting, then auth, then the subscription. Each step waits
            // for its acknowledgement: a subscribe sent before the server has
            // authenticated is a subscribe that is silently ignored, and a
            // feed that then sits in Streaming with no events looks like a
            // quiet market rather than a broken handshake.
            bool handshake_ok = true;

            const auto expect = [&](const char* what) -> ParseResult {
                ParseResult parsed;
                const SocketMessage message = socket.receive(10000);
                if (!message.ok) {
                    handshake_ok = false;
                    reason = message.closed ? std::string("connection closed during ") + what
                                            : std::string("timed out waiting for ") + what;
                    return parsed;
                }
                messages_.fetch_add(1, std::memory_order_relaxed);
                const Timestamp received = local_now();
                if (recorder_) {
                    recorder_->record(message.text, received);
                }
                parse_stream_message_into(message.text, symbol_table_, parsed);
                for (const StreamControl& control : parsed.controls) {
                    if (control.type == "error") {
                        handshake_ok = false;
                        fatal = is_fatal_stream_error(control.code);
                        reason =
                            "stream error " + std::to_string(control.code) + ": " + control.message;
                    }
                }
                return parsed;
            };

            (void)expect("the greeting");

            if (handshake_ok) {
                // The only outgoing message that carries the secret, and the
                // one place it must not reach the recorder. Incoming messages
                // are recorded; outgoing ones never are.
                nlohmann::json auth;
                auth["action"] = "auth";
                auth["key"] = credentials_.key_id;
                auth["secret"] = credentials_.secret;
                if (!socket.send(auth.dump())) {
                    handshake_ok = false;
                    reason = "could not send auth: " + socket.error();
                }
            }

            if (handshake_ok) {
                (void)expect("authentication");
            }

            if (handshake_ok) {
                if (!socket.send(subscribe_message(config_))) {
                    handshake_ok = false;
                    reason = "could not send subscription: " + socket.error();
                }
            }

            if (handshake_ok) {
                (void)expect("the subscription acknowledgement");
            }

            if (handshake_ok) {
                log::write(MS_LOG_INFO,
                           "live",
                           "streaming " + std::to_string(config_.symbols.size()) +
                               " symbols from " + std::string(kMarketDataStreamHost));
                state_.store(FeedState::Streaming, std::memory_order_release);
                consecutive_failures_.store(0, std::memory_order_relaxed);
                backoff_ms = config_.reconnect_initial_ms;
                set_error({});

                // The pump.
                while (!stopping_.load(std::memory_order_relaxed)) {
                    if (drop_requested_.exchange(false, std::memory_order_relaxed)) {
                        reason = "drop requested by the caller (reconnect exercise)";
                        break;
                    }
                    const SocketMessage message = socket.receive(kReceiveTimeoutMs);
                    if (message.closed) {
                        reason = socket.error().empty() ? "connection closed by the server"
                                                        : socket.error();
                        break;
                    }
                    if (!message.ok) {
                        continue;  // A quiet interval. Not an error.
                    }

                    messages_.fetch_add(1, std::memory_order_relaxed);
                    const Timestamp received = local_now();
                    if (recorder_) {
                        recorder_->record(message.text, received);
                    }

                    ParseResult parsed;
                    parse_stream_message_into(message.text, symbol_table_, parsed);
                    if (!parsed.error.empty()) {
                        malformed_.fetch_add(1, std::memory_order_relaxed);
                        log::write(MS_LOG_WARN, "live", parsed.error);
                        continue;
                    }
                    for (const StreamControl& control : parsed.controls) {
                        if (control.type == "error") {
                            log::write(MS_LOG_WARN,
                                       "live",
                                       "stream error " + std::to_string(control.code) + ": " +
                                           control.message);
                            if (is_fatal_stream_error(control.code)) {
                                fatal = true;
                                reason = "stream error " + std::to_string(control.code) + ": " +
                                         control.message;
                            }
                        }
                    }
                    publish(parsed, received);
                    if (fatal) {
                        break;
                    }
                }
            }
        }

        {
            // Cleared before the socket leaves scope, so stop() can never
            // reach a destroyed one.
            const std::lock_guard<std::mutex> lock(socket_mutex_);
            active_socket_ = nullptr;
        }
        socket.close();

        if (stopping_.load(std::memory_order_relaxed)) {
            break;
        }

        if (fatal) {
            // Retrying a refusal is the thing DATA_POLICY.md forbids. Stop,
            // say why, and leave it to a person.
            log::write(MS_LOG_ERROR, "live", "not retrying: " + reason);
            set_error(reason);
            state_.store(FeedState::Stopped, std::memory_order_release);
            return;
        }

        const std::uint32_t failures =
            consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (config_.max_reconnect_attempts != 0 && failures > config_.max_reconnect_attempts) {
            const std::string message = "gave up after " + std::to_string(failures) +
                                        " consecutive failures; last: " + reason;
            log::write(MS_LOG_ERROR, "live", message);
            set_error(message);
            state_.store(FeedState::Stopped, std::memory_order_release);
            return;
        }

        reconnects_.fetch_add(1, std::memory_order_relaxed);
        state_.store(FeedState::Reconnecting, std::memory_order_release);
        set_error(reason);

        const Timestamp at = local_now();
        if (recorder_) {
            // So a replay sees the gap rather than reading straight across it.
            recorder_->record_reconnect(at, failures, reason);
        }
        log::write(MS_LOG_WARN,
                   "live",
                   "connection lost (" + reason + "); attempt " + std::to_string(failures) +
                       " in " + std::to_string(backoff_ms) + "ms");

        interruptible_sleep(backoff_ms, stopping_);
        backoff_ms = std::min(backoff_ms * 2, config_.reconnect_max_ms);
    }

    state_.store(FeedState::Stopped, std::memory_order_release);
}

}  // namespace marketscan::live
