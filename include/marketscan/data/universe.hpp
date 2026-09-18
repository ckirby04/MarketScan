#ifndef MARKETSCAN_DATA_UNIVERSE_HPP
#define MARKETSCAN_DATA_UNIVERSE_HPP

/*
 * Which securities were in the index on a given day.
 *
 * This is the load-bearing abstraction of the whole project, and the reason is
 * survivorship bias. A backtest run over today's index members, applied to
 * history, is a backtest over the companies that survived -- and it will report
 * a premium that nobody could have earned, because nobody knew in 1999 which
 * names would still be there in 2024.
 *
 * The defence is not a rule a strategy author is asked to remember. It is
 * structural: when a universe is active, a strategy is never shown a bar for a
 * security that was not a member that day, and the host context gives it no API
 * by which to ask about one. Enforced in dispatch. See universe_filter.hpp.
 */

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "marketscan/data/identity_probe.hpp"
#include "marketscan/data/security_master.hpp"

namespace marketscan {

/// One stretch of membership for one security.
struct MembershipPeriod {
    SecurityId security = kInvalidSecurity;

    /// Inclusive.
    Timestamp from = kNever;

    /// Exclusive. kForever for a name still in the index.
    Timestamp until = kForever;

    [[nodiscard]] bool covers(Timestamp ts) const noexcept { return ts >= from && ts < until; }
};

/// An index's membership over time.
///
/// Built once and then read-only, because a universe that could change while a
/// replay was running would make the run depend on when the question was asked.
class Universe {
public:
    Universe() = default;

    /// Adds a period. Overlapping periods for one security are merged.
    void add(SecurityId security, Timestamp from, Timestamp until);

    /// Finalises: sorts, merges and builds the lookup index. Calling add()
    /// afterwards is an error.
    void build();

    /// Members on `ts`, ascending by id.
    ///
    /// Ascending, and a vector rather than a set, because this reaches ranking
    /// and therefore order-emitting code. Anything iterating a hash container
    /// here would make the decile a strategy trades depend on a hash seed.
    [[nodiscard]] const std::vector<SecurityId>& members_on(Timestamp ts) const;

    [[nodiscard]] bool is_member(SecurityId security, Timestamp ts) const noexcept;

    /// The instant `security` stops being a member, at or after `ts`. kForever
    /// if it never does. What the forced-exit path needs.
    [[nodiscard]] Timestamp membership_ends(SecurityId security, Timestamp ts) const noexcept;

    [[nodiscard]] const std::vector<MembershipPeriod>& periods() const noexcept { return periods_; }

    [[nodiscard]] bool empty() const noexcept { return periods_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return periods_.size(); }

    /// Distinct securities that were ever members.
    [[nodiscard]] std::size_t security_count() const noexcept;

    /// Earliest and latest dates the history covers.
    [[nodiscard]] Timestamp first() const noexcept;
    [[nodiscard]] Timestamp last() const noexcept;

private:
    std::vector<MembershipPeriod> periods_;
    bool built_ = false;

    /// Cached answer for the most recent query. A replay asks about one day at
    /// a time and asks many times per day, so this turns a scan per event into
    /// a scan per day.
    mutable Timestamp cached_ts_ = kNever;
    mutable std::vector<SecurityId> cached_members_;
    mutable bool cache_valid_ = false;
};

// ---------------------------------------------------------------------------
// Loading a membership history
// ---------------------------------------------------------------------------

/// How aggressively to treat a gap in a ticker's membership as a new company.
struct IdentityRules {
    /// A gap at least this long splits the ticker into two securities.
    ///
    /// Two years by default. This is a judgment call, not a derived constant,
    /// and it is exactly why every decision it makes is written to a report:
    /// the threshold is arguable and the audit trail is what makes it
    /// reviewable rather than magic.
    ///
    /// Over-splitting is the safe direction. It costs history -- a strategy
    /// cannot compute twelve-month momentum across a break -- which is a
    /// visible, bounded loss. Under-splitting fabricates a price series, which
    /// is invisible and poisons everything downstream.
    Duration split_gap = 2 * 365 * 24 * 60 * 60 * 1'000'000'000LL;

    /// Gaps within this much of the threshold are reported as near misses, so
    /// a decision that nearly went the other way is visible.
    Duration near_miss_window = 180 * 24 * 60 * 60 * 1'000'000'000LL;

    /// Forced and suppressed splits, keyed by ticker. Loaded from
    /// data/security_overrides.csv and consulted before the threshold.
    struct Override {
        std::string ticker;
        /// "split" or "keep".
        std::string action;
        /// The gap this applies to, identified by the date membership resumed.
        Timestamp resumed_on = kNever;
        std::string note;
    };

    std::vector<Override> overrides;

    [[nodiscard]] const Override* find_override(std::string_view ticker,
                                                Timestamp resumed_on) const noexcept;
};

/// One decision the split heuristic made, for the audit report.
struct IdentityDecision {
    std::string ticker;

    /// Start of the membership period *before* the gap. What the identity
    /// probe needs to tell "this series covers both eras" from "this series is
    /// the later issuer's alone".
    Timestamp flank_start = kNever;

    Timestamp gap_start = kNever;
    Timestamp gap_end = kNever;
    Duration gap = 0;
    bool split = false;
    /// "override", "threshold", "below threshold", "near miss".
    std::string rule;
    std::string note;
    SecurityId result = kInvalidSecurity;

    /// What the price series said, when there was one. Verdict Unresolved with
    /// an empty reason means no measurement was attempted at all.
    IdentityMeasurement measured;

    /// Whether the measurement contradicts the threshold.
    [[nodiscard]] bool disagrees() const noexcept {
        if (!measured.resolved()) {
            return false;
        }
        const bool measured_split = measured.verdict == IdentityVerdict::Discontinuous;
        return measured_split != split;
    }
};

/// A membership history, as loaded.
struct MembershipHistory {
    Universe universe;
    std::vector<IdentityDecision> decisions;

    /// Rows the source had that could not be used, with the reason.
    std::vector<std::string> rejected;
};

/// Source of a membership history.
///
/// An interface, and this is the seam that matters: when CRSP arrives, PERMNO
/// becomes SecurityId directly, a CRSP loader implements this, and the entire
/// gap heuristic below is deleted rather than adapted.
class IMembershipSource {
public:
    IMembershipSource() = default;
    IMembershipSource(const IMembershipSource&) = delete;
    IMembershipSource& operator=(const IMembershipSource&) = delete;
    IMembershipSource(IMembershipSource&&) = delete;
    IMembershipSource& operator=(IMembershipSource&&) = delete;
    virtual ~IMembershipSource() = default;

    /// Every (ticker, from, until) the source knows, in any order.
    struct Row {
        std::string ticker;
        Timestamp from = kNever;
        Timestamp until = kForever;
    };

    [[nodiscard]] virtual std::vector<Row> rows() const = 0;

    [[nodiscard]] virtual std::string_view describe() const noexcept = 0;
};

/// Reads the fja05680/sp500 membership history.
///
/// That project reconstructs S&P 500 constituents back to 1996 as a file of
/// (date, comma-separated tickers) snapshots. Snapshots, not intervals: the
/// intervals have to be derived by diffing consecutive days, which is where
/// every gap this module reasons about comes from.
///
/// It is ticker-keyed and cannot express identity, which is the entire reason
/// the heuristic exists.
class SnapshotMembershipSource final : public IMembershipSource {
public:
    /// `path` is a CSV whose first column is a date and whose remaining
    /// content is the member tickers for that date.
    explicit SnapshotMembershipSource(const std::filesystem::path& path);

    [[nodiscard]] std::vector<Row> rows() const override { return rows_; }

    [[nodiscard]] std::string_view describe() const noexcept override { return name_; }

    /// Distinct snapshot dates read.
    [[nodiscard]] std::size_t snapshots() const noexcept { return snapshots_; }

private:
    std::string name_;
    std::vector<Row> rows_;
    std::size_t snapshots_ = 0;
};

/// Turns membership rows into securities and a universe, applying the split
/// heuristic and recording every decision it makes.
[[nodiscard]] MembershipHistory build_membership(const IMembershipSource& source,
                                                 SecurityMaster& master,
                                                 const IdentityRules& rules = {});

/// Reads data/security_overrides.csv. A missing file is not an error: the
/// overrides are corrections, and having none is the normal state.
[[nodiscard]] IdentityRules load_overrides(const std::filesystem::path& path,
                                           IdentityRules rules = {});

/// Measures every gap against a directory of cached daily bars.
///
/// A ticker with no cached series is left Unresolved with the reason, never
/// Discontinuous: a vendor that does not carry a dead ticker is a coverage
/// limit, not a delisting.
void measure_identities(MembershipHistory& history,
                        const std::filesystem::path& price_cache,
                        const IdentityProbeConfig& config = {});

/// Writes docs/IDENTITY_DECISIONS.md.
void write_decision_report(const std::filesystem::path& path,
                           const MembershipHistory& history,
                           const SecurityMaster& master,
                           const IdentityRules& rules,
                           std::string_view source_description,
                           const IdentityProbeConfig& probe_config = {},
                           bool measured = false);

/// Writes data/security_overrides.csv from measurements only.
///
/// Every row carries the numbers it was drawn from and nothing else. "coverage
/// 97.4%, no hole > 3d" is a statement about what was observed;
/// "AutoNation traded continuously" is a claim about the world, and this file
/// is not the place for one.
///
/// Returns how many rows were written. An unresolved case produces none, which
/// leaves it governed by the threshold and open.
std::size_t write_measured_overrides(const std::filesystem::path& path,
                                     const MembershipHistory& history,
                                     const IdentityProbeConfig& config,
                                     std::string_view source_description);

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_UNIVERSE_HPP
