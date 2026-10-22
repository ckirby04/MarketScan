#ifndef MARKETSCAN_RESEARCH_RULE_RUN_HPP
#define MARKETSCAN_RESEARCH_RULE_RUN_HPP

/*
 * One run of the drawdown_entry rule over one instrument's daily bars.
 *
 * The research job's unit of work: a sweep is many of these with different
 * parameters, a bootstrap is many of these over different paths. It drives the
 * same Engine, fill model, broker and ledger as run_backtest -- there is no
 * second simulator -- but takes its bars from memory and its strategy from the
 * registry, because a resampled path has no file and a sweep point has no DLL.
 *
 * Execution is DRAWDOWN_ENTRY.md's: decided at a close, filled at the next
 * close. Every fill's lookahead triple is recorded so a caller can assert it.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/core/money.hpp"
#include "marketscan/core/types.hpp"
#include "marketscan/data/bar_store.hpp"
#include "marketscan/research/drawdown_entry.hpp"

namespace marketscan::research {

struct Tranche {
    double level_pct = 0.0;
    double fraction_pct = 100.0;
};

/// The rule's parameters, in the form's terms. DRAWDOWN_ENTRY.md "Inputs".
struct RuleParams {
    ReferenceKind reference = ReferenceKind::AllTimeHigh;
    int window_days = 252;

    /// In form order. One to kMaxTranches.
    std::vector<Tranche> tranches{Tranche{20.0, 100.0}};

    bool recover_to_high = true;
    double take_profit_pct = 0.0;    // 0 is off
    double stop_loss_pct = 0.0;      // 0 is off
    double trailing_stop_pct = 0.0;  // 0 is off
    int time_stop_days = 0;          // 0 is off

    bool reentry = true;

    /// Buy-and-hold, as DRAWDOWN_ENTRY.md "The benchmark" defines it.
    [[nodiscard]] static RuleParams buy_and_hold();
};

struct RuleRunConfig {
    /// One instrument, warmup bars included, ascending.
    BarSeries series;

    /// First decision day, days since 1970-01-01 of the bar's UTC stamp.
    std::int64_t start_day = 0;

    Money starting_cash = Money::from_dollars(10'000);

    /// Full spread of the synthesized quotes, basis points of the close.
    double spread_bps = 2.0;
    double commission_per_order = 0.0;

    /// DRAWDOWN_ENTRY.md step 6.
    double reserve_pct = 2.0;

    RuleParams rule;
};

struct RuleFill {
    /// Index into the series of the bar this fill happened on.
    std::size_t bar = 0;
    Timestamp ts = 0;
    Side side = MS_SIDE_BUY;
    double quantity = 0.0;
    double price = 0.0;
    std::uint64_t tag = 0;
    Money commission;

    /// The lookahead triple's other two legs, copied from the ledger.
    Timestamp decision_ts = 0;
    Timestamp price_ts = 0;
    bool honest = false;
};

struct RuleRun {
    /// One equity point per bar, at that bar's close, after that bar's fills.
    std::vector<Timestamp> times;
    std::vector<double> equity;

    std::vector<RuleFill> fills;

    /// Orders the broker refused -- step 7's gap-ups.
    std::uint64_t rejected = 0;

    bool invariant_held = true;

    /// Set when the run could not be made: bad parameters, an empty series, an
    /// equity curve that did not come out one point per bar.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] RuleRun run_rule(const RuleRunConfig& config);

/// Days since 1970-01-01 of a bar's UTC stamp: the unit of `start_day`.
///
/// Floor division: C++ truncates toward zero, which would put every instant in
/// the day before the epoch into day 0 along with the day after it.
[[nodiscard]] constexpr std::int64_t day_of(Timestamp ts) noexcept {
    constexpr std::int64_t kDay = 86'400LL * 1'000'000'000LL;
    return ts >= 0 ? ts / kDay : (ts - (kDay - 1)) / kDay;
}

}  // namespace marketscan::research

#endif  // MARKETSCAN_RESEARCH_RULE_RUN_HPP
