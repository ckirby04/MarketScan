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
 * `seq` counts messages. A replay that stops early can say exactly where.
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
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/types.hpp"

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
};

/// Appends socket messages to a file.
///
/// Flushes every message. A recording is worth having precisely when something
/// went wrong, and the most interesting recordings end with a crash.
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

    [[nodiscard]] std::uint64_t messages() const noexcept { return sequence_; }

    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void close();

private:
    std::filesystem::path path_;
    std::ofstream out_;
    std::uint64_t sequence_ = 0;
    std::uint64_t bytes_ = 0;
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

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_RECORDER_HPP
