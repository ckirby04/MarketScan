#ifndef MARKETSCAN_LIVE_CALENDAR_HPP
#define MARKETSCAN_LIVE_CALENDAR_HPP

/*
 * When the market is open, according to the market.
 *
 * WHY NOT JUST HARD-CODE 09:30 TO 16:00
 *
 * Because it is wrong about eleven days a year, and wrong in the direction
 * that costs money: half days close at 13:00, holidays do not open at all, and
 * the dates move. A runner that assumed the regular session would submit into
 * a closed market on the Friday after Thanksgiving, get its orders queued for
 * the next open, and then be surprised by fills hours later against prices it
 * never saw.
 *
 * So the schedule is fetched, from the same broker that will accept the
 * orders. If the two disagree, the broker is right by definition.
 *
 * Fetched once for a window and then consulted offline, because a calendar
 * that hits the network per bar is a calendar that gets rate limited.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/core/types.hpp"
#include "marketscan/live/http.hpp"

namespace marketscan::live {

/// One trading session.
struct TradingSession {
    /// The session date, as the broker spells it: "2026-09-21".
    std::string date;

    /// Regular session boundaries, as absolute timestamps.
    Timestamp open = 0;
    Timestamp close = 0;

    /// True when the close is earlier than the regular one -- the day after
    /// Thanksgiving, Christmas Eve. Worth knowing because a strategy that
    /// flattens before the close needs to flatten earlier on these.
    bool half_day = false;
};

/// A window of sessions.
class MarketCalendar {
public:
    MarketCalendar() = default;

    /// Fetches sessions covering [start, end], both "YYYY-MM-DD".
    ///
    /// Returns false on any failure, including a rate limit; `error()` says
    /// which. Nothing here retries -- see docs/DATA_POLICY.md.
    [[nodiscard]] bool fetch(HttpClient& client, const std::string& start, const std::string& end);

    /// Parses a calendar response body. Split out so a test can exercise it
    /// without a network.
    [[nodiscard]] bool parse(const std::string& body);

    [[nodiscard]] const std::vector<TradingSession>& sessions() const noexcept { return sessions_; }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    [[nodiscard]] bool empty() const noexcept { return sessions_.empty(); }

    /// The session containing `ts`, or nullptr when the market is shut.
    [[nodiscard]] const TradingSession* session_at(Timestamp ts) const noexcept;

    /// The first session that opens at or after `ts`, or nullptr when the
    /// fetched window does not extend that far. A null result means "look
    /// further", never "never again".
    [[nodiscard]] const TradingSession* next_session(Timestamp ts) const noexcept;

    [[nodiscard]] bool is_open(Timestamp ts) const noexcept { return session_at(ts) != nullptr; }

    /// Whether the fetched window covers `ts` at all. Distinguishes "the
    /// market is closed" from "this calendar cannot say", which are different
    /// answers and must not both be reported as closed.
    [[nodiscard]] bool covers(Timestamp ts) const noexcept;

private:
    std::vector<TradingSession> sessions_;
    std::string error_;
};

/// The exchange's UTC offset on `date`, in seconds, as US eastern time.
///
/// The calendar response gives local wall-clock times with no zone, so
/// something has to know that 09:30 in New York is 13:30 UTC in July and 14:30
/// in January. This implements the post-2007 US rule -- second Sunday in
/// March to first Sunday in November -- rather than reaching for a timezone
/// database the project does not otherwise need.
///
/// Returns 0 and leaves `out` untouched if `date` is not "YYYY-MM-DD".
[[nodiscard]] bool eastern_utc_offset(const std::string& date, int& out) noexcept;

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_CALENDAR_HPP
