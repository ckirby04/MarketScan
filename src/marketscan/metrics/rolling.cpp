#include "marketscan/metrics/rolling.hpp"

#include <algorithm>
#include <cmath>

#include "marketscan/metrics/correlation.hpp"

namespace marketscan::metrics {
namespace {

/// Applies `fn` to each trailing window of `values`, writing NaN until the
/// window fills. Every rolling function below is this loop plus one statistic.
template<typename Fn>
[[nodiscard]] std::vector<double>
rolling(std::span<const double> values, std::size_t window, Fn&& fn) {
    std::vector<double> result(values.size(), kUndefined);
    if (window == 0 || values.size() < window) {
        return result;
    }
    for (std::size_t end = window; end <= values.size(); ++end) {
        const std::span<const double> slice = values.subspan(end - window, window);
        result[end - 1] = fn(slice);
    }
    return result;
}

template<typename Fn>
[[nodiscard]] std::vector<double>
rolling_pair(std::span<const double> a, std::span<const double> b, std::size_t window, Fn&& fn) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    std::vector<double> result(n, kUndefined);
    if (window == 0 || n < window) {
        return result;
    }
    for (std::size_t end = window; end <= n; ++end) {
        result[end - 1] = fn(a.subspan(end - window, window), b.subspan(end - window, window));
    }
    return result;
}

}  // namespace

RollingWindow::RollingWindow(std::size_t window)
    : buffer_(window == 0 ? 1 : window), window_(window == 0 ? 1 : window) {}

void RollingWindow::push(double value) {
    buffer_[next_] = value;
    next_ = (next_ + 1) % window_;
    ++count_;
}

void RollingWindow::clear() noexcept {
    next_ = 0;
    count_ = 0;
}

std::vector<double> RollingWindow::values() const {
    std::vector<double> ordered;
    const std::size_t held = size();
    ordered.reserve(held);
    // Oldest first. Before the ring wraps that is slot zero; afterwards it is
    // whichever slot the next push would overwrite.
    const std::size_t start = count_ < window_ ? 0 : next_;
    for (std::size_t i = 0; i < held; ++i) {
        ordered.push_back(buffer_[(start + i) % window_]);
    }
    return ordered;
}

double RollingWindow::mean() const {
    if (!full()) {
        return kUndefined;
    }
    const std::vector<double> ordered = values();
    return metrics::mean(std::span<const double>(ordered));
}

double RollingWindow::variance() const {
    if (!full()) {
        return kUndefined;
    }
    const std::vector<double> ordered = values();
    return metrics::variance(std::span<const double>(ordered));
}

double RollingWindow::stddev() const {
    const double v = variance();
    return std::isfinite(v) ? std::sqrt(v) : kUndefined;
}

std::vector<double> rolling_mean(std::span<const double> values, std::size_t window) {
    return rolling(
        values, window, [](std::span<const double> slice) { return metrics::mean(slice); });
}

std::vector<double> rolling_stddev(std::span<const double> values, std::size_t window) {
    return rolling(
        values, window, [](std::span<const double> slice) { return metrics::stddev(slice); });
}

std::vector<double> rolling_sharpe(std::span<const double> returns,
                                   std::size_t window,
                                   const Annualisation& annualisation) {
    return rolling(returns, window, [&](std::span<const double> slice) {
        return metrics::sharpe(slice, annualisation);
    });
}

std::vector<double> rolling_sortino(std::span<const double> returns,
                                    std::size_t window,
                                    const Annualisation& annualisation) {
    return rolling(returns, window, [&](std::span<const double> slice) {
        return metrics::sortino(slice, annualisation);
    });
}

std::vector<double> rolling_volatility(std::span<const double> returns,
                                       std::size_t window,
                                       const Annualisation& annualisation) {
    return rolling(returns, window, [&](std::span<const double> slice) {
        return metrics::annualised_volatility(slice, annualisation);
    });
}

std::vector<double>
rolling_correlation(std::span<const double> a, std::span<const double> b, std::size_t window) {
    return rolling_pair(a, b, window, [](std::span<const double> x, std::span<const double> y) {
        return metrics::correlation(x, y);
    });
}

std::vector<double> rolling_beta(std::span<const double> returns,
                                 std::span<const double> benchmark_returns,
                                 std::size_t window) {
    return rolling_pair(returns,
                        benchmark_returns,
                        window,
                        [](std::span<const double> y, std::span<const double> x) {
                            // Beta is covariance over the benchmark's variance, which is the
                            // regression slope without the risk-free adjustment. A rolling
                            // beta is about co-movement, and subtracting a constant from both
                            // series would not change it anyway.
                            const double denominator = metrics::variance(x);
                            if (!std::isfinite(denominator) || denominator <= 0.0) {
                                return kUndefined;
                            }
                            const double numerator = metrics::covariance(y, x);
                            return std::isfinite(numerator) ? numerator / denominator : kUndefined;
                        });
}

std::vector<double> rolling_max_drawdown(std::span<const double> levels, std::size_t window) {
    return rolling(levels, window, [](std::span<const double> slice) {
        return metrics::max_drawdown(slice).max_drawdown;
    });
}

std::vector<double> relative_strength(std::span<const double> levels,
                                      std::span<const double> benchmark_levels) {
    const std::size_t n =
        levels.size() < benchmark_levels.size() ? levels.size() : benchmark_levels.size();
    std::vector<double> result(n, kUndefined);
    if (n == 0) {
        return result;
    }

    const double base = levels.front();
    const double benchmark_base = benchmark_levels.front();
    if (!(base > 0.0) || !(benchmark_base > 0.0)) {
        return result;
    }

    for (std::size_t i = 0; i < n; ++i) {
        const double benchmark = benchmark_levels[i];
        if (!std::isfinite(levels[i]) || !(benchmark > 0.0)) {
            continue;
        }
        // Both normalised to 1 at the start, so the line is "how much better,
        // proportionally, than the benchmark" and a flat stretch means they
        // moved together.
        result[i] = (levels[i] / base) / (benchmark / benchmark_base);
    }
    return result;
}

}  // namespace marketscan::metrics
