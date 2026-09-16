#include "marketscan/util/logging.hpp"

#include <atomic>
#include <utility>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "marketscan/core/clock.hpp"

namespace marketscan::log {
namespace {

std::atomic<LogLevel> g_level{MS_LOG_INFO};
std::atomic<bool> g_initialised{false};

[[nodiscard]] spdlog::level::level_enum to_spdlog(LogLevel level) noexcept {
    switch (level) {
        case MS_LOG_DEBUG:
            return spdlog::level::debug;
        case MS_LOG_INFO:
            return spdlog::level::info;
        case MS_LOG_WARN:
            return spdlog::level::warn;
        case MS_LOG_ERROR:
            return spdlog::level::err;
    }
    return spdlog::level::info;
}

[[nodiscard]] Timestamp wall_clock_now() noexcept {
    static const SystemClock clock;
    return clock.now();
}

}  // namespace

RecordBuffer::RecordBuffer(std::size_t capacity)
    : records_(capacity == 0 ? 1 : capacity), capacity_(capacity == 0 ? 1 : capacity) {}

void RecordBuffer::push(Record record) {
    const std::lock_guard<std::mutex> guard(mutex_);
    records_[head_] = std::move(record);
    head_ = (head_ + 1) % capacity_;
    ++sequence_;
}

std::vector<Record> RecordBuffer::snapshot() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    const std::size_t count =
        sequence_ < capacity_ ? static_cast<std::size_t>(sequence_) : capacity_;

    std::vector<Record> result;
    result.reserve(count);
    // Oldest first. When the ring has wrapped, that is the slot head_ points
    // at; before it wraps, it is slot zero.
    const std::size_t start = sequence_ < capacity_ ? 0 : head_;
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(records_[(start + i) % capacity_]);
    }
    return result;
}

std::vector<Record> RecordBuffer::since(std::uint64_t after_sequence,
                                        std::uint64_t& out_sequence) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    out_sequence = sequence_;

    if (after_sequence >= sequence_) {
        return {};
    }

    // Anything older than the ring's reach is simply gone; start from the
    // oldest record still held rather than pretending otherwise.
    const std::uint64_t oldest_available = sequence_ > capacity_ ? sequence_ - capacity_ : 0;
    const std::uint64_t from =
        after_sequence > oldest_available ? after_sequence : oldest_available;

    std::vector<Record> result;
    result.reserve(static_cast<std::size_t>(sequence_ - from));
    for (std::uint64_t seq = from; seq < sequence_; ++seq) {
        result.push_back(records_[static_cast<std::size_t>(seq % capacity_)]);
    }
    return result;
}

void RecordBuffer::clear() {
    const std::lock_guard<std::mutex> guard(mutex_);
    head_ = 0;
    sequence_ = 0;
}

std::size_t RecordBuffer::size() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return sequence_ < capacity_ ? static_cast<std::size_t>(sequence_) : capacity_;
}

std::uint64_t RecordBuffer::total_written() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return sequence_;
}

std::uint64_t RecordBuffer::dropped() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return sequence_ > capacity_ ? sequence_ - capacity_ : 0;
}

RecordBuffer& buffer() {
    static RecordBuffer instance;
    return instance;
}

void init(LogLevel minimum) {
    bool expected = false;
    if (!g_initialised.compare_exchange_strong(expected, true)) {
        set_level(minimum);
        return;
    }
    try {
        auto logger = spdlog::stdout_color_mt("marketscan");
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        spdlog::set_default_logger(logger);
        spdlog::set_level(to_spdlog(minimum));
        spdlog::flush_on(spdlog::level::warn);
    } catch (const spdlog::spdlog_ex&) {
        // Already registered, or no console to attach to. Logging is not
        // worth failing a run over; the ring buffer still works.
    }
    set_level(minimum);
}

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case MS_LOG_DEBUG:
            return "debug";
        case MS_LOG_INFO:
            return "info";
        case MS_LOG_WARN:
            return "warn";
        case MS_LOG_ERROR:
            return "error";
    }
    return "?";
}

void set_level(LogLevel level) {
    g_level.store(level, std::memory_order_relaxed);
    spdlog::set_level(to_spdlog(level));
}

LogLevel level() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

void write(LogLevel level, std::string_view source, std::string_view message) {
    if (level < g_level.load(std::memory_order_relaxed)) {
        return;
    }

    Record record;
    record.ts = wall_clock_now();
    record.level = level;
    record.source.assign(source);
    record.message.assign(message);
    buffer().push(std::move(record));

    // The console sink as well, so a headless run and a CI failure still say
    // something without anyone having to open the GUI.
    if (g_initialised.load(std::memory_order_relaxed)) {
        spdlog::log(to_spdlog(level), "[{}] {}", source, message);
    }
}

void debug(std::string_view source, std::string_view message) {
    write(MS_LOG_DEBUG, source, message);
}

void info(std::string_view source, std::string_view message) {
    write(MS_LOG_INFO, source, message);
}

void warn(std::string_view source, std::string_view message) {
    write(MS_LOG_WARN, source, message);
}

void error(std::string_view source, std::string_view message) {
    write(MS_LOG_ERROR, source, message);
}

}  // namespace marketscan::log
