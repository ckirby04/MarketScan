#include "marketscan/core/types.hpp"

#include <cstdio>

namespace marketscan::time {
namespace {

// Howard Hinnant's civil calendar algorithms. Used in preference to gmtime
// because they are pure, branch-stable and identical on every platform: the
// engine must format the same timestamp the same way in every run, and the C
// library's time functions are neither thread safe nor locale independent.
//
// Days are counted from 1970-01-01.

constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);                // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct CivilDate {
    std::int64_t year;
    unsigned month;
    unsigned day;
};

constexpr CivilDate civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned long>(z - era * 146097);                    // [0, 146096]
    const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    const unsigned long mp = (5 * doy + 2) / 153;                       // [0, 11]
    const unsigned long d = doy - (153 * mp + 2) / 5 + 1;               // [1, 31]
    const unsigned long m = mp + (mp < 10 ? 3 : -9);                    // [1, 12]
    return CivilDate{y + (m <= 2), static_cast<unsigned>(m), static_cast<unsigned>(d)};
}

/// Floor division and modulo, so that pre-epoch timestamps do not round toward
/// zero and land on the wrong day.
constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

constexpr std::int64_t floor_mod(std::int64_t a, std::int64_t b) noexcept {
    return a - floor_div(a, b) * b;
}

/// Reads exactly `digits` decimal characters. Refuses anything shorter, so a
/// malformed field is a parse failure rather than a plausible wrong number.
bool read_fixed(std::string_view& text, std::size_t digits, std::int64_t& out) noexcept {
    if (text.size() < digits) {
        return false;
    }
    std::int64_t value = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    text.remove_prefix(digits);
    out = value;
    return true;
}

bool consume(std::string_view& text, char c) noexcept {
    if (text.empty() || text.front() != c) {
        return false;
    }
    text.remove_prefix(1);
    return true;
}

constexpr bool is_leap(std::int64_t y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

constexpr unsigned days_in_month(std::int64_t y, unsigned m) noexcept {
    constexpr unsigned kLengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap(y)) {
        return 29;
    }
    return kLengths[m - 1];
}

}  // namespace

std::string to_iso8601(Timestamp ts) {
    const std::int64_t days = floor_div(ts, kDay);
    const std::int64_t rem = floor_mod(ts, kDay);

    const CivilDate date = civil_from_days(days);
    const std::int64_t hour = rem / kHour;
    const std::int64_t minute = (rem % kHour) / kMinute;
    const std::int64_t second = (rem % kMinute) / kSecond;
    const std::int64_t nanos = rem % kSecond;

    char buf[64];
    const int n = std::snprintf(buf,
                                sizeof buf,
                                "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%09lldZ",
                                static_cast<long long>(date.year),
                                date.month,
                                date.day,
                                static_cast<long long>(hour),
                                static_cast<long long>(minute),
                                static_cast<long long>(second),
                                static_cast<long long>(nanos));
    return std::string(buf, static_cast<std::size_t>(n < 0 ? 0 : n));
}

bool from_iso8601(std::string_view text, Timestamp& out) {
    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;

    if (!read_fixed(text, 4, year) || !consume(text, '-')) {
        return false;
    }
    if (!read_fixed(text, 2, month) || !consume(text, '-')) {
        return false;
    }
    if (!read_fixed(text, 2, day)) {
        return false;
    }
    if (month < 1 || month > 12) {
        return false;
    }
    if (day < 1 || day > days_in_month(year, static_cast<unsigned>(month))) {
        return false;
    }

    std::int64_t hour = 0;
    std::int64_t minute = 0;
    std::int64_t second = 0;
    std::int64_t nanos = 0;

    // A bare date is midnight UTC. Anything else needs a 'T' or a space.
    if (!text.empty()) {
        if (!consume(text, 'T') && !consume(text, ' ')) {
            return false;
        }
        if (!read_fixed(text, 2, hour) || !consume(text, ':')) {
            return false;
        }
        if (!read_fixed(text, 2, minute)) {
            return false;
        }
        // Seconds are optional: "13:30" is a timestamp people actually write.
        if (consume(text, ':')) {
            if (!read_fixed(text, 2, second)) {
                return false;
            }
            if (consume(text, '.')) {
                // Accept any number of fractional digits; scale to nanoseconds
                // and truncate beyond nine rather than rounding, so a replay
                // never reorders two events that differ below the resolution
                // the rest of the engine can represent.
                std::size_t digits = 0;
                while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
                    if (digits < 9) {
                        nanos = nanos * 10 + (text[digits] - '0');
                    }
                    ++digits;
                }
                if (digits == 0) {
                    return false;
                }
                for (std::size_t i = digits; i < 9; ++i) {
                    nanos *= 10;
                }
                text.remove_prefix(digits);
            }
        }
    }

    // Leap seconds are not representable as a Unix timestamp; reject rather
    // than silently folding 23:59:60 onto the following second.
    if (hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    // A trailing 'Z' is allowed and means what the whole function already
    // assumes. A numeric offset is not: honouring it silently would make the
    // same file parse differently depending on where it was written.
    (void)consume(text, 'Z');
    if (!text.empty()) {
        return false;
    }

    const std::int64_t days =
        days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    out = days * kDay + hour * kHour + minute * kMinute + second * kSecond + nanos;
    return true;
}

}  // namespace marketscan::time
