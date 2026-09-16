#ifndef MARKETSCAN_METRICS_PERFORMANCE_HPP
#define MARKETSCAN_METRICS_PERFORMANCE_HPP

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "marketscan/exec/ledger.hpp"
#include "marketscan/metrics/correlation.hpp"

/// Performance statistics.
///
/// **Undefined results are NaN, never zero.** A Sharpe ratio of zero means "no
/// risk-adjusted edge", which is a real and specific claim. A series too short
/// to have a Sharpe ratio is not making that claim, and conflating the two
/// puts a number on a leaderboard that nobody can distinguish from a result.
/// Every function here returns NaN when it cannot answer, NaN propagates
/// through anything built on it, and the GUI renders non-finite values as a
/// dash. Call is_defined() before doing anything else with a result.
namespace marketscan::metrics {

inline constexpr double kUndefined = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool is_defined(double value) noexcept;

// ---------------------------------------------------------------------------
// Annualisation
// ---------------------------------------------------------------------------

struct Annualisation {
    /// Return observations per year. 252 for daily, 252*390 for one-minute
    /// bars of a US equity session.
    double periods_per_year = 252.0;

    /// Annual risk-free rate, as a fraction. Subtracted from returns before
    /// the Sharpe ratio, and used as the target for the Sortino ratio.
    ///
    /// Zero by default. A non-zero rate is a real input that ought to come
    /// from somewhere, and inventing one here would quietly shift every
    /// Sharpe ratio in the application.
    double risk_free_rate = 0.0;

    [[nodiscard]] double per_period_risk_free() const noexcept;
};

/// Observations per year implied by a sampling interval.
///
/// Assumes a 6.5-hour session and 252 trading days, so a one-minute sample
/// gives 98,280 periods a year rather than the 525,600 a naive
/// year-divided-by-a-minute would give. Annualising intraday returns as though
/// markets trade overnight inflates volatility by roughly 2.3x, and every
/// Sharpe ratio built on it by the same factor in the other direction.
[[nodiscard]] double infer_periods_per_year(Duration sample_interval,
                                            double trading_days_per_year = 252.0,
                                            double hours_per_session = 6.5) noexcept;

// ---------------------------------------------------------------------------
// Series preparation
// ---------------------------------------------------------------------------

/// Period-over-period simple returns. One shorter than the input.
///
/// A period where the previous equity was zero or negative yields NaN rather
/// than a division result: a return is not meaningful once an account is wiped
/// out, and the alternative is an infinity that poisons every later statistic.
[[nodiscard]] std::vector<double> simple_returns(std::span<const EquityPoint> equity);

[[nodiscard]] std::vector<double> simple_returns(std::span<const double> levels);

/// Log returns. Used for compounding arithmetic, not for reporting.
[[nodiscard]] std::vector<double> log_returns(std::span<const double> levels);

/// Equity levels from a curve, for the functions that take a bare series.
[[nodiscard]] std::vector<double> equity_levels(std::span<const EquityPoint> equity);

// ---------------------------------------------------------------------------
// Moments
// ---------------------------------------------------------------------------

/// NaN for an empty series. Ignores non-finite observations.
[[nodiscard]] double mean(std::span<const double> values) noexcept;

/// Sample variance, dividing by n-1. NaN for fewer than two observations.
///
/// Sample rather than population because a return series is a sample of a
/// process, not the whole of it. Two-pass, so that a long series of small
/// numbers around a large mean does not lose the variance to cancellation --
/// which the textbook single-pass sum-of-squares form does, silently, and
/// sometimes to a negative result.
[[nodiscard]] double variance(std::span<const double> values) noexcept;

[[nodiscard]] double stddev(std::span<const double> values) noexcept;

/// Standard deviation of only the observations below `target`, divided by the
/// *total* count rather than the count of downside observations.
///
/// Dividing by the total is the convention Sortino's own papers use. The other
/// convention exists and gives systematically larger ratios; mixing the two
/// makes two strategies incomparable, so this one is stated rather than
/// assumed.
[[nodiscard]] double downside_deviation(std::span<const double> values,
                                        double target = 0.0) noexcept;

// ---------------------------------------------------------------------------
// Headline statistics
// ---------------------------------------------------------------------------

/// (last / first) - 1.
[[nodiscard]] double total_return(std::span<const double> levels) noexcept;

/// Compound annual growth rate. NaN for a non-positive span or a wiped-out
/// account.
[[nodiscard]] double cagr(double first_level, double last_level, double years) noexcept;

[[nodiscard]] double cagr(std::span<const EquityPoint> equity) noexcept;

/// Standard deviation of returns, scaled by sqrt(periods per year).
[[nodiscard]] double annualised_volatility(std::span<const double> returns,
                                           const Annualisation& annualisation) noexcept;

/// Mean excess return over its standard deviation, annualised.
///
/// NaN when volatility is zero: a constant series has no risk-adjusted return,
/// and reporting infinity for a strategy that never moved would put it at the
/// top of the leaderboard.
[[nodiscard]] double sharpe(std::span<const double> returns,
                            const Annualisation& annualisation) noexcept;

/// As Sharpe, but penalising only downside deviation.
[[nodiscard]] double sortino(std::span<const double> returns,
                             const Annualisation& annualisation) noexcept;

struct DrawdownResult {
    /// Worst peak-to-trough decline as a positive fraction: 0.25 is a 25% fall.
    double max_drawdown = kUndefined;

    std::size_t peak_index = 0;
    std::size_t trough_index = 0;

    /// Index at which the previous peak was regained, or npos if it never was.
    std::size_t recovery_index = static_cast<std::size_t>(-1);

    [[nodiscard]] bool recovered() const noexcept {
        return recovery_index != static_cast<std::size_t>(-1);
    }

    /// Observations spent below the peak that was eventually the worst one.
    std::size_t underwater_periods = 0;
};

[[nodiscard]] DrawdownResult max_drawdown(std::span<const double> levels) noexcept;

/// Running decline from the high-water mark, as a negative fraction per point.
/// This is the underwater plot.
[[nodiscard]] std::vector<double> drawdown_series(std::span<const double> levels);

/// CAGR divided by maximum drawdown. NaN when there was no drawdown, because
/// dividing by zero would rank a strategy that never lost anything above every
/// strategy that ever did, by an infinite margin.
[[nodiscard]] double calmar(double annual_return, double maximum_drawdown) noexcept;

// ---------------------------------------------------------------------------
// Relative to a benchmark
// ---------------------------------------------------------------------------

struct Regression {
    /// Slope: sensitivity to the benchmark.
    double beta = kUndefined;

    /// Intercept, annualised. Return not explained by the benchmark.
    double alpha = kUndefined;

    /// Fraction of variance the benchmark explains. A low value means the
    /// alpha and beta above are describing very little.
    double r_squared = kUndefined;
};

/// Least-squares regression of excess returns on benchmark excess returns.
///
/// The two series are truncated to their common length. They come from one
/// run and are sampled on one schedule, so a length mismatch means one
/// strategy started later, and pairing observation i of one with observation i
/// of the other from the front is the only alignment that does not compare
/// different instants.
[[nodiscard]] Regression regress(std::span<const double> returns,
                                 std::span<const double> benchmark_returns,
                                 const Annualisation& annualisation) noexcept;

/// Mean active return over the standard deviation of active return,
/// annualised. Active return is this series minus the benchmark, period by
/// period.
[[nodiscard]] double information_ratio(std::span<const double> returns,
                                       std::span<const double> benchmark_returns,
                                       const Annualisation& annualisation) noexcept;

/// Annualised standard deviation of active return.
[[nodiscard]] double tracking_error(std::span<const double> returns,
                                    std::span<const double> benchmark_returns,
                                    const Annualisation& annualisation) noexcept;

// ---------------------------------------------------------------------------
// Everything at once
// ---------------------------------------------------------------------------

/// One row of the leaderboard.
struct PerformanceSummary {
    std::size_t samples = 0;
    Duration span = 0;
    double years = 0.0;

    double starting_equity = kUndefined;
    double final_equity = kUndefined;

    double total_return = kUndefined;
    double cagr = kUndefined;
    double volatility = kUndefined;
    double sharpe = kUndefined;
    double sortino = kUndefined;
    double max_drawdown = kUndefined;
    double calmar = kUndefined;

    // Relative to the benchmark. All NaN when no benchmark was supplied.
    double beta = kUndefined;
    double alpha = kUndefined;
    double r_squared = kUndefined;
    double information_ratio = kUndefined;
    double tracking_error = kUndefined;
    double correlation = kUndefined;
};

/// Computes everything from an equity curve, optionally against a benchmark.
///
/// `annualisation.periods_per_year` of zero means "work it out from the
/// spacing of the curve".
[[nodiscard]] PerformanceSummary summarise(std::span<const EquityPoint> equity,
                                           std::span<const EquityPoint> benchmark,
                                           Annualisation annualisation = {});

[[nodiscard]] PerformanceSummary summarise(std::span<const EquityPoint> equity,
                                           Annualisation annualisation = {});

}  // namespace marketscan::metrics

#endif  // MARKETSCAN_METRICS_PERFORMANCE_HPP
