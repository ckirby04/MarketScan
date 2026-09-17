// Session boundaries, and the daylight-saving arithmetic behind them.
//
// The reason this file is long out of proportion to the header it tests: a
// session boundary that is quietly an hour out for eight months of the year
// produces a backtest that runs, reports plausible numbers, and dates a third
// of its decisions to the wrong day. Nothing downstream can detect that. The
// only place it can be caught is here, against dates worked out by hand.
//
// Every expected value below is stated as a civil date rather than a
// timestamp, and the timestamps are derived from it, so a reader can check the
// claim against a calendar instead of against this code.

#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/session_calendar.hpp"
#include "marketscan/core/types.hpp"

using namespace marketscan;
using calendar::civil_from_days;
using calendar::days_from_civil;
using calendar::kNanosPerDay;
using calendar::last_weekday_of;
using calendar::nth_weekday_of;
using calendar::us_daylight_saving;
using calendar::weekday_from_days;

namespace {

/// A UTC instant, built from the parts a person would read off a calendar.
[[nodiscard]] constexpr std::int64_t
utc(std::int64_t year, unsigned month, unsigned day, int hour = 0, int minute = 0) noexcept {
    return days_from_civil(year, month, day) * kNanosPerDay +
           (static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60) *
               calendar::kNanosPerSecond;
}

}  // namespace

// --------------------------------------------------------------------------
// Civil date arithmetic
// --------------------------------------------------------------------------

TEST_CASE("the epoch is day zero", "[calendar]") {
    STATIC_REQUIRE(days_from_civil(1970, 1, 1) == 0);
    STATIC_REQUIRE(weekday_from_days(0) == 4);  // 1970-01-01 was a Thursday
}

TEST_CASE("civil dates round trip", "[calendar]") {
    // Across leap years, century years and the 400-year rule, plus dates
    // before the epoch, where truncating division would go wrong.
    const std::int64_t years[] = {1899, 1900, 1996, 2000, 2024, 2100, 1969};
    for (std::int64_t year : years) {
        for (unsigned month = 1; month <= 12; ++month) {
            for (unsigned day = 1; day <= 28; ++day) {
                const std::int64_t days = days_from_civil(year, month, day);
                const calendar::CivilDate back = civil_from_days(days);
                INFO(year << "-" << month << "-" << day);
                REQUIRE(back.year == year);
                REQUIRE(back.month == month);
                REQUIRE(back.day == day);
            }
        }
    }
}

TEST_CASE("2024 is a leap year and 1900 is not", "[calendar]") {
    REQUIRE(days_from_civil(2024, 3, 1) - days_from_civil(2024, 2, 1) == 29);
    REQUIRE(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 1) == 28);
    REQUIRE(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 1) == 29);
}

TEST_CASE("nth and last weekday land on the right dates", "[calendar]") {
    // Second Sunday in March 2024 is the 10th; first Sunday in November is
    // the 3rd. Both are the 2024 daylight-saving transitions.
    REQUIRE(civil_from_days(nth_weekday_of(2024, 3, 0, 2)).day == 10);
    REQUIRE(civil_from_days(nth_weekday_of(2024, 11, 0, 1)).day == 3);

    // Last Sunday in October 2006 is the 29th. October 2006 began on a Sunday,
    // so it has five of them -- the case that catches an off-by-one.
    REQUIRE(civil_from_days(last_weekday_of(2006, 10, 0)).day == 29);
    REQUIRE(civil_from_days(last_weekday_of(2006, 10, 0)).month == 10);

    // February 2024 has exactly four Thursdays, so "last" must fall back.
    const std::int64_t last_thursday = last_weekday_of(2024, 2, 4);
    REQUIRE(civil_from_days(last_thursday).month == 2);
    REQUIRE(civil_from_days(last_thursday).day == 29);
}

// --------------------------------------------------------------------------
// Daylight saving
// --------------------------------------------------------------------------

TEST_CASE("the 2007 rules apply from 2007", "[calendar][dst]") {
    // Second Sunday in March 2024 at 02:00 EST = 07:00 UTC.
    REQUIRE_FALSE(us_daylight_saving(utc(2024, 3, 10, 6, 59)));
    REQUIRE(us_daylight_saving(utc(2024, 3, 10, 7, 0)));

    // First Sunday in November 2024 at 02:00 EDT = 06:00 UTC.
    REQUIRE(us_daylight_saving(utc(2024, 11, 3, 5, 59)));
    REQUIRE_FALSE(us_daylight_saving(utc(2024, 11, 3, 6, 0)));

    // March 2007 is the first year of the current rule. Under the old one,
    // the 11th would still have been standard time.
    REQUIRE(us_daylight_saving(utc(2007, 3, 11, 7, 0)));
}

TEST_CASE("the 1987 rules apply from 1987 to 2006", "[calendar][dst]") {
    // First Sunday in April 2006 was the 2nd; under the later rule March
    // would already have been daylight time.
    REQUIRE_FALSE(us_daylight_saving(utc(2006, 3, 20)));
    REQUIRE_FALSE(us_daylight_saving(utc(2006, 4, 2, 6, 59)));
    REQUIRE(us_daylight_saving(utc(2006, 4, 2, 7, 0)));

    // Last Sunday in October 2006 was the 29th; under the later rule the
    // first days of November would still have been daylight time.
    REQUIRE(us_daylight_saving(utc(2006, 10, 29, 5, 59)));
    REQUIRE_FALSE(us_daylight_saving(utc(2006, 10, 29, 6, 0)));
    REQUIRE_FALSE(us_daylight_saving(utc(2006, 11, 1)));

    // 1996, the earliest year the S&P 500 membership history reaches.
    REQUIRE_FALSE(us_daylight_saving(utc(1996, 4, 7, 6, 59)));
    REQUIRE(us_daylight_saving(utc(1996, 4, 7, 7, 0)));
}

TEST_CASE("the 1976 rules apply before 1987", "[calendar][dst]") {
    // Last Sunday in April 1986 was the 27th.
    REQUIRE_FALSE(us_daylight_saving(utc(1986, 4, 27, 6, 59)));
    REQUIRE(us_daylight_saving(utc(1986, 4, 27, 7, 0)));
    // The first Sunday, the 6th, is daylight time only under the later rule.
    REQUIRE_FALSE(us_daylight_saving(utc(1986, 4, 6, 12)));
}

TEST_CASE("winter is standard time and summer is not, every year", "[calendar][dst]") {
    for (std::int64_t year = 1980; year <= 2035; ++year) {
        INFO(year);
        REQUIRE_FALSE(us_daylight_saving(utc(year, 1, 15, 12)));
        REQUIRE(us_daylight_saving(utc(year, 7, 15, 12)));
        REQUIRE_FALSE(us_daylight_saving(utc(year, 12, 15, 12)));
    }
}

// --------------------------------------------------------------------------
// Offsets and session boundaries
// --------------------------------------------------------------------------

TEST_CASE("US Eastern is five hours behind in winter and four in summer", "[calendar]") {
    REQUIRE(utc_offset_seconds(ExchangeZone::UsEastern, utc(2024, 1, 15, 12)) == -5 * 3600);
    REQUIRE(utc_offset_seconds(ExchangeZone::UsEastern, utc(2024, 7, 15, 12)) == -4 * 3600);
    REQUIRE(utc_offset_seconds(ExchangeZone::Utc, utc(2024, 7, 15, 12)) == 0);
}

// The property the specification asks for by name: not UTC midnight.
TEST_CASE("a session rolls at exchange midnight, not UTC midnight", "[calendar][session]") {
    // 03:30 UTC on the 19th is 23:30 on the 18th in New York. A UTC-based
    // boundary calls this a new session; an exchange-based one does not.
    const std::int64_t late_evening = utc(2024, 3, 19, 3, 30);
    const std::int64_t afternoon = utc(2024, 3, 18, 19, 0);

    REQUIRE(same_session(ExchangeZone::UsEastern, afternoon, late_evening));
    REQUIRE_FALSE(same_session(ExchangeZone::Utc, afternoon, late_evening));

    // And half an hour later it is a new session in both.
    const std::int64_t after_midnight = utc(2024, 3, 19, 4, 30);
    REQUIRE_FALSE(same_session(ExchangeZone::UsEastern, afternoon, after_midnight));
}

TEST_CASE("a session ends at the next local midnight", "[calendar][session]") {
    // Midnight on 2024-03-19 in New York is 04:00 UTC, because March 19th is
    // daylight time.
    REQUIRE(session_end(ExchangeZone::UsEastern, utc(2024, 3, 18, 13, 30)) ==
            utc(2024, 3, 19, 4, 0));

    // In January it is 05:00 UTC.
    REQUIRE(session_end(ExchangeZone::UsEastern, utc(2024, 1, 10, 13, 30)) ==
            utc(2024, 1, 11, 5, 0));

    REQUIRE(session_end(ExchangeZone::Utc, utc(2024, 3, 18, 13, 30)) == utc(2024, 3, 19, 0, 0));
}

TEST_CASE("the boundary belongs to the session it opens", "[calendar][session]") {
    const std::int64_t boundary = session_end(ExchangeZone::UsEastern, utc(2024, 6, 10, 15));

    REQUIRE_FALSE(same_session(ExchangeZone::UsEastern, boundary - 1, boundary));
    REQUIRE(same_session(ExchangeZone::UsEastern, boundary, boundary + 1));
}

// If these two are wrong, every boundary in the surrounding week is wrong too.
TEST_CASE("the day the clocks go forward is 23 hours long", "[calendar][session][dst]") {
    const std::int64_t starts = session_start_of_day(
        ExchangeZone::UsEastern, session_day(ExchangeZone::UsEastern, utc(2024, 3, 10, 12)));
    const std::int64_t ends = session_end(ExchangeZone::UsEastern, utc(2024, 3, 10, 12));

    REQUIRE(starts == utc(2024, 3, 10, 5, 0));  // midnight EST
    REQUIRE(ends == utc(2024, 3, 11, 4, 0));    // midnight EDT
    REQUIRE(ends - starts == 23LL * 3600 * calendar::kNanosPerSecond);
}

TEST_CASE("the day the clocks go back is 25 hours long", "[calendar][session][dst]") {
    const std::int64_t starts = session_start_of_day(
        ExchangeZone::UsEastern, session_day(ExchangeZone::UsEastern, utc(2024, 11, 3, 12)));
    const std::int64_t ends = session_end(ExchangeZone::UsEastern, utc(2024, 11, 3, 12));

    REQUIRE(starts == utc(2024, 11, 3, 4, 0));  // midnight EDT
    REQUIRE(ends == utc(2024, 11, 4, 5, 0));    // midnight EST
    REQUIRE(ends - starts == 25LL * 3600 * calendar::kNanosPerSecond);
}

TEST_CASE("every day of a decade is between 23 and 25 hours", "[calendar][session][dst]") {
    // A sweep rather than a spot check, because an arithmetic error in the
    // offset would produce a day of some absurd length on exactly one date a
    // year, and spot checks are chosen from the dates one already suspects.
    const std::int64_t first = session_day(ExchangeZone::UsEastern, utc(2015, 1, 1, 12));
    const std::int64_t last = session_day(ExchangeZone::UsEastern, utc(2025, 1, 1, 12));

    std::int64_t short_days = 0;
    std::int64_t long_days = 0;
    for (std::int64_t day = first; day < last; ++day) {
        const std::int64_t length = session_start_of_day(ExchangeZone::UsEastern, day + 1) -
                                    session_start_of_day(ExchangeZone::UsEastern, day);
        INFO("day " << day);
        REQUIRE(length >= 23LL * 3600 * calendar::kNanosPerSecond);
        REQUIRE(length <= 25LL * 3600 * calendar::kNanosPerSecond);
        short_days += length < 24LL * 3600 * calendar::kNanosPerSecond ? 1 : 0;
        long_days += length > 24LL * 3600 * calendar::kNanosPerSecond ? 1 : 0;
    }

    // Exactly one of each per year, and the test would pass vacuously if the
    // offset were constant, so this is the part that matters.
    REQUIRE(short_days == 10);
    REQUIRE(long_days == 10);
}

TEST_CASE("consecutive instants never move the session backwards", "[calendar][session]") {
    // Walked in six-hour steps across both 2024 transitions. A session day
    // that ever decreased would let a strategy see the same day twice.
    std::int64_t previous = session_day(ExchangeZone::UsEastern, utc(2024, 3, 1));
    for (std::int64_t ts = utc(2024, 3, 1); ts < utc(2024, 12, 1);
         ts += 6LL * 3600 * calendar::kNanosPerSecond) {
        const std::int64_t day = session_day(ExchangeZone::UsEastern, ts);
        REQUIRE(day >= previous);
        REQUIRE(day - previous <= 1);
        previous = day;
    }
}

TEST_CASE("session arithmetic works before the epoch", "[calendar][session]") {
    // Not because anything replays 1969, but because floor division is the
    // kind of thing that is wrong only on the side nobody tested.
    const std::int64_t evening = utc(1969, 12, 31, 20, 0);  // 15:00 EST, the 31st
    const std::int64_t morning = utc(1970, 1, 1, 14, 0);    // 09:00 EST, the 1st

    REQUIRE_FALSE(same_session(ExchangeZone::UsEastern, evening, morning));
    REQUIRE(session_day(ExchangeZone::UsEastern, morning) -
                session_day(ExchangeZone::UsEastern, evening) ==
            1);
}
