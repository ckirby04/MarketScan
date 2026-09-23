#ifndef MARKETSCAN_LIVE_RECORDER_HPP
#define MARKETSCAN_LIVE_RECORDER_HPP

/*
 * Writes every byte the socket delivered, in the order it delivered them.
 *
 * WHY NOT CSV
 *
 * CsvFeed decides what a file holds from its header: bid and ask columns mean
 * quotes, open/high/low/close mean bars, a bare price means trades. One kind
 * per file. A live stream interleaves all three, and the interleaving is the
 * part that matters -- a strategy that sees a trade before the quote that
 * preceded it is being shown a market that did not happen. So a recording gets
 * its own format rather than three files that cannot be merged back.
 *
 * THE FORMAT
 *
 * NDJSON. One header line, then one line per socket message:
 *
 *   {"marketscan_recording":1,"host":...,"path":...,"symbols":[...],...}
 *   {"seq":0,"recv":1758479793355534795,"raw":"[{\"T\":\"success\",...}]"}
 *
 * `raw` is the message exactly as it arrived, as a JSON string. Nothing is
 * reinterpreted on the way in, so a parser bug found next month can be fixed
 * and the recording reparsed; a recording of already-parsed events would have
 * baked the bug in permanently.
 *
 * `recv` is when this process saw the message, from the same clock the feed
 * reports skew against. It is not written back into any event -- events keep
 * their exchange timestamps -- but it is what makes it possible to say later
 * how far behind the tape a run was.
 *
 * `seq` counts lines. A replay that stops early can say exactly where.
 *
 * BOTH DIRECTIONS, AS OF SESSION F2
 *
 * A recording used to hold market data only. That made the equivalence test
 * wrong in a way that took a real run to expose: replay fed the strategy
 * simulated acceptances where live had twenty-seven venue refusals, so the two
 * runs made different decisions from *different information* and the test
 * blamed the strategy.
 *
 * A recording is now a complete account of everything that crossed the
 * boundary, in arrival order:
 *
 *   {"seq":7,"recv":...,"submit":{...}}    what this process asked the venue for
 *   {"seq":8,"recv":...,"response":{...}}  what the venue said back, raw
 *
 * Responses are stored as the raw trade-update text for the same reason market
 * data is: nothing is reinterpreted on the way in, and replay re-parses with
 * the same parser rather than a second one that might disagree.
 *
 * NOT COMMITTED, EVER
 *
 * A recording is real market data under a subscriber agreement. docs/
 * DATA_POLICY.md §7 covers this and .gitignore enforces it. The equivalence
 * test generates its own recording and deletes it.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/types.hpp"
#include "marketscan/exec/order.hpp"

namespace marketscan::live {

/// What a recording says about itself, before its first message.
struct RecordingHeader {
    int version = 0;
    std::string host;
    std::string path;
    std::string feed;
    std::vector<std::string> symbols;

    /// Wall clock when recording started, as nanoseconds since the epoch.
    Timestamp started = 0;

    /// The engine build that wrote it. A replay under a different build is
    /// allowed -- the ABI is frozen -- but a divergence wants this in the
    /// report.
    std::string engine_version;

    /// The run token the live session used.
    ///
    /// Needed, not decorative: every client order id in the recording is
    /// `ms-<run_token>-<engine id>`, and a replay that used a different token
    /// would derive different ids and route none of the recorded responses.
    std::string run_token;
};

/// Appends to a recording.
///
/// Flushes every line. A recording is worth having precisely when something
/// went wrong, and the most interesting recordings end with a crash.
///
/// THREAD SAFE, BECAUSE THREE THREADS WRITE TO IT
///
/// The market data socket thread writes events, the trade-updates socket thread
/// writes responses, and the engine thread writes submissions as the strategy
/// makes them. One lock serialises them, which is also what makes the sequence
/// number a real arrival order across all three rather than three interleaved
/// counters. The lock is held for one formatted line and one flush; nothing on
/// the per-event path waits on anything else.
class StreamRecorder {
public:
    /// Throws std::runtime_error if the file cannot be opened.
    StreamRecorder(const std::filesystem::path& path, const RecordingHeader& header);
    ~StreamRecorder();

    StreamRecorder(const StreamRecorder&) = delete;
    StreamRecorder& operator=(const StreamRecorder&) = delete;

    /// Records one message. `recv` is this process's clock at arrival.
    void record(std::string_view raw, Timestamp recv);

    /// Records the fact of a reconnection, so a replay can see the gap rather
    /// than reading straight across it.
    void record_reconnect(Timestamp at, std::uint32_t attempt, std::string_view reason);

    /// Records an order this process sent, and what the submit call answered.
    ///
    /// `client_order_id` is the durable link -- the string this process chose,
    /// which the venue echoes on every later update. `accepted` is what the
    /// submit itself returned; a refusal carries the venue's words in `reason`.
    void record_submission(std::string_view client_order_id,
                           OrderId engine_id,
                           std::string_view symbol,
                           Side side,
                           MS_OrderType type,
                           MS_TimeInForce tif,
                           Quantity quantity,
                           Price limit_price,
                           std::uint64_t strategy_client_id,
                           bool accepted,
                           std::string_view reason,
                           Timestamp at);

    /// Records a venue response verbatim -- a trade-update message, exactly as
    /// it arrived.
    void record_response(std::string_view raw, Timestamp recv);

    [[nodiscard]] std::uint64_t messages() const noexcept { return sequence_; }

    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void close();

private:
    /// Writes one already-built JSON line, under the lock.
    void write_line(const std::string& text);

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::ofstream out_;
    std::uint64_t sequence_ = 0;
    std::uint64_t bytes_ = 0;
};

/// One order this process sent, as the recording holds it.
struct RecordedSubmission {
    std::uint64_t sequence = 0;
    Timestamp received = 0;

    std::string client_order_id;
    OrderId engine_id = kInvalidOrderId;

    std::string symbol;
    Side side = MS_SIDE_BUY;
    MS_OrderType type = MS_ORD_MARKET;
    MS_TimeInForce tif = MS_TIF_DAY;
    Quantity quantity = 0.0;
    Price limit_price = 0.0;

    /// The strategy's own id from the request, echoed so a replay can tell one
    /// intent from another.
    std::uint64_t strategy_client_id = 0;

    bool accepted = false;
    std::string reason;
};

/// One venue response, raw.
struct RecordedResponse {
    std::uint64_t sequence = 0;
    Timestamp received = 0;
    std::string raw;
};

/// One line of a recording.
struct RecordedMessage {
    std::uint64_t sequence = 0;
    Timestamp received = 0;
    std::string raw;

    /// A reconnect marker rather than a socket message. `raw` is empty and
    /// `reason` says why the connection dropped.
    bool reconnect = false;
    std::uint32_t attempt = 0;
    std::string reason;
};

/// Reads a recording back.
///
/// Throws std::runtime_error on a missing file, a missing or wrong-version
/// header, or a line that is not parseable -- never skips one. A replay that
/// silently drops a message is a replay that silently disagrees with the
/// session it is supposed to reproduce.
[[nodiscard]] std::vector<RecordedMessage> read_recording(const std::filesystem::path& path,
                                                          RecordingHeader& header);

/// Everything a recording holds, in one pass.
///
/// A recording written before Session F2 has no submissions or responses, and
/// reads back with those vectors empty -- which is exactly the signal a replay
/// uses to fall back to the fill simulator. No flag decides it; the file does.
struct RecordingContents {
    RecordingHeader header;
    std::vector<RecordedMessage> messages;
    std::vector<RecordedSubmission> submissions;
    std::vector<RecordedResponse> responses;

    /// True when this recording can answer for a venue. False for a market
    /// data-only recording, and for a synthetic panel that was never a
    /// recording at all.
    [[nodiscard]] bool has_venue_side() const noexcept {
        return !submissions.empty() || !responses.empty();
    }
};

[[nodiscard]] RecordingContents read_recording_contents(const std::filesystem::path& path);

/// How far through a recording a replay has got.
///
/// Written by the feed as it emits events, read by the broker to decide which
/// venue responses are due. One number in one place, because the alternative is
/// the feed and the broker each keeping their own idea of "now" and drifting.
///
/// Deliberately not a flag and deliberately not visible to the engine: the
/// engine holds an IEventSource and an IBroker and has no idea either of them
/// is reading a file.
struct ReplayPosition {
    std::uint64_t sequence = 0;
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_RECORDER_HPP
