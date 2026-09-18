// The identity classifier: does a ticker's price series span its membership gap?
//
// Entirely offline and entirely synthetic. The classifier is a pure function
// over a price series, and fetching is somebody else's job, precisely so that
// this file can assert what it asserts without depending on a vendor being up.
//
// Each test below builds a series shaped like one of the real cases and checks
// the classifier reaches the verdict that shape deserves. Two of them matter
// more than the rest:
//
//   "absence is never evidence"        -- missing data must never produce a
//                                         split, because a vendor that does not
//                                         carry a dead ticker is a coverage
//                                         limit and not a delisting.
//
//   "the later issuer's history alone" -- a vendor serves one series per
//                                         ticker, the current issuer's. Crown
//                                         Castle's CCI runs cleanly through the
//                                         1998-2012 gap and says nothing about
//                                         Citicorp. Reading that as "continuous"
//                                         would produce a confident, wrong
//                                         `keep` on exactly the cases where a
//                                         wrong keep welds two companies.

#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "marketscan/data/identity_probe.hpp"

using namespace marketscan;

namespace {

[[nodiscard]] Timestamp on(int year, unsigned month, unsigned day) {
    return bar_store::daily_close_stamp(calendar::days_from_civil(year, month, day));
}

/// A bar on every weekday in [from, until), at a drifting price.
///
/// Weekdays rather than exchange sessions, matching the denominator the
/// classifier measures against: a synthetic series that skipped holidays would
/// read 96% covered and make every threshold in these tests four points
/// vaguer than it looks.
void fill(std::vector<StoredBar>& bars, Timestamp from, Timestamp until, double price) {
    for (std::int64_t day = session_day(ExchangeZone::UsEastern, from);
         day < session_day(ExchangeZone::UsEastern, until);
         ++day) {
        const unsigned weekday = calendar::weekday_from_days(day);
        if (weekday == 0 || weekday == 6) {
            continue;
        }
        StoredBar bar;
        bar.ts = bar_store::daily_close_stamp(day);
        bar.open = bar.high = bar.low = bar.close = price;
        bar.volume = 1'000'000.0;
        bars.push_back(bar);
        price *= 1.0002;
    }
}

/// A series covering the membership before the gap and continuing through it.
/// The shape of a company that merely left the index: HP, OKE, TER, JBL, AN, RIG.
[[nodiscard]] std::vector<StoredBar> traded_throughout() {
    std::vector<StoredBar> bars;
    fill(bars, on(1996, 1, 2), on(2012, 1, 3), 40.0);
    return bars;
}

/// A series that stops and later resumes, with the earlier era present.
/// The shape the classifier can call discontinuous on its own evidence.
[[nodiscard]] std::vector<StoredBar> stopped_and_resumed() {
    std::vector<StoredBar> bars;
    fill(bars, on(1996, 1, 2), on(2000, 1, 3), 40.0);
    fill(bars, on(2006, 1, 3), on(2012, 1, 3), 15.0);
    return bars;
}

/// Only the later issuer's history, starting at the gap. The shape a vendor
/// actually returns for a reassigned ticker: CCI, BR, CNC, AMP.
[[nodiscard]] std::vector<StoredBar> later_issuer_only() {
    std::vector<StoredBar> bars;
    // Begins exactly where the gap does, so the *only* thing distinguishing
    // this from a company that traded throughout is the missing flank. A
    // fixture that started a month late would fail the hole test instead and
    // pass for the wrong reason.
    fill(bars, on(2000, 1, 3), on(2012, 1, 3), 12.0);
    return bars;
}

}  // namespace

// --------------------------------------------------------------------------
// The denominator
// --------------------------------------------------------------------------

TEST_CASE("weekdays are counted, not calendar days", "[identity][probe]") {
    // 2024-01-01 is a Monday. One full week is five.
    REQUIRE(weekdays_between(on(2024, 1, 1), on(2024, 1, 8)) == 5);
    REQUIRE(weekdays_between(on(2024, 1, 1), on(2024, 1, 15)) == 10);

    // A weekend alone contains none.
    REQUIRE(weekdays_between(on(2024, 1, 6), on(2024, 1, 8)) == 0);

    // Inverted and empty windows are zero rather than negative.
    REQUIRE(weekdays_between(on(2024, 1, 8), on(2024, 1, 1)) == 0);
    REQUIRE(weekdays_between(on(2024, 1, 1), on(2024, 1, 1)) == 0);

    // A year is about 261 weekdays; the ~9 exchange holidays are why the
    // continuous threshold is 90% and not 99%.
    const int year = weekdays_between(on(2023, 1, 1), on(2024, 1, 1));
    REQUIRE(year >= 258);
    REQUIRE(year <= 262);
}

// --------------------------------------------------------------------------
// Absence is never evidence
// --------------------------------------------------------------------------

TEST_CASE("no data is unresolved, never a split", "[identity][probe]") {
    // The most important branch in the classifier. Stooq's coverage of long
    // dead tickers is thin, and a vendor that does not carry one is a coverage
    // limit rather than a delisting. If this ever returns Discontinuous, the
    // classifier has started manufacturing splits out of nothing.
    const IdentityMeasurement result =
        probe_identity({}, on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    REQUIRE(result.verdict == IdentityVerdict::Unresolved);
    REQUIRE_FALSE(result.resolved());
    REQUIRE(result.reason.find("no price data") != std::string::npos);
}

TEST_CASE("scattered fragments are unresolved, not a split", "[identity][probe]") {
    // Two percent coverage, spread as a week a year across six years. That is a
    // vendor holding pieces, not a company that stopped trading -- and the
    // difference is the shape, not the amount. Calling this discontinuous would
    // be treating patchy data as evidence of a handover.
    std::vector<StoredBar> bars;
    fill(bars, on(1996, 1, 2), on(2000, 1, 3), 40.0);
    for (int year = 2000; year < 2006; ++year) {
        fill(bars, on(year, 6, 1), on(year, 6, 8), 40.0);
    }

    const IdentityMeasurement result =
        probe_identity(bars, on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    INFO("coverage " << result.coverage);
    REQUIRE(result.verdict == IdentityVerdict::Unresolved);
    REQUIRE(result.coverage > 0.0);
    REQUIRE(result.coverage < 0.10);
}

// --------------------------------------------------------------------------
// The verdicts
// --------------------------------------------------------------------------

TEST_CASE("a series that spans the gap and the era before it is continuous", "[identity][probe]") {
    const IdentityMeasurement result =
        probe_identity(traded_throughout(), on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    REQUIRE(result.verdict == IdentityVerdict::Continuous);
    REQUIRE(result.coverage > 0.99);
    REQUIRE(result.flank_coverage > 0.99);
    REQUIRE(result.largest_hole_days == 0);
    REQUIRE_FALSE(result.flagged);
}

TEST_CASE("a series with a hole through the gap is discontinuous", "[identity][probe]") {
    const IdentityMeasurement result =
        probe_identity(stopped_and_resumed(), on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    REQUIRE(result.verdict == IdentityVerdict::Discontinuous);
    REQUIRE(result.coverage < 0.01);
    REQUIRE(result.largest_hole_days > 1'000);
}

// The trap. This is the test that stops the classifier producing a confident,
// wrong `keep` on the cases where a wrong keep is most expensive.
TEST_CASE("the later issuer's history alone cannot resolve the gap", "[identity][probe]") {
    const IdentityMeasurement result =
        probe_identity(later_issuer_only(), on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    // The series runs cleanly through the whole gap...
    REQUIRE(result.coverage > 0.95);

    // ...and says nothing whatsoever about the ticker before it, so the
    // classifier declines rather than concluding.
    REQUIRE(result.flank_coverage < 0.05);
    REQUIRE(result.verdict == IdentityVerdict::Unresolved);
    REQUIRE(result.reason.find("later issuer") != std::string::npos);
}

TEST_CASE("a long halt inside an otherwise covered gap is unresolved", "[identity][probe]") {
    // Covered enough to look continuous, with a two-month hole in the middle.
    // That is not a verdict; it is a question for a person.
    std::vector<StoredBar> bars;
    fill(bars, on(1996, 1, 2), on(2002, 6, 1), 40.0);
    fill(bars, on(2002, 9, 1), on(2006, 1, 3), 40.0);

    const IdentityMeasurement result =
        probe_identity(bars, on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    INFO("coverage " << result.coverage << " hole " << result.largest_hole_days);
    REQUIRE(result.coverage > 0.90);
    REQUIRE(result.largest_hole_days > 20);
    REQUIRE(result.verdict == IdentityVerdict::Unresolved);
}

// --------------------------------------------------------------------------
// The seam flags; it never decides
// --------------------------------------------------------------------------

TEST_CASE("a large seam flags a case without changing its verdict", "[identity][probe]") {
    // A ten-day hole with the price a third of what it was afterwards -- the
    // shape of a reverse split, a spin-off adjustment, or unadjusted vendor
    // data. None of those is a different company, so the verdict must not move.
    std::vector<StoredBar> bars;
    fill(bars, on(1996, 1, 2), on(2002, 6, 3), 90.0);
    fill(bars, on(2002, 6, 20), on(2006, 1, 3), 30.0);

    const IdentityMeasurement result =
        probe_identity(bars, on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    REQUIRE(result.flagged);
    REQUIRE(result.seam_return < -0.5);

    // Flagged, and still continuous. A classifier that split on seam size
    // would split every reverse split in the index.
    REQUIRE(result.verdict == IdentityVerdict::Continuous);
}

TEST_CASE("an unbroken series has no seam to measure", "[identity][probe]") {
    const IdentityMeasurement result =
        probe_identity(traded_throughout(), on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3));

    // NaN rather than zero, for the reason every metric in this project returns
    // NaN: "no seam" and "a seam of zero" are different claims.
    REQUIRE(std::isnan(result.seam_return));
    REQUIRE_FALSE(result.flagged);
}

// --------------------------------------------------------------------------
// Thresholds
// --------------------------------------------------------------------------

TEST_CASE("the thresholds are the thresholds", "[identity][probe]") {
    // Coverage just under the bar does not resolve; just over it does. Asserted
    // because these numbers are judgment calls that end up in a report, and a
    // classifier whose behaviour did not actually track them would make that
    // report a decoration.
    std::vector<StoredBar> bars;
    fill(bars, on(1996, 1, 2), on(2000, 1, 3), 40.0);
    fill(bars, on(2000, 1, 3), on(2005, 6, 1), 40.0);  // ~88% of a 2000-2006 gap

    IdentityProbeConfig strict;
    strict.continuous_coverage = 0.95;
    const IdentityMeasurement refused =
        probe_identity(bars, on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3), strict);
    REQUIRE(refused.verdict == IdentityVerdict::Unresolved);

    IdentityProbeConfig lenient;
    lenient.continuous_coverage = 0.85;
    lenient.max_hole_days = 400;
    const IdentityMeasurement accepted =
        probe_identity(bars, on(1996, 1, 2), on(2000, 1, 3), on(2006, 1, 3), lenient);
    REQUIRE(accepted.verdict == IdentityVerdict::Continuous);
}

TEST_CASE("a gap with no flank window does not fail the flank test", "[identity][probe]") {
    // A ticker whose first membership period began the day the gap did has no
    // flank to cover. That is not evidence against continuity, so it must not
    // be treated as a failure -- the NaN path through the comparison.
    const IdentityMeasurement result =
        probe_identity(traded_throughout(), on(2000, 1, 3), on(2000, 1, 3), on(2006, 1, 3));

    REQUIRE(std::isnan(result.flank_coverage));
    REQUIRE(result.verdict == IdentityVerdict::Continuous);
}
