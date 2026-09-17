#ifndef MARKETSCAN_CORE_SESSION_CALENDAR_HPP
#define MARKETSCAN_CORE_SESSION_CALENDAR_HPP

/*
 * When one trading session ends and the next begins.
 *
 * A cross-sectional strategy needs to act once per day, on everything at
 * once, at a defined instant. That instant is local midnight at the exchange,
 * which is not UTC midnight and is not a fixed offset from it either: the
 * United States moves its clocks twice a year, and a session boundary that
 * ignores that is wrong for half the calendar.
 *
 * This header computes those boundaries from rules compiled into it, rather
 * than from the operating system's time zone database. That is a deliberate
 * choice and it is the same one made for the random number generator. The IANA
 * database is updated when governments change their minds, Windows ships those
 * updates, and a replay whose session boundaries move when the machine takes a
 * patch is not reproducible. Rules in the source are inspectable, diffable and
 * identical on every machine forever.
 *
 * The cost is that the rules have to be maintained by hand and cover only what
 * is written here. Both are stated rather than hidden: see kEarliestSupported
 * and the note on each rule's validity.
 *
 * Header-only and dependent on nothing at all, because a theory plugin
 * compiles this and a theory plugin links against nothing.
 */

#include <cstdint>

namespace marketscan {

/// Which wall clock a session's boundaries are measured against.
///
/// Not an open-ended time zone type. An exchange calendar is a small, closed
/// set of things the engine actually replays, and an enumeration keeps this
/// header free of any dependency -- which is what lets a plugin compile it.
enum class ExchangeZone : std::uint8_t {
    /// Sessions roll at UTC midnight. Correct for a venue that quotes in UTC,
    /// and useful as a control in tests.
    Utc,

    /// US Eastern: UTC-5, or UTC-4 while daylight saving is in effect. The
    /// zone for NYSE and Nasdaq, and the default.
    UsEastern,
};

namespace calendar {

inline constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
inline constexpr std::int64_t kSecondsPerDay = 86'400;
inline constexpr std::int64_t kNanosPerDay = kSecondsPerDay * kNanosPerSecond;

/// The first year whose daylight-saving transitions this header gets right.
///
/// The United States has changed its rules repeatedly. Three regimes are
/// implemented, covering 1976 onward; before 1976 the 1976 rule is applied,
/// which is wrong for the 1974-75 emergency year-round daylight time and for
/// the patchwork before 1966. No dataset this project handles reaches back
/// that far, and a caller that finds one should read this comment rather than
/// trust the answer.
inline constexpr int kEarliestSupported = 1976;

/// Days from 1970-01-01 to the given civil date, proleptic Gregorian.
///
/// Howard Hinnant's algorithm. Correct for any date the return type can hold,
/// branch-free, and constexpr -- which matters because the tests want to state
/// boundaries as dates rather than as magic numbers.
[[nodiscard]] constexpr std::int64_t
days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146'097 + static_cast<std::int64_t>(day_of_era) - 719'468;
}

struct CivilDate {
    std::int64_t year = 1970;
    unsigned month = 1;
    unsigned day = 1;
};

/// The inverse of days_from_civil.
[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
    days += 719'468;
    const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
    const auto day_of_era = static_cast<std::int64_t>(days - era * 146'097);
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
    const std::int64_t year = year_of_era + era * 400;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t mp = (5 * day_of_year + 2) / 153;
    const auto day = static_cast<unsigned>(day_of_year - (153 * mp + 2) / 5 + 1);
    const auto month = static_cast<unsigned>(mp + (mp < 10 ? 3 : -9));
    return CivilDate{year + (month <= 2 ? 1 : 0), month, day};
}

/// 0 = Sunday. Valid for negative day numbers too.
[[nodiscard]] constexpr unsigned weekday_from_days(std::int64_t days) noexcept {
    return static_cast<unsigned>(days >= -4 ? (days + 4) % 7 : (days + 5) % 7 + 6);
}

/// Day number of the nth given weekday in a month; n counts from 1.
[[nodiscard]] constexpr std::int64_t
nth_weekday_of(std::int64_t year, unsigned month, unsigned weekday, unsigned n) noexcept {
    const std::int64_t first = days_from_civil(year, month, 1);
    const unsigned first_weekday = weekday_from_days(first);
    const unsigned offset = (weekday + 7 - first_weekday) % 7;
    return first + static_cast<std::int64_t>(offset) + 7 * static_cast<std::int64_t>(n - 1);
}

/// Day number of the last given weekday in a month.
[[nodiscard]] constexpr std::int64_t
last_weekday_of(std::int64_t year, unsigned month, unsigned weekday) noexcept {
    // The 5th occurrence if the month has one, otherwise the 4th. Cheaper than
    // a table of month lengths and correct in February of a leap year.
    const std::int64_t fifth = nth_weekday_of(year, month, weekday, 5);
    const CivilDate date = civil_from_days(fifth);
    return date.month == month ? fifth : fifth - 7;
}

/// Whether US daylight saving time is in effect at a given UTC instant.
///
/// Three regimes, each stated with the law that produced it:
///
///   1976-1986  last Sunday in April  -> last Sunday in October
///   1987-2006  first Sunday in April -> last Sunday in October
///   2007-      second Sunday in March -> first Sunday in November
///
/// Transitions happen at 02:00 local, which is 07:00 UTC entering (from
/// standard time, UTC-5) and 06:00 UTC leaving (from daylight time, UTC-4).
/// Comparing in UTC avoids the ambiguity of the hour that happens twice.
[[nodiscard]] constexpr bool us_daylight_saving(std::int64_t ts_nanos) noexcept {
    const std::int64_t utc_day =
        (ts_nanos >= 0 ? ts_nanos : ts_nanos - (kNanosPerDay - 1)) / kNanosPerDay;
    const std::int64_t year = civil_from_days(utc_day).year;

    std::int64_t start_day = 0;
    std::int64_t end_day = 0;
    if (year >= 2007) {
        start_day = nth_weekday_of(year, 3, 0, 2);
        end_day = nth_weekday_of(year, 11, 0, 1);
    } else if (year >= 1987) {
        start_day = nth_weekday_of(year, 4, 0, 1);
        end_day = last_weekday_of(year, 10, 0);
    } else {
        start_day = last_weekday_of(year, 4, 0);
        end_day = last_weekday_of(year, 10, 0);
    }

    const std::int64_t start = start_day * kNanosPerDay + 7 * 3600 * kNanosPerSecond;
    const std::int64_t end = end_day * kNanosPerDay + 6 * 3600 * kNanosPerSecond;
    return ts_nanos >= start && ts_nanos < end;
}

}  // namespace calendar

/// Seconds east of UTC in `zone` at the instant `ts_nanos`. Negative in the
/// Americas.
[[nodiscard]] constexpr std::int32_t utc_offset_seconds(ExchangeZone zone,
                                                        std::int64_t ts_nanos) noexcept {
    switch (zone) {
        case ExchangeZone::Utc:
            return 0;
        case ExchangeZone::UsEastern:
            return calendar::us_daylight_saving(ts_nanos) ? -4 * 3600 : -5 * 3600;
    }
    return 0;
}

/// The local calendar day containing `ts_nanos`, as a day number.
///
/// The number itself means "days since the Unix epoch in local terms" and is
/// not useful on its own. What is useful is that it changes exactly when the
/// session does, which is the only thing anything here asks of it.
[[nodiscard]] constexpr std::int64_t session_day(ExchangeZone zone,
                                                 std::int64_t ts_nanos) noexcept {
    const std::int64_t local =
        ts_nanos +
        static_cast<std::int64_t>(utc_offset_seconds(zone, ts_nanos)) * calendar::kNanosPerSecond;
    // Floor division: C++ truncates toward zero, which would put every instant
    // in the twenty-four hours before the epoch into day 0 along with the day
    // after it.
    return local >= 0 ? local / calendar::kNanosPerDay
                      : (local - (calendar::kNanosPerDay - 1)) / calendar::kNanosPerDay;
}

/// The UTC instant at which local day `day` begins in `zone`.
///
/// Iterated once, because the offset depends on the answer: converting local
/// midnight to UTC needs the offset in effect at that local midnight, and the
/// offset is looked up by UTC instant. One correction settles it for every
/// transition that does not happen at midnight, and no United States
/// transition ever has.
[[nodiscard]] constexpr std::int64_t session_start_of_day(ExchangeZone zone,
                                                          std::int64_t day) noexcept {
    const std::int64_t local_midnight = day * calendar::kNanosPerDay;
    std::int64_t guess =
        local_midnight - static_cast<std::int64_t>(utc_offset_seconds(zone, local_midnight)) *
                             calendar::kNanosPerSecond;
    guess = local_midnight -
            static_cast<std::int64_t>(utc_offset_seconds(zone, guess)) * calendar::kNanosPerSecond;
    return guess;
}

/// The instant at which the session containing `ts_nanos` ends, exclusive.
///
/// Exclusive because a boundary belongs to exactly one side of itself, and
/// putting it on the later side means the first event of a new session is
/// never mistaken for the last of the old one.
[[nodiscard]] constexpr std::int64_t session_end(ExchangeZone zone,
                                                 std::int64_t ts_nanos) noexcept {
    return session_start_of_day(zone, session_day(zone, ts_nanos) + 1);
}

/// Whether two instants fall in the same session.
[[nodiscard]] constexpr bool
same_session(ExchangeZone zone, std::int64_t a, std::int64_t b) noexcept {
    return session_day(zone, a) == session_day(zone, b);
}

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_SESSION_CALENDAR_HPP
