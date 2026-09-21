#include "marketscan/live/calendar.hpp"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "marketscan/util/logging.hpp"

namespace marketscan::live {
namespace {

constexpr std::int64_t kNanosPerSecond = 1'000'000'000LL;

/// Days from 1970-01-01 to y-m-d, proleptic Gregorian. Howard Hinnant's
/// algorithm, the same one core/types.cpp uses; duplicated rather than exposed
/// because making it public would invite it to be used for arithmetic the
/// engine deliberately does in nanoseconds.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] bool parse_date(const std::string& text, int& y, unsigned& m, unsigned& d) noexcept {
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (std::sscanf(text.c_str(), "%4d-%2u-%2u", &year, &month, &day) != 3) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    y = year;
    m = month;
    d = day;
    return true;
}

/// "HH:MM" or "HH:MM:SS" as seconds past local midnight.
[[nodiscard]] bool parse_time_of_day(const std::string& text, int& out) noexcept {
    int hour = 0;
    int minute = 0;
    int second = 0;
    const int fields = std::sscanf(text.c_str(), "%2d:%2d:%2d", &hour, &minute, &second);
    if (fields < 2) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    out = hour * 3600 + minute * 60 + second;
    return true;
}

/// Day-of-month of the `n`th `weekday` in `month`. Sunday is 0.
[[nodiscard]] unsigned nth_weekday(int year, unsigned month, unsigned weekday, int n) noexcept {
    const std::int64_t first = days_from_civil(year, month, 1);
    // 1970-01-01 was a Thursday.
    const unsigned first_dow = static_cast<unsigned>(((first % 7) + 11) % 7);
    const unsigned delta = (weekday + 7U - first_dow) % 7U;
    return 1U + delta + static_cast<unsigned>(n - 1) * 7U;
}

}  // namespace

bool eastern_utc_offset(const std::string& date, int& out) noexcept {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!parse_date(date, year, month, day)) {
        return false;
    }

    // Second Sunday in March to first Sunday in November, the rule since 2007.
    const unsigned dst_start = nth_weekday(year, 3, 0, 2);
    const unsigned dst_end = nth_weekday(year, 11, 0, 1);

    const std::int64_t today = days_from_civil(year, month, day);
    const std::int64_t start = days_from_civil(year, 3, dst_start);
    const std::int64_t end = days_from_civil(year, 11, dst_end);

    // The changeover happens at 02:00 local, before any session opens, so a
    // date-level comparison is exact for market hours. The first Sunday in
    // November is standard time by 09:30 and the market is shut anyway.
    const bool daylight = today >= start && today < end;
    out = daylight ? -4 * 3600 : -5 * 3600;
    return true;
}

bool MarketCalendar::parse(const std::string& body) {
    sessions_.clear();
    error_.clear();

    const nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
    if (doc.is_discarded()) {
        error_ = "calendar response was not valid JSON";
        return false;
    }
    if (!doc.is_array()) {
        error_ = "calendar response was not a JSON array";
        return false;
    }

    for (const nlohmann::json& element : doc) {
        if (!element.is_object()) {
            continue;
        }
        TradingSession session;
        session.date = element.value("date", std::string{});
        const std::string open = element.value("open", std::string{});
        const std::string close = element.value("close", std::string{});
        if (session.date.empty() || open.empty() || close.empty()) {
            error_ = "calendar entry missing date, open or close";
            sessions_.clear();
            return false;
        }

        int offset = 0;
        int open_seconds = 0;
        int close_seconds = 0;
        if (!eastern_utc_offset(session.date, offset) || !parse_time_of_day(open, open_seconds) ||
            !parse_time_of_day(close, close_seconds)) {
            error_ = "calendar entry for " + session.date + " is not parseable";
            sessions_.clear();
            return false;
        }

        int year = 0;
        unsigned month = 0;
        unsigned day = 0;
        (void)parse_date(session.date, year, month, day);
        const std::int64_t midnight_utc = days_from_civil(year, month, day) * 86400LL;

        session.open = (midnight_utc + open_seconds - offset) * kNanosPerSecond;
        session.close = (midnight_utc + close_seconds - offset) * kNanosPerSecond;
        session.half_day = close_seconds < 16 * 3600;

        if (session.close <= session.open) {
            error_ = "calendar entry for " + session.date + " closes before it opens";
            sessions_.clear();
            return false;
        }
        sessions_.push_back(std::move(session));
    }

    std::sort(sessions_.begin(),
              sessions_.end(),
              [](const TradingSession& a, const TradingSession& b) { return a.open < b.open; });
    return true;
}

bool MarketCalendar::fetch(HttpClient& client, const std::string& start, const std::string& end) {
    sessions_.clear();
    error_.clear();

    const HttpResponse response = client.get("/v2/calendar?start=" + start + "&end=" + end);
    if (response.rate_limited()) {
        error_ = "calendar request was rate limited; retry after " +
                 std::to_string(response.retry_after_seconds) + "s";
        log::write(MS_LOG_WARN, "calendar", error_ + ". Not retrying -- see docs/DATA_POLICY.md.");
        return false;
    }
    if (response.unauthorised()) {
        error_ = "calendar request was refused: check APCA_API_KEY_ID and APCA_API_SECRET_KEY";
        return false;
    }
    if (!response.ok()) {
        error_ = response.error.empty()
                     ? "calendar request returned status " + std::to_string(response.status)
                     : response.error;
        return false;
    }
    return parse(response.body);
}

const TradingSession* MarketCalendar::session_at(Timestamp ts) const noexcept {
    for (const TradingSession& session : sessions_) {
        if (ts >= session.open && ts < session.close) {
            return &session;
        }
    }
    return nullptr;
}

const TradingSession* MarketCalendar::next_session(Timestamp ts) const noexcept {
    for (const TradingSession& session : sessions_) {
        if (session.open >= ts) {
            return &session;
        }
    }
    return nullptr;
}

bool MarketCalendar::covers(Timestamp ts) const noexcept {
    if (sessions_.empty()) {
        return false;
    }
    // The window runs from the first session's midnight to the last session's
    // close. A timestamp outside it is one this calendar has nothing to say
    // about, which is not the same as one the market was closed for.
    return ts >= sessions_.front().open - 86400LL * kNanosPerSecond && ts <= sessions_.back().close;
}

}  // namespace marketscan::live
