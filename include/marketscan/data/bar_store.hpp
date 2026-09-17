#ifndef MARKETSCAN_DATA_BAR_STORE_HPP
#define MARKETSCAN_DATA_BAR_STORE_HPP

/*
 * The on-disk format for cached daily bars.
 *
 * WHY NOT CSV
 *
 * Twenty years of daily bars across five hundred names is about two and a half
 * million rows. As CSV that is roughly 150MB of text to parse on every run, and
 * parsing it is not incidental work: a decimal string has to be turned into a
 * double, and doing that two and a half million times is most of the time a
 * replay takes before a single strategy has been asked anything.
 *
 * The same data here is a header and a packed array of fixed-size records,
 * read in one pass with no parsing at all. The cost is that it is not
 * greppable, which is why the fetcher also writes a manifest in plain text and
 * why `marketscan --dump` exists.
 *
 * THE FORMAT
 *
 *   offset  size  field
 *        0     4  magic, "MSB1"
 *        4     4  format version
 *        8     8  record count
 *       16    32  ticker, NUL-padded, as fetched
 *       48     8  interval in nanoseconds (86400e9 for daily)
 *       56     8  reserved, zero
 *   --- 64-byte header, then `count` records of 48 bytes ---
 *        0     8  timestamp, nanoseconds since the Unix epoch
 *        8     8  open
 *       16     8  high
 *       24     8  low
 *       32     8  close
 *       40     8  volume
 *
 * Little-endian and IEEE-754, which is every platform this builds for. Both
 * assumptions are asserted at load rather than assumed: a file written on one
 * machine and read on another that disagreed would not fail, it would produce
 * prices.
 *
 * WHAT A DAILY TIMESTAMP MEANS
 *
 * A daily bar covers a session, and a session is not an instant. It is stamped
 * at the session **close** -- 16:00 in the exchange's own time zone, converted
 * to UTC through the same calendar the session boundaries use, so the stamp
 * moves with daylight saving exactly as the closing bell does.
 *
 * The close, not the open, because that is when the information in the bar
 * exists. A strategy handed a daily bar stamped at the open would be able to
 * act at 09:30 on a high, low and close that had not happened yet, and would
 * look extremely clever.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "marketscan/core/session_calendar.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

/// One bar, as stored. Deliberately not MS_Bar: this has no symbol id, because
/// ids are assigned per run and a file outlives the run that wrote it.
struct StoredBar {
    Timestamp ts = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

static_assert(sizeof(StoredBar) == 48, "StoredBar is written to disk; its layout is the format");

/// A bar file: its ticker, its cadence, and its bars in ascending time order.
struct BarSeries {
    std::string ticker;
    Duration interval = 24 * 60 * 60 * 1'000'000'000LL;
    std::vector<StoredBar> bars;

    [[nodiscard]] bool empty() const noexcept { return bars.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return bars.size(); }
};

namespace bar_store {

inline constexpr std::uint32_t kMagic = 0x3142'534DU;  // "MSB1" little-endian
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 64;
inline constexpr std::size_t kTickerBytes = 32;
inline constexpr Duration kDailyInterval = 24 * 60 * 60 * 1'000'000'000LL;

/// The conventional stamp for a daily bar: the closing bell, in UTC.
///
/// `day` is a local calendar day number in `zone`, as session_day returns.
[[nodiscard]] Timestamp daily_close_stamp(std::int64_t day,
                                          ExchangeZone zone = ExchangeZone::UsEastern,
                                          int close_hour = 16,
                                          int close_minute = 0) noexcept;

/// Writes a series. Throws std::runtime_error if the bars are out of order,
/// the ticker does not fit, or the file cannot be written.
///
/// Out-of-order bars are refused rather than sorted: a writer that produced
/// them has misunderstood its input, and silently sorting would hide that
/// while leaving whatever else it misunderstood in place.
void write(const std::filesystem::path& path, const BarSeries& series);

/// Reads a series. Throws std::runtime_error naming the file for a bad magic,
/// an unknown version, a truncated file, or bars out of order.
[[nodiscard]] BarSeries read(const std::filesystem::path& path);

/// Ticker and bar count without reading the bars. For listing a cache.
struct Summary {
    std::string ticker;
    std::uint64_t bars = 0;
    Duration interval = 0;
    Timestamp first = 0;
    Timestamp last = 0;
};

[[nodiscard]] Summary summarise(const std::filesystem::path& path);

/// Every .msb file in `directory`, in sorted filename order.
///
/// Sorted, because the order files are enumerated in is the order their
/// symbols get interned, and therefore the order symbol ids are assigned --
/// which reaches the tie-break between two bars at the same timestamp. A
/// directory listing is not a promise about order on any filesystem.
[[nodiscard]] std::vector<std::filesystem::path> list(const std::filesystem::path& directory);

inline constexpr const char* kExtension = ".msb";

}  // namespace bar_store

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_BAR_STORE_HPP
