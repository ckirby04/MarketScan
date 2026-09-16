#ifndef MARKETSCAN_UTIL_LOGGING_HPP
#define MARKETSCAN_UTIL_LOGGING_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/types.hpp"

namespace marketscan::log {

/// One line, as the console panel shows it.
struct Record {
    /// Wall clock, not the engine clock. A log line is a fact about when
    /// something was written, not about when it happened in the market -- and
    /// during a fast replay those are wildly different.
    Timestamp ts = 0;

    LogLevel level = MS_LOG_INFO;

    /// Theory id, or a subsystem name such as "engine" or "loader". This is
    /// what the console's per-theory filter keys on.
    std::string source;

    std::string message;
};

/// A bounded ring of the most recent records.
///
/// The GUI reads this from the render thread while the engine writes to it
/// from its own, so every access takes the lock. That is fine here and only
/// here: this is explicitly not on the per-event hot path, and a strategy that
/// logs on every tick is doing something it should be told about.
///
/// Bounded because an unbounded log is a memory leak with good intentions: a
/// replay at ten thousand times speed can produce millions of lines a second.
class RecordBuffer {
public:
    explicit RecordBuffer(std::size_t capacity = 20'000);

    void push(Record record);

    /// A copy, so the caller can iterate without holding the lock.
    [[nodiscard]] std::vector<Record> snapshot() const;

    /// Records added since `after_sequence`, plus the new sequence number.
    /// Lets the console append rather than re-copying the whole ring.
    [[nodiscard]] std::vector<Record> since(std::uint64_t after_sequence,
                                            std::uint64_t& out_sequence) const;

    void clear();

    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Total ever pushed, including records that have since been overwritten.
    [[nodiscard]] std::uint64_t total_written() const;

    /// How many were dropped off the back of the ring.
    [[nodiscard]] std::uint64_t dropped() const;

private:
    mutable std::mutex mutex_;
    std::vector<Record> records_;
    std::size_t capacity_;
    std::size_t head_ = 0;
    std::uint64_t sequence_ = 0;
};

/// Installs the spdlog sinks: one to the console, one into the shared
/// RecordBuffer the GUI reads. Safe to call more than once.
void init(LogLevel minimum = MS_LOG_INFO);

/// The buffer the GUI console reads.
[[nodiscard]] RecordBuffer& buffer();

/// Writes one line. `source` is a theory id or a subsystem name.
void write(LogLevel level, std::string_view source, std::string_view message);

void debug(std::string_view source, std::string_view message);
void info(std::string_view source, std::string_view message);
void warn(std::string_view source, std::string_view message);
void error(std::string_view source, std::string_view message);

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

/// Suppresses anything below `level`. Applies to the console sink and the
/// buffer alike.
void set_level(LogLevel level);

[[nodiscard]] LogLevel level() noexcept;

}  // namespace marketscan::log

#endif  // MARKETSCAN_UTIL_LOGGING_HPP
