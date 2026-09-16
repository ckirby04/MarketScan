#include "marketscan/metrics/performance.hpp"

#include <algorithm>
#include <cmath>

namespace marketscan::metrics {
namespace {

/// Observations that are not finite are dropped rather than propagated.
///
/// A single NaN in a return series -- from a period where equity was zero, say
/// -- would otherwise turn every statistic downstream into NaN, which hides a
/// whole run's results behind one bad point. Dropping is the lesser evil, and
/// it is only ever a handful of points at the start of a curve.
[[nodiscard]] std::vector<double> finite_only(std::span<const double> values) {
    std::vector<double> result;
    result.reserve(values.size());
    for (const double value : values) {
        if (std::isfinite(value)) {
            result.push_back(value);
        }
    }
    return result;
}

[[nodiscard]] std::size_t common_length(std::size_t a, std::size_t b) noexcept {
    return a < b ? a : b;
}

/// True when a series varies only at the level of floating point noise.
///
/// Testing `sigma <= 0` is not enough, and the difference matters. Summing a
/// hundred copies of 0.01 does not give exactly a hundred times 0.01 -- the
/// accumulated rounding leaves a mean a few parts in 10^18 away from the
/// value, so the deviations are ~1e-18 rather than 0, the standard deviation
/// is ~1e-18 rather than 0, and a Sharpe ratio computed from it comes out at
/// 2e16. A strategy that never moved would then sit at the top of the
/// leaderboard with an astronomical score, which is exactly the outcome the
/// zero check was written to prevent.
///
/// Scale-relative rather than absolute: a series of returns around 1e-4 and a
/// series of prices around 1e4 have wildly different noise floors. A spread
/// twelve orders of magnitude below the data is not a signal, and is far above
/// the ~1e-14 relative error that summation actually accumulates.
[[nodiscard]] bool negligible_spread(double sigma, std::span<const double> values) noexcept {
    double scale = 0.0;
    for (const double value : values) {
        if (std::isfinite(value)) {
            scale = std::max(scale, std::fabs(value));
        }
    }
    // Written as a negated greater-than so that a NaN sigma also counts.
    return !(sigma > scale * 1e-12);
}

constexpr double kSecondsPerYear = 365.2425 * 24.0 * 3600.0;

}  // namespace

bool is_defined(double value) noexcept {
    return std::isfinite(value);
}

double Annualisation::per_period_risk_free() const noexcept {
    if (periods_per_year <= 0.0) {
        return 0.0;
    }
    if (risk_free_rate == 0.0) {
        return 0.0;
    }
    // Geometric, not arithmetic: a 5% annual rate is not 5%/252 compounded 252
    // times, and using the arithmetic split biases every Sharpe ratio slightly
    // upward.
    return std::pow(1.0 + risk_free_rate, 1.0 / periods_per_year) - 1.0;
}

double infer_periods_per_year(Duration sample_interval,
                              double trading_days_per_year,
                              double hours_per_session) noexcept {
    if (sample_interval <= 0 || trading_days_per_year <= 0.0 || hours_per_session <= 0.0) {
        return kUndefined;
    }
    const double interval_seconds =
        static_cast<double>(sample_interval) / static_cast<double>(time::kSecond);

    // A sample interval of a day or more is counted in trading days. Anything
    // shorter is counted inside the session, because the market is not open
    // between them.
    const double session_seconds = hours_per_session * 3600.0;
    if (interval_seconds >= session_seconds) {
        const double days = interval_seconds / 86400.0;
        return days <= 0.0 ? kUndefined : trading_days_per_year / days;
    }
    return trading_days_per_year * (session_seconds / interval_seconds);
}

// ---------------------------------------------------------------------------
// Series preparation
// ---------------------------------------------------------------------------

std::vector<double> equity_levels(std::span<const EquityPoint> equity) {
    std::vector<double> levels;
    levels.reserve(equity.size());
    for (const EquityPoint& point : equity) {
        levels.push_back(point.equity.to_double());
    }
    return levels;
}

std::vector<double> simple_returns(std::span<const double> levels) {
    std::vector<double> returns;
    if (levels.size() < 2) {
        return returns;
    }
    returns.reserve(levels.size() - 1);
    for (std::size_t i = 1; i < levels.size(); ++i) {
        const double previous = levels[i - 1];
        // A return is not meaningful once the account is gone. NaN says so;
        // dividing by zero would say "infinity", which is worse.
        returns.push_back(previous > 0.0 ? (levels[i] / previous) - 1.0 : kUndefined);
    }
    return returns;
}

std::vector<double> simple_returns(std::span<const EquityPoint> equity) {
    const std::vector<double> levels = equity_levels(equity);
    return simple_returns(std::span<const double>(levels));
}

std::vector<double> log_returns(std::span<const double> levels) {
    std::vector<double> returns;
    if (levels.size() < 2) {
        return returns;
    }
    returns.reserve(levels.size() - 1);
    for (std::size_t i = 1; i < levels.size(); ++i) {
        const double previous = levels[i - 1];
        const double current = levels[i];
        returns.push_back(previous > 0.0 && current > 0.0 ? std::log(current / previous)
                                                          : kUndefined);
    }
    return returns;
}

// ---------------------------------------------------------------------------
// Moments
// ---------------------------------------------------------------------------

double mean(std::span<const double> values) noexcept {
    double sum = 0.0;
    std::size_t count = 0;
    for (const double value : values) {
        if (std::isfinite(value)) {
            sum += value;
            ++count;
        }
    }
    return count == 0 ? kUndefined : sum / static_cast<double>(count);
}

double variance(std::span<const double> values) noexcept {
    const double average = mean(values);
    if (!std::isfinite(average)) {
        return kUndefined;
    }

    // Second pass over the deviations. The single-pass sum-of-squares form
    // loses the answer to cancellation when the values are small relative to
    // their mean, and can even come out negative.
    double sum_squared = 0.0;
    std::size_t count = 0;
    for (const double value : values) {
        if (std::isfinite(value)) {
            const double deviation = value - average;
            sum_squared += deviation * deviation;
            ++count;
        }
    }
    if (count < 2) {
        return kUndefined;
    }
    return sum_squared / static_cast<double>(count - 1);
}

double stddev(std::span<const double> values) noexcept {
    const double v = variance(values);
    return std::isfinite(v) ? std::sqrt(v) : kUndefined;
}

double downside_deviation(std::span<const double> values, double target) noexcept {
    double sum_squared = 0.0;
    std::size_t total = 0;
    for (const double value : values) {
        if (!std::isfinite(value)) {
            continue;
        }
        ++total;
        if (value < target) {
            const double shortfall = target - value;
            sum_squared += shortfall * shortfall;
        }
    }
    if (total < 2) {
        return kUndefined;
    }
    // Divided by the total count, not the downside count. See the header.
    return std::sqrt(sum_squared / static_cast<double>(total));
}

// ---------------------------------------------------------------------------
// Headline statistics
// ---------------------------------------------------------------------------

double total_return(std::span<const double> levels) noexcept {
    if (levels.size() < 2 || !(levels.front() > 0.0)) {
        return kUndefined;
    }
    return (levels.back() / levels.front()) - 1.0;
}

double cagr(double first_level, double last_level, double years) noexcept {
    if (!(first_level > 0.0) || !(last_level > 0.0) || !(years > 0.0)) {
        return kUndefined;
    }
    return std::pow(last_level / first_level, 1.0 / years) - 1.0;
}

double cagr(std::span<const EquityPoint> equity) noexcept {
    if (equity.size() < 2) {
        return kUndefined;
    }
    const double years = static_cast<double>(equity.back().ts - equity.front().ts) /
                         static_cast<double>(time::kSecond) / kSecondsPerYear;
    return cagr(equity.front().equity.to_double(), equity.back().equity.to_double(), years);
}

double annualised_volatility(std::span<const double> returns,
                             const Annualisation& annualisation) noexcept {
    const double sigma = stddev(returns);
    if (!std::isfinite(sigma) || !(annualisation.periods_per_year > 0.0)) {
        return kUndefined;
    }
    return sigma * std::sqrt(annualisation.periods_per_year);
}

double sharpe(std::span<const double> returns, const Annualisation& annualisation) noexcept {
    if (!(annualisation.periods_per_year > 0.0)) {
        return kUndefined;
    }

    const std::vector<double> clean = finite_only(returns);
    if (clean.size() < 2) {
        return kUndefined;
    }

    const double risk_free = annualisation.per_period_risk_free();
    std::vector<double> excess;
    excess.reserve(clean.size());
    for (const double value : clean) {
        excess.push_back(value - risk_free);
    }

    const double sigma = stddev(excess);
    // A series that never moved has no risk-adjusted return. Reporting
    // infinity -- or the 2e16 that floating point noise produces -- would put
    // it at the top of the leaderboard.
    if (!std::isfinite(sigma) || negligible_spread(sigma, std::span<const double>(excess))) {
        return kUndefined;
    }
    return (mean(excess) / sigma) * std::sqrt(annualisation.periods_per_year);
}

double sortino(std::span<const double> returns, const Annualisation& annualisation) noexcept {
    if (!(annualisation.periods_per_year > 0.0)) {
        return kUndefined;
    }

    const std::vector<double> clean = finite_only(returns);
    if (clean.size() < 2) {
        return kUndefined;
    }

    const double target = annualisation.per_period_risk_free();
    const double downside = downside_deviation(clean, target);
    if (!std::isfinite(downside) || negligible_spread(downside, std::span<const double>(clean))) {
        // Nothing ever fell below the target, or only by an amount indistin-
        // guishable from rounding. That is not an infinite Sortino ratio; it
        // is a series with no downside to measure.
        return kUndefined;
    }
    return ((mean(clean) - target) / downside) * std::sqrt(annualisation.periods_per_year);
}

DrawdownResult max_drawdown(std::span<const double> levels) noexcept {
    DrawdownResult result;
    if (levels.size() < 2) {
        return result;
    }

    double peak = kUndefined;
    std::size_t peak_index = 0;
    double worst = 0.0;
    std::size_t worst_peak = 0;
    std::size_t worst_trough = 0;

    for (std::size_t i = 0; i < levels.size(); ++i) {
        const double level = levels[i];
        if (!std::isfinite(level)) {
            continue;
        }
        if (!std::isfinite(peak) || level > peak) {
            peak = level;
            peak_index = i;
        }
        if (peak > 0.0) {
            const double decline = 1.0 - (level / peak);
            if (decline > worst) {
                worst = decline;
                worst_peak = peak_index;
                worst_trough = i;
            }
        }
    }

    result.max_drawdown = worst;
    result.peak_index = worst_peak;
    result.trough_index = worst_trough;

    // How long it stayed below that peak, and whether it ever got back.
    if (worst > 0.0) {
        const double peak_level = levels[worst_peak];
        for (std::size_t i = worst_trough + 1; i < levels.size(); ++i) {
            if (std::isfinite(levels[i]) && levels[i] >= peak_level) {
                result.recovery_index = i;
                break;
            }
        }
        const std::size_t end = result.recovered() ? result.recovery_index : levels.size();
        result.underwater_periods = end - worst_peak;
    }
    return result;
}

std::vector<double> drawdown_series(std::span<const double> levels) {
    std::vector<double> series;
    series.reserve(levels.size());

    double peak = kUndefined;
    for (const double level : levels) {
        if (!std::isfinite(level)) {
            series.push_back(kUndefined);
            continue;
        }
        if (!std::isfinite(peak) || level > peak) {
            peak = level;
        }
        // Negative, so the underwater plot hangs below zero.
        series.push_back(peak > 0.0 ? (level / peak) - 1.0 : kUndefined);
    }
    return series;
}

double calmar(double annual_return, double maximum_drawdown) noexcept {
    if (!std::isfinite(annual_return) || !std::isfinite(maximum_drawdown)) {
        return kUndefined;
    }
    // No drawdown is not an infinitely good result; it is usually a run too
    // short to have had one yet.
    if (maximum_drawdown <= 0.0) {
        return kUndefined;
    }
    return annual_return / maximum_drawdown;
}

// ---------------------------------------------------------------------------
// Relative to a benchmark
// ---------------------------------------------------------------------------

Regression regress(std::span<const double> returns,
                   std::span<const double> benchmark_returns,
                   const Annualisation& annualisation) noexcept {
    Regression result;

    const std::size_t n = common_length(returns.size(), benchmark_returns.size());
    if (n < 2) {
        return result;
    }

    const double risk_free = annualisation.per_period_risk_free();

    // Pairwise complete: a period is used only when both series have a finite
    // observation for it, because a regression on misaligned pairs is not a
    // regression on anything.
    std::vector<double> y;
    std::vector<double> x;
    y.reserve(n);
    x.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(returns[i]) && std::isfinite(benchmark_returns[i])) {
            y.push_back(returns[i] - risk_free);
            x.push_back(benchmark_returns[i] - risk_free);
        }
    }
    if (y.size() < 2) {
        return result;
    }

    const double mean_x = mean(x);
    const double mean_y = mean(y);
    if (!std::isfinite(mean_x) || !std::isfinite(mean_y)) {
        return result;
    }

    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double dx = x[i] - mean_x;
        const double dy = y[i] - mean_y;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }

    // A benchmark that never moved explains nothing, and the slope of a
    // vertical line is not a number. Compared against the scale of the data
    // rather than against zero, for the reason negligible_spread explains.
    double scale_x = 0.0;
    for (const double value : x) {
        scale_x = std::max(scale_x, std::fabs(value));
    }
    if (!(sxx > scale_x * scale_x * 1e-24 * static_cast<double>(x.size()))) {
        return result;
    }

    result.beta = sxy / sxx;
    const double intercept = mean_y - result.beta * mean_x;
    result.alpha = intercept * annualisation.periods_per_year;
    result.r_squared = syy > 0.0 ? (sxy * sxy) / (sxx * syy) : kUndefined;
    return result;
}

double tracking_error(std::span<const double> returns,
                      std::span<const double> benchmark_returns,
                      const Annualisation& annualisation) noexcept {
    const std::size_t n = common_length(returns.size(), benchmark_returns.size());
    if (n < 2 || !(annualisation.periods_per_year > 0.0)) {
        return kUndefined;
    }

    std::vector<double> active;
    active.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(returns[i]) && std::isfinite(benchmark_returns[i])) {
            active.push_back(returns[i] - benchmark_returns[i]);
        }
    }

    const double sigma = stddev(active);
    return std::isfinite(sigma) ? sigma * std::sqrt(annualisation.periods_per_year) : kUndefined;
}

double information_ratio(std::span<const double> returns,
                         std::span<const double> benchmark_returns,
                         const Annualisation& annualisation) noexcept {
    const std::size_t n = common_length(returns.size(), benchmark_returns.size());
    if (n < 2 || !(annualisation.periods_per_year > 0.0)) {
        return kUndefined;
    }

    std::vector<double> active;
    active.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(returns[i]) && std::isfinite(benchmark_returns[i])) {
            active.push_back(returns[i] - benchmark_returns[i]);
        }
    }
    if (active.size() < 2) {
        return kUndefined;
    }

    const double sigma = stddev(active);
    // Tracking the benchmark exactly is not infinite skill at beating it.
    if (!std::isfinite(sigma) || negligible_spread(sigma, std::span<const double>(active))) {
        return kUndefined;
    }
    return (mean(active) / sigma) * std::sqrt(annualisation.periods_per_year);
}

// ---------------------------------------------------------------------------
// Everything at once
// ---------------------------------------------------------------------------

PerformanceSummary summarise(std::span<const EquityPoint> equity,
                             std::span<const EquityPoint> benchmark,
                             Annualisation annualisation) {
    PerformanceSummary summary;
    summary.samples = equity.size();
    if (equity.size() < 2) {
        return summary;
    }

    summary.span = equity.back().ts - equity.front().ts;
    summary.years =
        static_cast<double>(summary.span) / static_cast<double>(time::kSecond) / kSecondsPerYear;

    // Work the sampling rate out from the curve if the caller did not say.
    if (!(annualisation.periods_per_year > 0.0)) {
        const Duration interval = summary.span / static_cast<Duration>(equity.size() - 1);
        annualisation.periods_per_year = infer_periods_per_year(interval);
    }

    const std::vector<double> levels = equity_levels(equity);
    const std::vector<double> returns = simple_returns(std::span<const double>(levels));

    summary.starting_equity = levels.front();
    summary.final_equity = levels.back();
    summary.total_return = total_return(levels);
    summary.cagr = cagr(levels.front(), levels.back(), summary.years);
    summary.volatility = annualised_volatility(returns, annualisation);
    summary.sharpe = sharpe(returns, annualisation);
    summary.sortino = sortino(returns, annualisation);

    const DrawdownResult drawdown = max_drawdown(levels);
    summary.max_drawdown = drawdown.max_drawdown;
    summary.calmar = calmar(summary.cagr, summary.max_drawdown);

    if (benchmark.size() >= 2) {
        const std::vector<double> benchmark_levels = equity_levels(benchmark);
        const std::vector<double> benchmark_returns =
            simple_returns(std::span<const double>(benchmark_levels));

        const Regression regression = regress(returns, benchmark_returns, annualisation);
        summary.beta = regression.beta;
        summary.alpha = regression.alpha;
        summary.r_squared = regression.r_squared;
        summary.information_ratio = information_ratio(returns, benchmark_returns, annualisation);
        summary.tracking_error = tracking_error(returns, benchmark_returns, annualisation);

        const std::size_t n = common_length(returns.size(), benchmark_returns.size());
        summary.correlation = correlation(std::span<const double>(returns).first(n),
                                          std::span<const double>(benchmark_returns).first(n));
    }
    return summary;
}

PerformanceSummary summarise(std::span<const EquityPoint> equity, Annualisation annualisation) {
    return summarise(equity, std::span<const EquityPoint>{}, annualisation);
}

}  // namespace marketscan::metrics
