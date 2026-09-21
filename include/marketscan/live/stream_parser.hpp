#ifndef MARKETSCAN_LIVE_STREAM_PARSER_HPP
#define MARKETSCAN_LIVE_STREAM_PARSER_HPP

/*
 * Turns one stream message into engine events.
 *
 * WHY THIS IS ITS OWN FILE
 *
 * The live feed and the replay feed both need to do exactly this, and if they
 * each did it themselves the equivalence test in Part 5 would be comparing two
 * parsers rather than comparing live against replay. A divergence in rounding,
 * in timestamp handling, in which message kinds are recognised, would show up
 * as a strategy disagreement and be debugged in the wrong place -- or worse,
 * would not show up at all because both sides were wrong the same way.
 *
 * So there is one parser. The live feed hands it bytes off the socket; the
 * recorded feed hands it the same bytes off disk. Whatever it does, it does
 * identically to both, because it is the same function.
 *
 * SYMBOLS ARE LOOKED UP, NEVER INTERNED
 *
 * Interning mutates the SymbolTable, and the live parser runs on the feed
 * thread while the engine thread reads that table. So the subscription set is
 * interned up front, on the owning thread, and a message about a symbol that
 * was never subscribed is counted and dropped rather than silently given a new
 * id. Dropping is visible in ParseStats::unknown_symbol.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/event.hpp"
#include "marketscan/core/symbol_table.hpp"

namespace marketscan::live {

/// Shares per unit of the quote-size field on this feed.
///
/// One, because the feed sends share counts. This is not the obvious answer --
/// several vendors quote sizes in round lots, and the first draft of this
/// parser multiplied by a hundred on that assumption.
///
/// The assumption was checked against a real capture rather than left standing,
/// and it was wrong. In a 45-second IEX session on 2026-09-21, bid and ask
/// sizes for AAPL, SPY and MSFT ranged from 40 to 1880 while trades in the same
/// session ranged from 1 to 200 shares. Read as lots those quotes would be
/// 4,000 to 188,000 shares at the touch on a venue with roughly two per cent of
/// consolidated volume, against trades averaging forty shares -- a book that
/// could never be consumed. Read as shares they are ordinary.
///
/// The constant stays rather than being deleted, so that the conversion has a
/// name and this note has somewhere to live. See docs/EXECUTION.md.
inline constexpr double kQuoteLotSize = 1.0;

/// A non-market message: the handshake, an error, a subscription echo.
struct StreamControl {
    /// The message's "T" field verbatim: "success", "error", "subscription".
    std::string type;
    /// The "msg" field, when there is one.
    std::string message;
    /// The "code" field, when there is one. Zero otherwise.
    int code = 0;
};

/// Tallies of what a parse did and did not turn into events.
///
/// Every one of these is a reason a replay could be shorter than the recording
/// it came from, so they are counted rather than logged and forgotten.
struct ParseStats {
    std::uint64_t events = 0;
    std::uint64_t controls = 0;

    /// Messages whose "T" is a market kind this build does not handle --
    /// corrections, cancels, status, LULD. Counted, not events.
    std::uint64_t unhandled_kind = 0;

    /// Messages about a symbol not in the table. See the header comment.
    std::uint64_t unknown_symbol = 0;

    /// Messages that were the right kind but did not have the fields.
    std::uint64_t malformed = 0;
};

/// The result of parsing one socket message.
struct ParseResult {
    /// In the order the message listed them. A stream message is a JSON array
    /// and the order inside it is the order events happened.
    std::vector<MarketEvent> events;
    std::vector<StreamControl> controls;
    ParseStats stats;

    /// Set when the message was not JSON at all, or not an array. Distinct
    /// from a malformed element, which is counted in stats.
    std::string error;
};

/// Parses one message.
///
/// `symbols` is read, never written: see the header comment. `text` is the
/// whole socket message, which is a JSON array of zero or more objects.
/// Forces a stream of events into the non-decreasing order the engine requires.
///
/// WHY THIS IS NEEDED AT ALL
///
/// Events arrive carrying exchange timestamps, and those are not sorted. Two
/// symbols travel different paths to the same socket, and a trade stamped a
/// few microseconds ago can arrive after one stamped later. IEventSource
/// promises non-decreasing order and EngineT::refill throws when a source
/// breaks that promise -- correctly, because a replay that silently reorders
/// is a replay that silently lies.
///
/// So the disorder is resolved here, where it is visible, rather than by
/// relaxing the engine. An event stamped before the last one emitted is
/// raised to that timestamp and counted. It is not dropped: losing a trade to
/// keep a sort clean is a worse trade than moving it forward by the
/// microseconds the transport cost.
///
/// SHARED ON PURPOSE
///
/// The live feed and the recorded feed both run their events through this, for
/// the same reason they share a parser. Given the same message sequence the
/// two produce byte-identical timestamps, which is what lets the equivalence
/// test in Part 5 compare decisions rather than compare two orderings.
///
/// TWO COUNTERS, NOT ONE
///
/// Bars and ticks arrive late for unrelated reasons and the two must not share
/// a statistic.
///
/// A trade or a quote is stamped at the instant it happened, so one arriving
/// behind its predecessor is transport jitter, and the size of the gap is a
/// fact about the network. A bar is stamped at the *start* of its interval and
/// cannot be sent until the interval ends, so a one-minute bar is always
/// roughly sixty seconds behind the ticks around it. That is not jitter, it is
/// what a bar is.
///
/// Counting them together produces a number that is dominated by the bar
/// interval and says nothing about either. The first capture this feed took
/// reported a maximum of 60,236 ms backwards, which read as a broken feed and
/// was in fact three one-minute bars behaving exactly as documented. So they
/// are counted apart.
class MonotonicGate {
public:
    /// Raises `event`'s timestamp to the last one emitted if it is behind.
    /// Returns true if it had to.
    bool apply(MarketEvent& event) noexcept;

    void reset() noexcept {
        last_ = 0;
        reordered_ = 0;
        max_backwards_ = 0;
        interval_stamped_ = 0;
        max_interval_lag_ = 0;
    }

    [[nodiscard]] Timestamp last() const noexcept { return last_; }

    /// Trades and quotes that arrived stamped before their predecessor.
    [[nodiscard]] std::uint64_t reordered() const noexcept { return reordered_; }

    /// The largest such gap, in nanoseconds. A few microseconds is transport
    /// jitter; a few seconds is a broken feed.
    [[nodiscard]] std::int64_t max_backwards() const noexcept { return max_backwards_; }

    /// Bars whose interval-start timestamp was behind the tick stream.
    /// Expected, not alarming: see the class comment.
    [[nodiscard]] std::uint64_t interval_stamped() const noexcept { return interval_stamped_; }

    /// The largest bar lag, in nanoseconds. Should sit near one bar interval;
    /// materially more than that means bars are arriving late on top of being
    /// interval-stamped, which is worth knowing.
    [[nodiscard]] std::int64_t max_interval_lag() const noexcept { return max_interval_lag_; }

private:
    Timestamp last_ = 0;
    std::uint64_t reordered_ = 0;
    std::int64_t max_backwards_ = 0;
    std::uint64_t interval_stamped_ = 0;
    std::int64_t max_interval_lag_ = 0;
};

[[nodiscard]] ParseResult parse_stream_message(std::string_view text, const SymbolTable& symbols);

/// Accumulates into an existing result, so a caller draining several messages
/// gets one event vector and one set of tallies.
void parse_stream_message_into(std::string_view text, const SymbolTable& symbols, ParseResult& out);

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_STREAM_PARSER_HPP
