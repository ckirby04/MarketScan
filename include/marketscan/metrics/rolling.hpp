#ifndef MARKETSCAN_METRICS_ROLLING_HPP
#define MARKETSCAN_METRICS_ROLLING_HPP

#include <cstddef>
#include <span>
#include <vector>

#include "marketscan/metrics/performance.hpp"

namespace marketscan::metrics {

/// A fixed-length window of the most recent observations.
///
/// Statistics are computed over the window by a two-pass scan rather than by
/// maintaining running sums. Running sums are O(1) per update and wrong: a
/// running sum of squares over a long return series loses the variance to
/// cancellation, and a running sum that has values subtracted from it as they
/// leave the window accumulates error without bound, so a rolling volatility
/// slowly drifts and can go negative. At the window sizes a chart uses, O(w)
/// per point costs nothing worth having.
class RollingWindow {
public:
    explicit RollingWindow(std::size_t window);

    void push(double value);

    void clear() noexcept;

    [[nodiscard]] bool full() const noexcept { return count_ >= window_; }

    [[nodiscard]] std::size_t size() const noexcept { return count_ < window_ ? count_ : window_; }

    [[nodiscard]] std::size_t window() const noexcept { return window_; }

    /// The window in chronological order. Valid until the next push.
    [[nodiscard]] std::vector<double> values() const;

    /// NaN until the window is full, so that a chart does not open with a
    /// statistic computed from three points and presented like the rest.
    [[nodiscard]] double mean() const;
    [[nodiscard]] double variance() const;
    [[nodiscard]] double stddev() const;

private:
    std::vector<double> buffer_;
    std::size_t window_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

/// Each rolling function returns a series the same length as its input, with
/// NaN in the positions before the window fills. Same length so that a chart
/// can plot it against the same x axis without an offset nobody remembers.

[[nodiscard]] std::vector<double> rolling_mean(std::span<const double> values, std::size_t window);

[[nodiscard]] std::vector<double> rolling_stddev(std::span<const double> values,
                                                 std::size_t window);

/// Annualised, using the same convention as the non-rolling Sharpe ratio.
[[nodiscard]] std::vector<double> rolling_sharpe(std::span<const double> returns,
                                                 std::size_t window,
                                                 const Annualisation& annualisation);

[[nodiscard]] std::vector<double> rolling_sortino(std::span<const double> returns,
                                                  std::size_t window,
                                                  const Annualisation& annualisation);

[[nodiscard]] std::vector<double> rolling_volatility(std::span<const double> returns,
                                                     std::size_t window,
                                                     const Annualisation& annualisation);

[[nodiscard]] std::vector<double>
rolling_correlation(std::span<const double> a, std::span<const double> b, std::size_t window);

[[nodiscard]] std::vector<double> rolling_beta(std::span<const double> returns,
                                               std::span<const double> benchmark_returns,
                                               std::size_t window);

/// Worst drawdown seen within each trailing window.
[[nodiscard]] std::vector<double> rolling_max_drawdown(std::span<const double> levels,
                                                       std::size_t window);

/// Series divided by benchmark, normalised so both start at 1.
///
/// This is the relative strength plot. Normalised because the interesting
/// question is which one grew faster, and an un-normalised ratio of two curves
/// that started at different levels answers a different one.
[[nodiscard]] std::vector<double> relative_strength(std::span<const double> levels,
                                                    std::span<const double> benchmark_levels);

}  // namespace marketscan::metrics

#endif  // MARKETSCAN_METRICS_ROLLING_HPP
