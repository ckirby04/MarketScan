/*
 * The trading calendar. No network: every response below is the shape the
 * broker returns, with dates chosen because they are the ones that break a
 * hard-coded 09:30-to-16:00 assumption.
 */

#include <catch2/catch_test_macros.hpp>

#include "marketscan/live/calendar.hpp"

using namespace marketscan;
using namespace marketscan::live;

namespace {

[[nodiscard]] Timestamp at(const char* iso) {
    Timestamp ts = 0;
    REQUIRE(time::from_iso8601(iso, ts));
    return ts;
}

}  // namespace

TEST_CASE("eastern offset follows the post-2007 daylight rule", "[live][calendar]") {
    int offset = 0;

    // Second Sunday in March 2026 is the 8th; first Sunday in November is the
    // 1st. A day either side of each boundary, and one in deep winter.
    REQUIRE(eastern_utc_offset("2026-01-15", offset));
    REQUIRE(offset == -5 * 3600);

    REQUIRE(eastern_utc_offset("2026-03-06", offset));
    REQUIRE(offset == -5 * 3600);

    REQUIRE(eastern_utc_offset("2026-03-09", offset));
    REQUIRE(offset == -4 * 3600);

    REQUIRE(eastern_utc_offset("2026-07-04", offset));
    REQUIRE(offset == -4 * 3600);

    REQUIRE(eastern_utc_offset("2026-10-30", offset));
    REQUIRE(offset == -4 * 3600);

    REQUIRE(eastern_utc_offset("2026-11-02", offset));
    REQUIRE(offset == -5 * 3600);
}

TEST_CASE("eastern offset refuses a date it cannot read", "[live][calendar]") {
    int offset = 99;
    REQUIRE_FALSE(eastern_utc_offset("not-a-date", offset));
    REQUIRE_FALSE(eastern_utc_offset("2026-13-01", offset));
    REQUIRE(offset == 99);
}

TEST_CASE("a regular session converts to the right UTC instants", "[live][calendar]") {
    MarketCalendar calendar;
    REQUIRE(calendar.parse(
        R"([{"date":"2026-09-21","open":"09:30","close":"16:00","session_open":"0700",)"
        R"("session_close":"1900"}])"));

    REQUIRE(calendar.sessions().size() == 1);
    const TradingSession& session = calendar.sessions().front();
    REQUIRE(session.date == "2026-09-21");
    // September is daylight time: 09:30 in New York is 13:30 UTC.
    REQUIRE(session.open == at("2026-09-21T13:30:00Z"));
    REQUIRE(session.close == at("2026-09-21T20:00:00Z"));
    REQUIRE_FALSE(session.half_day);
}

TEST_CASE("a winter session shifts by the extra hour", "[live][calendar]") {
    MarketCalendar calendar;
    REQUIRE(calendar.parse(R"([{"date":"2026-01-15","open":"09:30","close":"16:00"}])"));
    REQUIRE(calendar.sessions().front().open == at("2026-01-15T14:30:00Z"));
    REQUIRE(calendar.sessions().front().close == at("2026-01-15T21:00:00Z"));
}

TEST_CASE("a half day is recognised as one", "[live][calendar]") {
    // The Friday after Thanksgiving 2026. A runner that assumed a 16:00 close
    // would submit into a market that shut three hours earlier.
    MarketCalendar calendar;
    REQUIRE(calendar.parse(R"([{"date":"2026-11-27","open":"09:30","close":"13:00"}])"));
    const TradingSession& session = calendar.sessions().front();
    REQUIRE(session.half_day);
    REQUIRE(session.close == at("2026-11-27T18:00:00Z"));
}

TEST_CASE("open and closed are answered from the fetched sessions", "[live][calendar]") {
    MarketCalendar calendar;
    REQUIRE(calendar.parse(R"([{"date":"2026-09-21","open":"09:30","close":"16:00"},)"
                           R"({"date":"2026-09-22","open":"09:30","close":"16:00"}])"));

    REQUIRE(calendar.is_open(at("2026-09-21T14:00:00Z")));
    REQUIRE_FALSE(calendar.is_open(at("2026-09-21T12:00:00Z")));
    // The close is exclusive: an event stamped exactly at the bell belongs to
    // the auction, not to the continuous session.
    REQUIRE_FALSE(calendar.is_open(at("2026-09-21T20:00:00Z")));
    REQUIRE(calendar.is_open(at("2026-09-21T19:59:59Z")));

    // Overnight.
    REQUIRE_FALSE(calendar.is_open(at("2026-09-22T02:00:00Z")));

    const TradingSession* next = calendar.next_session(at("2026-09-21T21:00:00Z"));
    REQUIRE(next != nullptr);
    REQUIRE(next->date == "2026-09-22");
}

TEST_CASE("a closed market and an uncovered date are different answers", "[live][calendar]") {
    // The distinction that stops a runner treating "I have no calendar for
    // December" as "December is a holiday".
    MarketCalendar calendar;
    REQUIRE(calendar.parse(R"([{"date":"2026-09-21","open":"09:30","close":"16:00"}])"));

    REQUIRE(calendar.covers(at("2026-09-21T12:00:00Z")));
    REQUIRE_FALSE(calendar.is_open(at("2026-09-21T12:00:00Z")));

    REQUIRE_FALSE(calendar.covers(at("2026-12-25T15:00:00Z")));
    REQUIRE_FALSE(calendar.is_open(at("2026-12-25T15:00:00Z")));
}

TEST_CASE("a malformed calendar is an error, not an empty schedule", "[live][calendar]") {
    // An empty schedule means "the market never opens", which a runner would
    // obey. That must not be what a parse failure looks like.
    MarketCalendar calendar;
    REQUIRE_FALSE(calendar.parse("not json"));
    REQUIRE_FALSE(calendar.error().empty());

    REQUIRE_FALSE(calendar.parse(R"([{"date":"2026-09-21","open":"09:30"}])"));
    REQUIRE(calendar.sessions().empty());
    REQUIRE_FALSE(calendar.error().empty());

    REQUIRE_FALSE(calendar.parse(R"([{"date":"2026-09-21","open":"16:00","close":"09:30"}])"));
    REQUIRE_FALSE(calendar.error().empty());
}

TEST_CASE("an empty calendar is not open and covers nothing", "[live][calendar]") {
    MarketCalendar calendar;
    REQUIRE(calendar.parse("[]"));
    REQUIRE(calendar.empty());
    REQUIRE_FALSE(calendar.is_open(at("2026-09-21T14:00:00Z")));
    REQUIRE_FALSE(calendar.covers(at("2026-09-21T14:00:00Z")));
}
