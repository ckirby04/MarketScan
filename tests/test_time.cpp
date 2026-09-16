#include <string>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/types.hpp"

using namespace marketscan;

TEST_CASE("epoch formats and parses", "[time]") {
    REQUIRE(time::to_iso8601(0) == "1970-01-01T00:00:00.000000000Z");

    Timestamp ts = -1;
    REQUIRE(time::from_iso8601("1970-01-01T00:00:00Z", ts));
    REQUIRE(ts == 0);
}

TEST_CASE("a realistic session timestamp round trips", "[time]") {
    // 2024-03-15 13:30:00 UTC, the US equity open on a day in a leap year.
    Timestamp ts = 0;
    REQUIRE(time::from_iso8601("2024-03-15T13:30:00Z", ts));
    REQUIRE(time::to_iso8601(ts) == "2024-03-15T13:30:00.000000000Z");

    Timestamp again = 0;
    REQUIRE(time::from_iso8601(time::to_iso8601(ts), again));
    REQUIRE(again == ts);
}

TEST_CASE("pre-epoch timestamps land on the right day", "[time]") {
    // Floor division, not truncation toward zero: getting this wrong puts
    // every negative timestamp one day late.
    Timestamp ts = 0;
    REQUIRE(time::from_iso8601("1969-12-31T23:59:59Z", ts));
    REQUIRE(ts == -time::kSecond);
    REQUIRE(time::to_iso8601(ts) == "1969-12-31T23:59:59.000000000Z");

    REQUIRE(time::from_iso8601("1969-07-20T20:17:40Z", ts));
    REQUIRE(time::to_iso8601(ts) == "1969-07-20T20:17:40.000000000Z");
}

TEST_CASE("leap days are handled", "[time]") {
    Timestamp leap = 0;
    REQUIRE(time::from_iso8601("2024-02-29T12:00:00Z", leap));
    REQUIRE(time::to_iso8601(leap) == "2024-02-29T12:00:00.000000000Z");

    // 2000 is a leap year; 1900 and 2100 are not.
    Timestamp ts = 0;
    REQUIRE(time::from_iso8601("2000-02-29T00:00:00Z", ts));
    REQUIRE_FALSE(time::from_iso8601("1900-02-29T00:00:00Z", ts));
    REQUIRE_FALSE(time::from_iso8601("2100-02-29T00:00:00Z", ts));
    REQUIRE_FALSE(time::from_iso8601("2023-02-29T00:00:00Z", ts));
}

TEST_CASE("fractional seconds scale to nanoseconds", "[time]") {
    Timestamp ts = 0;

    REQUIRE(time::from_iso8601("1970-01-01T00:00:00.5Z", ts));
    REQUIRE(ts == 500 * time::kMillisecond);

    REQUIRE(time::from_iso8601("1970-01-01T00:00:00.001Z", ts));
    REQUIRE(ts == time::kMillisecond);

    REQUIRE(time::from_iso8601("1970-01-01T00:00:00.000000001Z", ts));
    REQUIRE(ts == 1);

    // Beyond nanosecond resolution the extra digits are truncated, never
    // rounded: rounding could move an event past its neighbour and reorder a
    // replay.
    REQUIRE(time::from_iso8601("1970-01-01T00:00:00.0000000019Z", ts));
    REQUIRE(ts == 1);
}

TEST_CASE("the forms a CSV export actually contains are accepted", "[time]") {
    Timestamp with_t = 0;
    Timestamp with_space = 0;
    REQUIRE(time::from_iso8601("2024-03-15T13:30:00", with_t));
    REQUIRE(time::from_iso8601("2024-03-15 13:30:00", with_space));
    REQUIRE(with_t == with_space);

    // A bare date means midnight UTC.
    Timestamp midnight = 0;
    Timestamp explicit_midnight = 0;
    REQUIRE(time::from_iso8601("2024-03-15", midnight));
    REQUIRE(time::from_iso8601("2024-03-15T00:00:00Z", explicit_midnight));
    REQUIRE(midnight == explicit_midnight);

    // Seconds are optional.
    Timestamp no_seconds = 0;
    REQUIRE(time::from_iso8601("2024-03-15T13:30", no_seconds));
    REQUIRE(no_seconds == with_t);
}

// A timestamp that parses to a plausible wrong value silently reorders a whole
// replay, so anything not fully understood is refused.
TEST_CASE("malformed timestamps are refused rather than guessed at", "[time]") {
    Timestamp ts = 0;

    REQUIRE_FALSE(time::from_iso8601("", ts));
    REQUIRE_FALSE(time::from_iso8601("not a timestamp", ts));
    REQUIRE_FALSE(time::from_iso8601("2024-3-15T13:30:00Z", ts));    // unpadded month
    REQUIRE_FALSE(time::from_iso8601("2024-13-01T00:00:00Z", ts));   // month 13
    REQUIRE_FALSE(time::from_iso8601("2024-00-01T00:00:00Z", ts));   // month 0
    REQUIRE_FALSE(time::from_iso8601("2024-04-31T00:00:00Z", ts));   // April has 30
    REQUIRE_FALSE(time::from_iso8601("2024-03-15T24:00:00Z", ts));   // hour 24
    REQUIRE_FALSE(time::from_iso8601("2024-03-15T13:60:00Z", ts));   // minute 60
    REQUIRE_FALSE(time::from_iso8601("2024-03-15T23:59:60Z", ts));   // leap second
    REQUIRE_FALSE(time::from_iso8601("2024-03-15T13:30:00.Z", ts));  // empty fraction
    REQUIRE_FALSE(time::from_iso8601("2024-03-15T13:30:00Z extra", ts));
}

// Honouring an offset silently would make the same file mean different things
// depending on where it was written, so an offset is a parse failure.
TEST_CASE("numeric UTC offsets are refused", "[time]") {
    Timestamp ts = 0;
    REQUIRE_FALSE(time::from_iso8601("2024-03-15T13:30:00-05:00", ts));
    REQUIRE_FALSE(time::from_iso8601("2024-03-15T13:30:00+00:00", ts));
}

TEST_CASE("time constants are consistent", "[time]") {
    STATIC_REQUIRE(time::kSecond == 1'000 * time::kMillisecond);
    STATIC_REQUIRE(time::kMinute == 60 * time::kSecond);
    STATIC_REQUIRE(time::kHour == 3'600 * time::kSecond);
    STATIC_REQUIRE(time::kDay == 86'400 * time::kSecond);
}

TEST_CASE("side helpers", "[types]") {
    STATIC_REQUIRE(opposite(MS_SIDE_BUY) == MS_SIDE_SELL);
    STATIC_REQUIRE(opposite(MS_SIDE_SELL) == MS_SIDE_BUY);
    STATIC_REQUIRE(side_sign(MS_SIDE_BUY) == 1.0);
    STATIC_REQUIRE(side_sign(MS_SIDE_SELL) == -1.0);
    REQUIRE(to_string(MS_SIDE_BUY) == "BUY");
    REQUIRE(to_string(MS_ORD_LIMIT) == "LIMIT");
    REQUIRE(to_string(MS_TIF_IOC) == "IOC");
}
