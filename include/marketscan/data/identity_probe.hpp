#ifndef MARKETSCAN_DATA_IDENTITY_PROBE_HPP
#define MARKETSCAN_DATA_IDENTITY_PROBE_HPP

/*
 * Deciding whether a ticker kept trading across a gap in its index membership.
 *
 * M3 split a ticker into two securities when its membership gap exceeded 730
 * days. That is an inference from a threshold, and roughly a quarter of the
 * forty-one splits it produced are probably wrong in the "keep" direction.
 *
 * They do not have to stay inferences. A company that merely left the index
 * kept trading, so its price series spans the gap. A ticker that was
 * surrendered and reassigned has a hole between one company's last trade and
 * the next company's first. That is measurable.
 *
 * THE TRAP, AND WHY THIS IS NOT A SIMPLE COVERAGE TEST
 *
 * A vendor serves one series per ticker: the *current* issuer's. Ask Stooq for
 * CCI and you get Crown Castle from 1998, not Citicorp from before it. So for a
 * reassigned ticker the earlier company's history is simply absent, and naive
 * coverage-within-the-gap reads a continuous Crown Castle series and concludes
 * "continuous" -- producing a false `keep` on exactly the cases where a wrong
 * keep welds two companies together.
 *
 * The defence is to check the flank as well as the gap. A verdict of
 * `continuous` requires the series to also cover the period when the ticker was
 * a member *before* the gap. If it does not, the series in hand is the later
 * issuer's alone, it cannot speak to what came before, and the answer is
 * unresolved -- not continuous, and not split either.
 *
 * ABSENCE IS NOT EVIDENCE
 *
 * Missing data never produces a verdict. Stooq's coverage of long-dead tickers
 * is thin, and a missing series is a vendor limitation rather than a delisting.
 * Every path that runs out of data returns Unresolved with the reason.
 *
 * Pure, offline, and unit-tested against synthetic series. Fetching and
 * classifying are separate on purpose: a classifier that could reach the
 * network would be untestable and its results unreproducible.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/core/session_calendar.hpp"
#include "marketscan/core/types.hpp"
#include "marketscan/data/bar_store.hpp"

namespace marketscan {

/// What the price series says about a membership gap.
enum class IdentityVerdict : std::uint8_t {
    /// The data cannot decide. The 730-day threshold governs, and the case
    /// stays open. This is the honest answer far more often than either of the
    /// others, and it is never a failure.
    Unresolved = 0,

    /// The ticker traded throughout the gap, on a series that also covers the
    /// era before it. The same company left the index and came back.
    Continuous = 1,

    /// The ticker stopped trading and later resumed, with a hole between. The
    /// string changed hands.
    Discontinuous = 2,
};

[[nodiscard]] constexpr std::string_view to_string(IdentityVerdict v) noexcept {
    switch (v) {
        case IdentityVerdict::Unresolved:
            return "unresolved";
        case IdentityVerdict::Continuous:
            return "continuous";
        case IdentityVerdict::Discontinuous:
            return "discontinuous";
    }
    return "?";
}

/// Where the thresholds are set, and every one of them is a judgment.
///
/// Stated in the generated report next to the 730 days, for the same reason
/// that one is: a number nobody can see is a number nobody can argue with.
struct IdentityProbeConfig {
    /// Coverage at or above this, with no long hole, is continuous trading.
    ///
    /// Ninety percent rather than something higher because coverage is measured
    /// against weekdays and roughly four percent of weekdays are exchange
    /// holidays, so a perfectly covered series reads about 96%.
    double continuous_coverage = 0.90;

    /// Coverage at or below this is a series that was not trading -- but only
    /// together with discontinuous_hole below.
    double discontinuous_coverage = 0.10;

    /// A discontinuous verdict also requires one hole covering at least this
    /// fraction of the gap.
    ///
    /// Both conditions, not either. A series that is 2% covered by a scatter of
    /// stray days across six years is a vendor holding fragments, and calling
    /// that "did not trade" would be treating patchy data as evidence -- the
    /// one thing this classifier must never do. A series that is 2% covered
    /// because it stops dead for five years is something else entirely, and the
    /// hole is what tells them apart.
    double discontinuous_hole = 0.50;

    /// The longest run of consecutive weekdays without a price that a
    /// continuous series may contain.
    ///
    /// Twenty is about a month. Long halts happen -- a month of them does not,
    /// and a gap that long wants a human rather than a verdict.
    int max_hole_days = 20;

    /// Coverage required over the membership period *before* the gap, for a
    /// continuous verdict to mean anything. See the header.
    double flank_coverage = 0.50;

    /// A return across the largest hole beyond this is flagged for review,
    /// whatever the verdict.
    ///
    /// Fifty percent catches reverse splits, spin-off adjustments and genuine
    /// handovers alike -- which is the point. It flags; it never decides. A
    /// large seam on an otherwise continuous series is far more likely to be a
    /// corporate action than a different company.
    double seam_review = 0.50;
};

/// One measurement, with every number the verdict was drawn from.
struct IdentityMeasurement {
    IdentityVerdict verdict = IdentityVerdict::Unresolved;

    /// Why, in a phrase. Always populated, including for a clean verdict.
    std::string reason;

    /// Weekdays in the gap window for which a price exists, over weekdays in
    /// the window. NaN when the window is empty.
    double coverage = 0.0;

    /// The same, over the membership period before the gap.
    double flank_coverage = 0.0;

    /// Longest run of consecutive weekdays with no price, inside the gap.
    int largest_hole_days = 0;

    /// Return across that hole. NaN when there is no hole to cross.
    double seam_return = 0.0;

    /// Bars observed inside the gap window, and weekdays expected in it.
    int bars_in_gap = 0;
    int weekdays_in_gap = 0;

    /// Set when the seam is beyond the review threshold. Independent of the
    /// verdict: a flagged case still has whatever verdict it earned.
    bool flagged = false;

    [[nodiscard]] bool resolved() const noexcept { return verdict != IdentityVerdict::Unresolved; }
};

/// Classifies one membership gap from one price series.
///
/// `bars` must be ascending in time; `gap_start` is the exclusive end of the
/// earlier membership period and `gap_end` the inclusive start of the later
/// one. `flank_start` is the beginning of the earlier membership period, which
/// is what makes the vendor-coverage check possible.
[[nodiscard]] IdentityMeasurement probe_identity(const std::vector<StoredBar>& bars,
                                                 Timestamp flank_start,
                                                 Timestamp gap_start,
                                                 Timestamp gap_end,
                                                 const IdentityProbeConfig& config = {});

/// Weekdays in [from, until). The denominator coverage is measured against.
///
/// Weekdays rather than exchange sessions: this project has no holiday
/// calendar, and inventing one to get four percent more precision in a
/// denominator would be a much larger claim than the one being made.
[[nodiscard]] int weekdays_between(Timestamp from, Timestamp until) noexcept;

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_IDENTITY_PROBE_HPP
