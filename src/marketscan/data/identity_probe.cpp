#include "marketscan/data/identity_probe.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace marketscan {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Local calendar day for a bar, in the exchange's own zone.
[[nodiscard]] std::int64_t day_of(Timestamp ts) noexcept {
    return session_day(ExchangeZone::UsEastern, ts);
}

[[nodiscard]] bool is_weekday(std::int64_t day) noexcept {
    const unsigned weekday = calendar::weekday_from_days(day);
    return weekday != 0 && weekday != 6;  // not Sunday, not Saturday
}

}  // namespace

int weekdays_between(Timestamp from, Timestamp until) noexcept {
    if (until <= from) {
        return 0;
    }
    const std::int64_t first = day_of(from);
    const std::int64_t last = day_of(until);

    // Counted day by day rather than by arithmetic on weeks. The window here is
    // at most a few decades, and a closed form would have to be right about
    // both endpoints' weekdays to save a loop nobody is waiting on.
    int count = 0;
    for (std::int64_t day = first; day < last; ++day) {
        count += is_weekday(day) ? 1 : 0;
    }
    return count;
}

IdentityMeasurement probe_identity(const std::vector<StoredBar>& bars,
                                   Timestamp flank_start,
                                   Timestamp gap_start,
                                   Timestamp gap_end,
                                   const IdentityProbeConfig& config) {
    IdentityMeasurement out;
    out.coverage = kNaN;
    out.flank_coverage = kNaN;
    out.seam_return = kNaN;

    if (gap_end <= gap_start) {
        out.reason = "the gap ends before it begins";
        return out;
    }
    if (bars.empty()) {
        // The single most important branch in this file. Stooq's coverage of
        // long-dead tickers is thin, and a missing series is a vendor
        // limitation, not a delisting. Absence is never evidence of a split.
        out.reason = "no price data for this ticker";
        return out;
    }

    out.weekdays_in_gap = weekdays_between(gap_start, gap_end);
    if (out.weekdays_in_gap == 0) {
        out.reason = "the gap contains no weekdays";
        return out;
    }

    // --- Coverage inside the gap ----------------------------------------
    std::vector<std::int64_t> days_in_gap;
    days_in_gap.reserve(bars.size());
    for (const StoredBar& bar : bars) {
        if (bar.ts >= gap_start && bar.ts < gap_end) {
            days_in_gap.push_back(day_of(bar.ts));
        }
    }
    std::sort(days_in_gap.begin(), days_in_gap.end());
    days_in_gap.erase(std::unique(days_in_gap.begin(), days_in_gap.end()), days_in_gap.end());

    out.bars_in_gap = static_cast<int>(days_in_gap.size());
    out.coverage = static_cast<double>(out.bars_in_gap) / static_cast<double>(out.weekdays_in_gap);

    // --- The largest hole, in weekdays ----------------------------------
    //
    // Measured between consecutive observed days rather than by walking the
    // whole window, so a series covering twenty years costs the length of its
    // own data rather than the length of the calendar.
    const std::int64_t first_expected = day_of(gap_start);
    const std::int64_t last_expected = day_of(gap_end);

    std::int64_t hole_from = first_expected;
    std::int64_t hole_to = first_expected;
    int largest = 0;

    const auto consider = [&](std::int64_t from, std::int64_t until) {
        int missing = 0;
        for (std::int64_t day = from; day < until; ++day) {
            missing += is_weekday(day) ? 1 : 0;
        }
        if (missing > largest) {
            largest = missing;
            hole_from = from;
            hole_to = until;
        }
    };

    std::int64_t previous = first_expected;
    for (const std::int64_t day : days_in_gap) {
        consider(previous, day);
        previous = day + 1;
    }
    consider(previous, last_expected);

    out.largest_hole_days = largest;

    // --- The seam -------------------------------------------------------
    //
    // The return across the largest hole: the last price before it against the
    // first price after. It flags; it never decides. A large seam is at least
    // as likely to be a reverse split or a spin-off adjustment as a different
    // company, and this has no way to tell those apart.
    if (largest > 0) {
        const StoredBar* before = nullptr;
        const StoredBar* after = nullptr;
        for (const StoredBar& bar : bars) {
            const std::int64_t day = day_of(bar.ts);
            if (day < hole_from) {
                before = &bar;
            } else if (day >= hole_to && after == nullptr) {
                after = &bar;
            }
        }
        if (before != nullptr && after != nullptr && before->close > 0.0) {
            out.seam_return = after->close / before->close - 1.0;
            out.flagged = std::fabs(out.seam_return) > config.seam_review;
        }
    }

    // --- The flank ------------------------------------------------------
    //
    // Whether the series in hand covers the period when this ticker was a
    // member *before* the gap. Without that it is the later issuer's history
    // alone, and its continuity through the gap says nothing about what the
    // string meant beforehand -- which is exactly the case that would otherwise
    // produce a confident, wrong `keep`.
    const int flank_weekdays = weekdays_between(flank_start, gap_start);
    if (flank_weekdays > 0) {
        std::vector<std::int64_t> flank_days;
        for (const StoredBar& bar : bars) {
            if (bar.ts >= flank_start && bar.ts < gap_start) {
                flank_days.push_back(day_of(bar.ts));
            }
        }
        std::sort(flank_days.begin(), flank_days.end());
        flank_days.erase(std::unique(flank_days.begin(), flank_days.end()), flank_days.end());
        out.flank_coverage =
            static_cast<double>(flank_days.size()) / static_cast<double>(flank_weekdays);
    }

    // --- The verdict ----------------------------------------------------
    const bool covers_gap =
        out.coverage >= config.continuous_coverage && out.largest_hole_days <= config.max_hole_days;
    const bool empty_gap = out.coverage <= config.discontinuous_coverage &&
                           static_cast<double>(out.largest_hole_days) >=
                               config.discontinuous_hole * static_cast<double>(out.weekdays_in_gap);
    const bool covers_flank = !(
        out.flank_coverage < config.flank_coverage);  // NaN-safe: no flank window is not a failure

    if (covers_gap && covers_flank) {
        out.verdict = IdentityVerdict::Continuous;
        out.reason = "traded through the gap on a series that also covers the period before it";
        return out;
    }
    if (covers_gap && !covers_flank) {
        // The trap this classifier exists to avoid. Crown Castle's CCI series
        // runs cleanly through the 1998-2012 gap and says nothing whatsoever
        // about Citicorp, whose CCI it is not.
        out.verdict = IdentityVerdict::Unresolved;
        out.reason = "traded through the gap, but the series does not reach back over the earlier "
                     "membership period -- it is probably the later issuer's history alone";
        return out;
    }
    if (empty_gap) {
        out.verdict = IdentityVerdict::Discontinuous;
        out.reason = "did not trade through the gap";
        return out;
    }

    out.verdict = IdentityVerdict::Unresolved;
    out.reason = "coverage falls between the thresholds";
    if (out.coverage <= config.discontinuous_coverage) {
        out.reason = "barely covered, but in scattered fragments rather than one stretch -- a "
                     "vendor holding pieces, not a company that stopped trading";
    }
    if (out.coverage >= config.continuous_coverage) {
        out.reason = "well covered, but with a hole of " + std::to_string(out.largest_hole_days) +
                     " weekdays in it";
    }
    return out;
}

}  // namespace marketscan
