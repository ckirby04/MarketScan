#ifndef MARKETSCAN_LIVE_RECORDED_FEED_HPP
#define MARKETSCAN_LIVE_RECORDED_FEED_HPP

/*
 * Replays a recording as an IMarketDataFeed.
 *
 * This is the other half of the equivalence test. The live feed turns socket
 * messages into events; this turns the same messages, off disk, into events --
 * through the same parser and the same monotonic gate, because they are the
 * same functions, not two implementations that agree today.
 *
 * WHAT IS AND IS NOT REPRODUCED
 *
 * The event sequence is reproduced exactly: same events, same order, same
 * timestamps, same symbol ids given the same interning order. That is what the
 * strategy sees, so that is what has to match.
 *
 * Timing is not reproduced. A replay runs as fast as the engine will go, and
 * the wall-clock intervals between events are gone. A strategy that made a
 * decision because of how long something took would therefore diverge -- which
 * is a true statement about that strategy, and one the equivalence test is
 * built to surface rather than hide.
 *
 * Fills are not reproduced either, and are not meant to be: a replay prices
 * orders through the fill simulator, and the live session got whatever the
 * venue gave it. Part 5 compares decisions, not fills, for exactly this
 * reason.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "marketscan/data/feed.hpp"
#include "marketscan/live/recorder.hpp"
#include "marketscan/live/stream_parser.hpp"

namespace marketscan::live {

/// What a replay found in the file.
struct RecordedFeedStats {
    std::uint64_t messages = 0;
    std::uint64_t events = 0;
    std::uint64_t controls = 0;
    std::uint64_t unknown_symbol = 0;
    std::uint64_t malformed = 0;
    std::uint64_t unhandled_kind = 0;
    std::uint64_t reordered = 0;
    std::int64_t max_backwards_ns = 0;
    std::uint64_t bars_interval_stamped = 0;
    std::int64_t max_bar_lag_ns = 0;

    /// Reconnect markers in the recording. Each one is a gap in the tape the
    /// live session did not see either.
    std::uint64_t reconnects = 0;

    /// Recorded arrival time minus exchange timestamp, over trades and quotes.
    ///
    /// Measured here, during construction, against the timestamp the message
    /// carried and *before* the monotonic gate may raise it -- which is the
    /// same point in the pipeline the live feed measures at. That is not a
    /// detail: an earlier version of this computed skew in the CLI from the
    /// gated timestamps, and the replay of a session reported a mean 0.2 ms
    /// adrift of the live run it was reproducing. Two definitions of one
    /// number is one too many, so there is one, here.
    ///
    /// Bars are excluded, for the reason MonotonicGate gives.
    std::int64_t skew_min_ns = 0;
    std::int64_t skew_max_ns = 0;
    std::int64_t skew_mean_ns = 0;
    std::uint64_t skew_samples = 0;
};

/// A feed that reads a recording written by StreamRecorder.
class RecordedFeed final : public IMarketDataFeed {
public:
    /// Reports progress through the recording, so a RecordedBroker alongside
    /// this knows which venue responses have come due.
    ///
    /// Borrowed and may be null, which is what a market-data-only replay
    /// passes.
    void set_position_sink(ReplayPosition* sink) noexcept { position_ = sink; }

    /// Interns the recording's own symbol list, in the order the header lists
    /// it, so that a replay assigns the same ids the live session did when the
    /// same table is used. Throws std::runtime_error if the file is missing,
    /// is not a recording, or has a line it cannot parse.
    RecordedFeed(const std::filesystem::path& path, SymbolTable& symbols);

    bool next(MarketEvent& out) override;

    [[nodiscard]] bool can_reset() const noexcept override { return true; }

    void reset() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "recorded"; }

    [[nodiscard]] const std::vector<SymbolId>& symbols() const noexcept override {
        return symbol_ids_;
    }

    [[nodiscard]] FeedSpan span() const noexcept override { return span_; }

    [[nodiscard]] const RecordingHeader& header() const noexcept { return header_; }

    [[nodiscard]] const RecordedFeedStats& stats() const noexcept { return stats_; }

    /// Receive timestamps from the recording, one per event, parallel to the
    /// event sequence. Not fed to the engine -- events keep their exchange
    /// timestamps -- but available to a report that wants to say how far
    /// behind the tape the live session was.
    [[nodiscard]] const std::vector<Timestamp>& receive_timestamps() const noexcept {
        return received_;
    }

private:
    RecordingHeader header_;
    std::vector<SymbolId> symbol_ids_;
    std::vector<MarketEvent> events_;
    std::vector<Timestamp> received_;

    /// The recording line each event came from, so progress can be reported in
    /// the recording's own sequence numbers rather than in an event index.
    std::vector<std::uint64_t> sequences_;
    ReplayPosition* position_ = nullptr;
    std::size_t cursor_ = 0;
    FeedSpan span_;
    RecordedFeedStats stats_;
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_RECORDED_FEED_HPP
