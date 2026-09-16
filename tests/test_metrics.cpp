// Metrics, checked against numbers worked out by hand.
//
// Every expected value below is either exact or derived in the comment beside
// it. A metrics test that compares the implementation against itself proves
// only that it is consistent, which is not the property anyone cares about.

#include <cmath>
#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "marketscan/metrics/correlation.hpp"
#include "marketscan/metrics/performance.hpp"
#include "marketscan/metrics/rolling.hpp"

using namespace marketscan;
using namespace marketscan::metrics;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::span<const double> view(const std::vector<double>& values) {
    return std::span<const double>(values);
}

std::vector<EquityPoint> curve(const std::vector<double>& levels, Duration interval = time::kDay) {
    std::vector<EquityPoint> points;
    points.reserve(levels.size());
    Timestamp ts = 0;
    for (const double level : levels) {
        points.push_back(EquityPoint{ts, Money::from_double(level), Money{}});
        ts += interval;
    }
    return points;
}

/// Every undefined result is NaN, and the tests say so explicitly rather than
/// comparing against zero.
void require_undefined(double value) {
    REQUIRE_FALSE(is_defined(value));
}

}  // namespace

// --------------------------------------------------------------------------
// Moments
// --------------------------------------------------------------------------

TEST_CASE("mean and variance against a worked example", "[metrics]") {
    // The textbook set: 2 4 4 4 5 5 7 9.
    // mean = 40/8 = 5
    // squared deviations: 9 + 1 + 1 + 1 + 0 + 0 + 4 + 16 = 32
    // sample variance = 32/7, sample sd = sqrt(32/7)
    const std::vector<double> values{2, 4, 4, 4, 5, 5, 7, 9};

    REQUIRE_THAT(mean(view(values)), WithinRel(5.0, 1e-15));
    REQUIRE_THAT(variance(view(values)), WithinRel(32.0 / 7.0, 1e-15));
    REQUIRE_THAT(stddev(view(values)), WithinRel(std::sqrt(32.0 / 7.0), 1e-15));
}

TEST_CASE("moments of too-short series are undefined", "[metrics]") {
    const std::vector<double> empty;
    const std::vector<double> single{1.0};

    require_undefined(mean(view(empty)));
    require_undefined(variance(view(empty)));
    require_undefined(variance(view(single)));
    REQUIRE_THAT(mean(view(single)), WithinRel(1.0, 1e-15));
}

// A NaN partway through a series must not hide the whole run's results.
TEST_CASE("non-finite observations are skipped, not propagated", "[metrics]") {
    const std::vector<double> values{2.0, kUndefined, 4.0, 6.0};
    REQUIRE_THAT(mean(view(values)), WithinRel(4.0, 1e-15));
    REQUIRE(is_defined(variance(view(values))));
}

TEST_CASE("downside deviation divides by the total count", "[metrics]") {
    // -0.01 and -0.02 fall below zero; 0.02 and 0.03 do not.
    // sum of squared shortfalls = 0.0001 + 0.0004 = 0.0005
    // divided by 4 (not 2) = 0.000125, sqrt = 0.011180339887...
    const std::vector<double> returns{0.02, -0.01, 0.03, -0.02};
    REQUIRE_THAT(downside_deviation(view(returns), 0.0), WithinRel(std::sqrt(0.000125), 1e-14));
}

TEST_CASE("a series with no downside has no downside deviation", "[metrics]") {
    const std::vector<double> returns{0.01, 0.02, 0.03};
    REQUIRE_THAT(downside_deviation(view(returns), 0.0), WithinAbs(0.0, 1e-15));
}

// --------------------------------------------------------------------------
// Returns
// --------------------------------------------------------------------------

TEST_CASE("simple returns", "[metrics]") {
    const std::vector<double> levels{100.0, 110.0, 99.0};
    const std::vector<double> returns = simple_returns(view(levels));

    REQUIRE(returns.size() == 2);
    REQUIRE_THAT(returns[0], WithinRel(0.10, 1e-15));
    REQUIRE_THAT(returns[1], WithinRel(-0.10, 1e-15));
}

// A return is not meaningful once the account is gone, and an infinity here
// would poison every statistic downstream.
TEST_CASE("a return from zero equity is undefined, not infinite", "[metrics]") {
    const std::vector<double> levels{100.0, 0.0, 50.0};
    const std::vector<double> returns = simple_returns(view(levels));

    REQUIRE(returns.size() == 2);
    REQUIRE_THAT(returns[0], WithinRel(-1.0, 1e-15));
    require_undefined(returns[1]);
}

TEST_CASE("log returns compound to the total", "[metrics]") {
    const std::vector<double> levels{100.0, 110.0, 121.0};
    const std::vector<double> logs = log_returns(view(levels));

    REQUIRE(logs.size() == 2);
    REQUIRE_THAT(std::exp(logs[0] + logs[1]), WithinRel(1.21, 1e-14));
}

TEST_CASE("total return", "[metrics]") {
    const std::vector<double> levels{100.0, 150.0};
    REQUIRE_THAT(total_return(view(levels)), WithinRel(0.5, 1e-15));
}

// --------------------------------------------------------------------------
// CAGR
// --------------------------------------------------------------------------

TEST_CASE("CAGR against an exact case", "[metrics]") {
    // 100 -> 121 over two years is exactly 10% a year, because 1.1^2 = 1.21.
    REQUIRE_THAT(cagr(100.0, 121.0, 2.0), WithinRel(0.10, 1e-14));

    // Halving over one year.
    REQUIRE_THAT(cagr(100.0, 50.0, 1.0), WithinRel(-0.5, 1e-14));

    // Unchanged is zero growth, however long it took.
    REQUIRE_THAT(cagr(100.0, 100.0, 7.0), WithinAbs(0.0, 1e-14));
}

TEST_CASE("CAGR of a degenerate span is undefined", "[metrics]") {
    require_undefined(cagr(100.0, 121.0, 0.0));
    require_undefined(cagr(0.0, 121.0, 1.0));
    require_undefined(cagr(100.0, 0.0, 1.0));
}

// --------------------------------------------------------------------------
// Sharpe and Sortino
// --------------------------------------------------------------------------

TEST_CASE("Sharpe ratio against a worked example", "[metrics]") {
    // returns: 0.02, 0.01, 0.03, 0.00
    // mean = 0.015
    // deviations: 0.005, -0.005, 0.015, -0.015
    // sum of squares = 25e-6 + 25e-6 + 225e-6 + 225e-6 = 500e-6
    // sample variance = 500e-6 / 3, sd = sqrt(500e-6/3) = 0.0129099444...
    // Sharpe per period = 0.015 / 0.0129099444 = 1.1618950...
    const std::vector<double> returns{0.02, 0.01, 0.03, 0.00};

    Annualisation per_period;
    per_period.periods_per_year = 1.0;
    REQUIRE_THAT(sharpe(view(returns), per_period),
                 WithinRel(0.015 / std::sqrt(500e-6 / 3.0), 1e-13));

    Annualisation daily;
    daily.periods_per_year = 252.0;
    REQUIRE_THAT(sharpe(view(returns), daily),
                 WithinRel(0.015 / std::sqrt(500e-6 / 3.0) * std::sqrt(252.0), 1e-13));
}

// A constant series has no risk-adjusted return. Reporting infinity would put
// a strategy that never moved at the top of the leaderboard.
TEST_CASE("Sharpe of a riskless series is undefined, not infinite", "[metrics]") {
    const std::vector<double> flat{0.01, 0.01, 0.01, 0.01};
    require_undefined(sharpe(view(flat), Annualisation{}));

    const std::vector<double> zero{0.0, 0.0, 0.0};
    require_undefined(sharpe(view(zero), Annualisation{}));
}

TEST_CASE("Sharpe of a zero-mean series is zero, which is a real answer", "[metrics]") {
    const std::vector<double> returns{0.01, -0.01, 0.01, -0.01};
    const double result = sharpe(view(returns), Annualisation{});
    REQUIRE(is_defined(result));
    REQUIRE_THAT(result, WithinAbs(0.0, 1e-13));
}

TEST_CASE("the risk-free rate is split geometrically", "[metrics]") {
    Annualisation annualisation;
    annualisation.periods_per_year = 252.0;
    annualisation.risk_free_rate = 0.05;

    // Compounding the per-period rate 252 times must recover the annual rate.
    const double per_period = annualisation.per_period_risk_free();
    REQUIRE_THAT(std::pow(1.0 + per_period, 252.0) - 1.0, WithinRel(0.05, 1e-12));

    // And it is slightly below the naive arithmetic split.
    REQUIRE(per_period < 0.05 / 252.0);
}

TEST_CASE("a risk-free rate lowers the Sharpe ratio", "[metrics]") {
    const std::vector<double> returns{0.02, 0.01, 0.03, 0.00};

    Annualisation without;
    Annualisation with;
    with.risk_free_rate = 0.05;

    REQUIRE(sharpe(view(returns), with) < sharpe(view(returns), without));
}

TEST_CASE("Sortino ratio against a worked example", "[metrics]") {
    // returns: 0.02, -0.01, 0.03, -0.02
    // mean = 0.005
    // downside deviation (divided by 4) = sqrt(0.000125)
    // Sortino per period = 0.005 / sqrt(0.000125)
    const std::vector<double> returns{0.02, -0.01, 0.03, -0.02};

    Annualisation per_period;
    per_period.periods_per_year = 1.0;
    REQUIRE_THAT(sortino(view(returns), per_period), WithinRel(0.005 / std::sqrt(0.000125), 1e-13));
}

// A series that never fell below the target has no downside to measure. That
// is not an infinitely good Sortino ratio.
TEST_CASE("Sortino with no downside is undefined", "[metrics]") {
    const std::vector<double> returns{0.01, 0.02, 0.03};
    require_undefined(sortino(view(returns), Annualisation{}));
}

TEST_CASE("Sortino exceeds Sharpe when losses are smaller than gains", "[metrics]") {
    // Big up moves, small down moves: total volatility is larger than
    // downside volatility, so penalising only the downside scores better.
    const std::vector<double> returns{0.05, -0.01, 0.06, -0.01, 0.04, -0.01};
    REQUIRE(sortino(view(returns), Annualisation{}) > sharpe(view(returns), Annualisation{}));
}

// --------------------------------------------------------------------------
// Drawdown
// --------------------------------------------------------------------------

TEST_CASE("maximum drawdown against a worked example", "[metrics]") {
    // Peak 120 at index 1, trough 60 at index 2: a 50% decline.
    // The peak is regained at index 4, where the level reaches 130.
    const std::vector<double> levels{100, 120, 60, 80, 130};
    const DrawdownResult result = max_drawdown(view(levels));

    REQUIRE_THAT(result.max_drawdown, WithinRel(0.5, 1e-15));
    REQUIRE(result.peak_index == 1);
    REQUIRE(result.trough_index == 2);
    REQUIRE(result.recovered());
    REQUIRE(result.recovery_index == 4);
    REQUIRE(result.underwater_periods == 3);
}

TEST_CASE("a drawdown that never recovers is reported as such", "[metrics]") {
    const std::vector<double> levels{100, 200, 100, 150, 120};
    const DrawdownResult result = max_drawdown(view(levels));

    REQUIRE_THAT(result.max_drawdown, WithinRel(0.5, 1e-15));
    REQUIRE_FALSE(result.recovered());
}

TEST_CASE("a monotonic rise has no drawdown", "[metrics]") {
    const std::vector<double> levels{100, 110, 120, 130};
    REQUIRE_THAT(max_drawdown(view(levels)).max_drawdown, WithinAbs(0.0, 1e-15));
}

TEST_CASE("the underwater series hangs below zero", "[metrics]") {
    const std::vector<double> levels{100, 120, 60, 80, 130};
    const std::vector<double> underwater = drawdown_series(view(levels));

    REQUIRE(underwater.size() == 5);
    REQUIRE_THAT(underwater[0], WithinAbs(0.0, 1e-15));         // at a new high
    REQUIRE_THAT(underwater[1], WithinAbs(0.0, 1e-15));         // at a new high
    REQUIRE_THAT(underwater[2], WithinRel(-0.5, 1e-15));        // 60/120 - 1
    REQUIRE_THAT(underwater[3], WithinRel(-1.0 / 3.0, 1e-14));  // 80/120 - 1
    REQUIRE_THAT(underwater[4], WithinAbs(0.0, 1e-15));         // new high again
}

TEST_CASE("Calmar ratio", "[metrics]") {
    REQUIRE_THAT(calmar(0.20, 0.10), WithinRel(2.0, 1e-15));
    REQUIRE_THAT(calmar(-0.10, 0.20), WithinRel(-0.5, 1e-15));

    // A run with no drawdown is almost always a run too short to have had one,
    // not a strategy with infinite risk-adjusted return.
    require_undefined(calmar(0.20, 0.0));
}

// --------------------------------------------------------------------------
// Against a benchmark
// --------------------------------------------------------------------------

TEST_CASE("regression recovers an exact slope", "[metrics]") {
    // Constructed so the answer is not in doubt: returns are exactly twice the
    // benchmark, with no noise.
    const std::vector<double> benchmark{0.01, 0.02, -0.01, 0.03, -0.02};
    std::vector<double> returns;
    for (const double value : benchmark) {
        returns.push_back(2.0 * value);
    }

    Annualisation per_period;
    per_period.periods_per_year = 1.0;
    const Regression regression = regress(view(returns), view(benchmark), per_period);

    REQUIRE_THAT(regression.beta, WithinRel(2.0, 1e-13));
    REQUIRE_THAT(regression.alpha, WithinAbs(0.0, 1e-14));
    REQUIRE_THAT(regression.r_squared, WithinRel(1.0, 1e-13));
}

TEST_CASE("regression recovers an exact intercept", "[metrics]") {
    // Twice the benchmark, plus a constant 1% every period. With one period
    // per year, the annualised alpha is that 1%.
    const std::vector<double> benchmark{0.01, 0.02, -0.01, 0.03, -0.02};
    std::vector<double> returns;
    for (const double value : benchmark) {
        returns.push_back(2.0 * value + 0.01);
    }

    Annualisation per_period;
    per_period.periods_per_year = 1.0;
    const Regression regression = regress(view(returns), view(benchmark), per_period);

    REQUIRE_THAT(regression.beta, WithinRel(2.0, 1e-13));
    REQUIRE_THAT(regression.alpha, WithinRel(0.01, 1e-12));

    // Annualising multiplies the intercept by the period count.
    Annualisation daily;
    daily.periods_per_year = 252.0;
    REQUIRE_THAT(regress(view(returns), view(benchmark), daily).alpha,
                 WithinRel(0.01 * 252.0, 1e-12));
}

// A benchmark that never moved explains nothing, and the slope of a vertical
// line is not a number.
TEST_CASE("regression against a flat benchmark is undefined", "[metrics]") {
    const std::vector<double> benchmark{0.01, 0.01, 0.01, 0.01};
    const std::vector<double> returns{0.02, -0.01, 0.03, 0.00};

    const Regression regression = regress(view(returns), view(benchmark), Annualisation{});
    require_undefined(regression.beta);
    require_undefined(regression.alpha);
}

TEST_CASE("information ratio against a worked example", "[metrics]") {
    // Active return is a constant 1% every period except one, so it has both a
    // mean and a spread to divide by.
    const std::vector<double> benchmark{0.01, 0.01, 0.01, 0.01};
    const std::vector<double> returns{0.02, 0.02, 0.02, 0.03};
    // active: 0.01, 0.01, 0.01, 0.02; mean = 0.0125
    // deviations: -0.0025 x3, 0.0075; squares: 6.25e-6 x3 + 56.25e-6 = 75e-6
    // sample variance = 25e-6, sd = 0.005
    // IR per period = 0.0125 / 0.005 = 2.5
    Annualisation per_period;
    per_period.periods_per_year = 1.0;
    REQUIRE_THAT(information_ratio(view(returns), view(benchmark), per_period),
                 WithinRel(2.5, 1e-12));
}

// Tracking the benchmark exactly is not infinite skill at beating it.
TEST_CASE("information ratio against an identical series is undefined", "[metrics]") {
    const std::vector<double> series{0.01, -0.02, 0.03};
    require_undefined(information_ratio(view(series), view(series), Annualisation{}));
    REQUIRE_THAT(tracking_error(view(series), view(series), Annualisation{}),
                 WithinAbs(0.0, 1e-15));
}

// --------------------------------------------------------------------------
// Correlation
// --------------------------------------------------------------------------

TEST_CASE("correlation of the obvious cases", "[metrics][correlation]") {
    const std::vector<double> a{1, 2, 3, 4, 5};
    std::vector<double> same_direction;
    std::vector<double> opposite;
    for (const double value : a) {
        same_direction.push_back(3.0 * value + 7.0);
        opposite.push_back(-2.0 * value + 1.0);
    }

    REQUIRE_THAT(correlation(view(a), view(same_direction)), WithinRel(1.0, 1e-13));
    REQUIRE_THAT(correlation(view(a), view(opposite)), WithinRel(-1.0, 1e-13));
    REQUIRE_THAT(correlation(view(a), view(a)), WithinRel(1.0, 1e-13));
}

TEST_CASE("correlation stays inside its range", "[metrics][correlation]") {
    // The arithmetic can land a few ulps outside [-1, 1], and a correlation of
    // 1.0000000000000002 breaks every plot axis it touches.
    const std::vector<double> a{1e-9, 2e-9, 3e-9, 4e-9};
    std::vector<double> b;
    for (const double value : a) {
        b.push_back(value * 1e9);
    }
    const double r = correlation(view(a), view(b));
    REQUIRE(r <= 1.0);
    REQUIRE(r >= -1.0);
}

TEST_CASE("correlation with a flat series is undefined", "[metrics][correlation]") {
    const std::vector<double> moving{1, 2, 3, 4};
    const std::vector<double> flat{5, 5, 5, 5};
    require_undefined(correlation(view(moving), view(flat)));
    require_undefined(correlation(view(flat), view(flat)));
}

TEST_CASE("covariance against a worked example", "[metrics][correlation]") {
    // a: 1 2 3 4, mean 2.5; b: 2 4 6 8, mean 5
    // products of deviations: (-1.5)(-3) + (-0.5)(-1) + (0.5)(1) + (1.5)(3)
    //                       = 4.5 + 0.5 + 0.5 + 4.5 = 10
    // sample covariance = 10/3
    const std::vector<double> a{1, 2, 3, 4};
    const std::vector<double> b{2, 4, 6, 8};
    REQUIRE_THAT(covariance(view(a), view(b)), WithinRel(10.0 / 3.0, 1e-14));
}

TEST_CASE("the correlation matrix is symmetric with a unit diagonal", "[metrics][correlation]") {
    const std::vector<std::vector<double>> series{
        {1, 2, 3, 4, 5},
        {2, 4, 6, 8, 10},  // perfectly correlated with the first
        {5, 4, 3, 2, 1},   // perfectly anti-correlated
    };
    const CorrelationMatrix matrix = correlation_matrix(series, {"alpha", "beta", "gamma"});

    REQUIRE(matrix.size() == 3);
    REQUIRE(matrix.labels()[1] == "beta");

    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE_THAT(matrix.at(i, i), WithinRel(1.0, 1e-13));
        for (std::size_t j = 0; j < 3; ++j) {
            REQUIRE_THAT(matrix.at(i, j), WithinAbs(matrix.at(j, i), 1e-15));
        }
    }
    REQUIRE_THAT(matrix.at(0, 1), WithinRel(1.0, 1e-13));
    REQUIRE_THAT(matrix.at(0, 2), WithinRel(-1.0, 1e-13));

    std::size_t row = 0;
    std::size_t column = 0;
    double value = 0.0;
    REQUIRE(matrix.most_correlated(row, column, value));
    REQUIRE(row == 0);
    REQUIRE(column == 1);
    REQUIRE_THAT(value, WithinRel(1.0, 1e-13));
}

TEST_CASE("a matrix entry out of range is undefined, not a crash", "[metrics][correlation]") {
    const CorrelationMatrix matrix = correlation_matrix({{1, 2, 3}}, {"only"});
    require_undefined(matrix.at(5, 0));
    require_undefined(matrix.at(0, 5));
}

// A strategy that never traded is not perfectly correlated with itself; it is
// simply not something correlation applies to.
TEST_CASE("a flat series has an undefined diagonal", "[metrics][correlation]") {
    const CorrelationMatrix matrix = correlation_matrix({{1, 2, 3}, {7, 7, 7}}, {"moving", "flat"});
    REQUIRE_THAT(matrix.at(0, 0), WithinRel(1.0, 1e-13));
    require_undefined(matrix.at(1, 1));
}

// --------------------------------------------------------------------------
// Annualisation
// --------------------------------------------------------------------------

// Annualising intraday returns as though markets trade overnight inflates
// volatility by about 2.3x, and deflates every Sharpe ratio by the same.
TEST_CASE("periods per year respect the session length", "[metrics]") {
    REQUIRE_THAT(infer_periods_per_year(time::kDay), WithinRel(252.0, 1e-12));
    REQUIRE_THAT(infer_periods_per_year(time::kMinute), WithinRel(252.0 * 390.0, 1e-12));
    REQUIRE_THAT(infer_periods_per_year(time::kHour), WithinRel(252.0 * 6.5, 1e-12));
    REQUIRE_THAT(infer_periods_per_year(7 * time::kDay), WithinRel(36.0, 1e-12));

    require_undefined(infer_periods_per_year(0));
    require_undefined(infer_periods_per_year(-1));
}

// --------------------------------------------------------------------------
// Rolling
// --------------------------------------------------------------------------

TEST_CASE("a rolling window reports nothing until it is full", "[metrics][rolling]") {
    RollingWindow window(3);
    REQUIRE_FALSE(window.full());
    require_undefined(window.mean());

    window.push(1.0);
    window.push(2.0);
    require_undefined(window.mean());

    window.push(3.0);
    REQUIRE(window.full());
    REQUIRE_THAT(window.mean(), WithinRel(2.0, 1e-15));

    // It slides: 2, 3, 4.
    window.push(4.0);
    REQUIRE_THAT(window.mean(), WithinRel(3.0, 1e-15));
    REQUIRE(window.values() == std::vector<double>{2.0, 3.0, 4.0});
}

TEST_CASE("rolling series keep the input length", "[metrics][rolling]") {
    const std::vector<double> values{1, 2, 3, 4, 5};
    const std::vector<double> means = rolling_mean(view(values), 3);

    REQUIRE(means.size() == values.size());
    require_undefined(means[0]);
    require_undefined(means[1]);
    REQUIRE_THAT(means[2], WithinRel(2.0, 1e-15));  // (1+2+3)/3
    REQUIRE_THAT(means[3], WithinRel(3.0, 1e-15));  // (2+3+4)/3
    REQUIRE_THAT(means[4], WithinRel(4.0, 1e-15));  // (3+4+5)/3
}

TEST_CASE("a window longer than the series yields nothing", "[metrics][rolling]") {
    const std::vector<double> values{1, 2, 3};
    const std::vector<double> means = rolling_mean(view(values), 10);
    REQUIRE(means.size() == 3);
    for (const double value : means) {
        require_undefined(value);
    }
}

TEST_CASE("the last rolling value matches the whole-series value", "[metrics][rolling]") {
    const std::vector<double> returns{0.02, 0.01, 0.03, 0.00, -0.01, 0.02};

    const std::vector<double> sharpes =
        rolling_sharpe(view(returns), returns.size(), Annualisation{});
    REQUIRE_THAT(sharpes.back(), WithinRel(sharpe(view(returns), Annualisation{}), 1e-13));

    const std::vector<double> deviations = rolling_stddev(view(returns), returns.size());
    REQUIRE_THAT(deviations.back(), WithinRel(stddev(view(returns)), 1e-13));
}

TEST_CASE("rolling beta recovers an exact slope", "[metrics][rolling]") {
    const std::vector<double> benchmark{0.01, 0.02, -0.01, 0.03, -0.02, 0.01};
    std::vector<double> returns;
    for (const double value : benchmark) {
        returns.push_back(1.5 * value);
    }

    const std::vector<double> betas = rolling_beta(view(returns), view(benchmark), 4);
    REQUIRE(betas.size() == 6);
    require_undefined(betas[2]);
    REQUIRE_THAT(betas[3], WithinRel(1.5, 1e-12));
    REQUIRE_THAT(betas.back(), WithinRel(1.5, 1e-12));
}

TEST_CASE("rolling correlation tracks a changing relationship", "[metrics][rolling]") {
    // Moves with the benchmark for the first half, against it for the second.
    const std::vector<double> benchmark{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<double> returns{1, 2, 3, 4, -5, -6, -7, -8};

    const std::vector<double> correlations = rolling_correlation(view(returns), view(benchmark), 4);
    REQUIRE_THAT(correlations[3], WithinRel(1.0, 1e-12));
    REQUIRE_THAT(correlations.back(), WithinRel(-1.0, 1e-12));
}

TEST_CASE("relative strength normalises both series", "[metrics][rolling]") {
    // Different starting levels, so an un-normalised ratio would answer a
    // different question.
    const std::vector<double> levels{100, 110, 121};
    const std::vector<double> benchmark{50, 50, 55};

    const std::vector<double> strength = relative_strength(view(levels), view(benchmark));
    REQUIRE(strength.size() == 3);
    REQUIRE_THAT(strength[0], WithinRel(1.0, 1e-15));
    REQUIRE_THAT(strength[1], WithinRel(1.10, 1e-14));  // +10% vs flat
    REQUIRE_THAT(strength[2], WithinRel(1.10, 1e-14));  // +21% vs +10%
}

// --------------------------------------------------------------------------
// The summary
// --------------------------------------------------------------------------

TEST_CASE("a summary of a known curve", "[metrics][summary]") {
    // Daily samples, doubling over four days. Total return is exactly 100%.
    const std::vector<EquityPoint> equity = curve({100, 120, 60, 80, 200});

    Annualisation daily;
    daily.periods_per_year = 252.0;
    const PerformanceSummary summary = summarise(equity, daily);

    REQUIRE(summary.samples == 5);
    REQUIRE_THAT(summary.starting_equity, WithinRel(100.0, 1e-12));
    REQUIRE_THAT(summary.final_equity, WithinRel(200.0, 1e-12));
    REQUIRE_THAT(summary.total_return, WithinRel(1.0, 1e-12));
    REQUIRE_THAT(summary.max_drawdown, WithinRel(0.5, 1e-12));
    REQUIRE(summary.span == 4 * time::kDay);
    REQUIRE(is_defined(summary.sharpe));
    REQUIRE(is_defined(summary.volatility));

    // No benchmark was given, so nothing relative to one is claimed.
    require_undefined(summary.beta);
    require_undefined(summary.alpha);
    require_undefined(summary.information_ratio);
}

TEST_CASE("a summary against a benchmark", "[metrics][summary]") {
    const std::vector<EquityPoint> benchmark = curve({100, 110, 105, 115, 120});
    // Exactly twice the benchmark's return each period, starting from the same
    // level, so beta is 2 and alpha is near zero.
    std::vector<double> levels{100.0};
    const std::vector<double> benchmark_levels{100, 110, 105, 115, 120};
    for (std::size_t i = 1; i < benchmark_levels.size(); ++i) {
        const double benchmark_return = benchmark_levels[i] / benchmark_levels[i - 1] - 1.0;
        levels.push_back(levels.back() * (1.0 + 2.0 * benchmark_return));
    }

    Annualisation daily;
    daily.periods_per_year = 252.0;
    const PerformanceSummary summary = summarise(curve(levels), benchmark, daily);

    // Not machine precision, and the reason is worth stating: equity is exact
    // integer micro-dollars, so a curve built here from exact ratios is
    // quantised to the nearest micro-dollar before any of this is computed.
    // On a 100-unit account that is a relative error of about 1e-8, and the
    // regression recovers the slope to the same order. The tolerance is set to
    // the accuracy the ledger's representation actually permits rather than to
    // the accuracy doubles would.
    REQUIRE_THAT(summary.beta, WithinRel(2.0, 1e-6));
    REQUIRE_THAT(summary.alpha, WithinAbs(0.0, 1e-5));
    REQUIRE_THAT(summary.r_squared, WithinRel(1.0, 1e-9));
    REQUIRE_THAT(summary.correlation, WithinRel(1.0, 1e-9));
}

TEST_CASE("a summary of too few points claims nothing", "[metrics][summary]") {
    const PerformanceSummary empty = summarise(std::span<const EquityPoint>{});
    REQUIRE(empty.samples == 0);
    require_undefined(empty.sharpe);
    require_undefined(empty.total_return);

    const std::vector<EquityPoint> single = curve({100});
    const PerformanceSummary one = summarise(single);
    REQUIRE(one.samples == 1);
    require_undefined(one.sharpe);
}

TEST_CASE("a summary infers the sampling rate from the curve", "[metrics][summary]") {
    const std::vector<EquityPoint> daily = curve({100, 101, 102, 103}, time::kDay);
    const std::vector<EquityPoint> minutely = curve({100, 101, 102, 103}, time::kMinute);

    // Same returns, sampled at different rates. The minute-sampled series
    // annualises to a much larger volatility, which is the whole reason the
    // rate has to be inferred rather than assumed.
    Annualisation infer;
    infer.periods_per_year = 0.0;
    REQUIRE(summarise(minutely, infer).volatility > summarise(daily, infer).volatility);
}

// --------------------------------------------------------------------------
// Constant series, at length
// --------------------------------------------------------------------------

// Regression tests for a bug the quickstart notebook found.
//
// Testing `sigma <= 0` is not enough. Summing a hundred copies of 0.01 does not
// give exactly a hundred times 0.01: the accumulated rounding leaves the mean a
// few parts in 10^18 from the value, the deviations are ~1e-18 rather than 0,
// and a Sharpe ratio computed from that comes out at 2e16. A strategy that
// never moved then sits at the top of the leaderboard with an astronomical
// score -- the exact outcome the zero check was written to prevent.
//
// Four identical values happen to sum exactly, which is why the original test
// passed and this one is written with a hundred.
TEST_CASE("a long constant series has no Sharpe ratio", "[metrics][regression]") {
    for (const std::size_t count : {4U, 10U, 100U, 1'000U, 10'000U}) {
        const std::vector<double> flat(count, 0.01);
        INFO("series length " << count);
        require_undefined(sharpe(view(flat), Annualisation{}));
    }
}

TEST_CASE("a long constant series has no Sortino ratio", "[metrics][regression]") {
    const std::vector<double> flat(1'000, 0.01);
    require_undefined(sortino(view(flat), Annualisation{}));
}

TEST_CASE("a long constant series correlates with nothing", "[metrics][correlation][regression]") {
    const std::vector<double> flat(1'000, 0.01);
    std::vector<double> moving;
    moving.reserve(1'000);
    for (std::size_t i = 0; i < 1'000; ++i) {
        moving.push_back(static_cast<double>(i) * 0.001);
    }

    require_undefined(correlation(view(flat), view(moving)));
    require_undefined(correlation(view(flat), view(flat)));
}

TEST_CASE("a long constant benchmark cannot be regressed against", "[metrics][regression]") {
    const std::vector<double> flat(1'000, 0.01);
    std::vector<double> returns;
    returns.reserve(1'000);
    for (std::size_t i = 0; i < 1'000; ++i) {
        returns.push_back(0.001 * static_cast<double>(i % 7) - 0.002);
    }

    const Regression regression = regress(view(returns), view(flat), Annualisation{});
    require_undefined(regression.beta);
    require_undefined(regression.alpha);
}

TEST_CASE("tracking a benchmark exactly gives no information ratio", "[metrics][regression]") {
    std::vector<double> series;
    series.reserve(1'000);
    for (std::size_t i = 0; i < 1'000; ++i) {
        series.push_back(0.0005 * static_cast<double>(i % 11));
    }
    require_undefined(information_ratio(view(series), view(series), Annualisation{}));
}

// The guard must not swallow a real, small volatility. A series whose moves are
// a thousandth of its level is quiet, not constant.
TEST_CASE("a genuinely small volatility still has a Sharpe ratio", "[metrics][regression]") {
    std::vector<double> returns;
    returns.reserve(1'000);
    for (std::size_t i = 0; i < 1'000; ++i) {
        returns.push_back(0.01 + (i % 2 == 0 ? 1e-5 : -1e-5));
    }

    const double value = sharpe(view(returns), Annualisation{});
    REQUIRE(is_defined(value));
    REQUIRE(std::fabs(value) > 0.0);
}
